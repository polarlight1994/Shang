//===----- VASTScheduling.cpp - Scheduling Graph on VAST  -------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VASTSUnit class, which represents the elemental
// scheduling unit in VAST.
//
//===----------------------------------------------------------------------===//
//

#include "TimingNetlist.h"
#include "VASTScheduling.h"
#include "SDCScheduler.h"
#include "ScheduleDOT.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTModulePass.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Support/CFG.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-scheduling-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumMemDep, "Number of Memory Dependencies Added");
STATISTIC(NumForceBrSync, "Number of Dependencies add to sync the loop exit");

//===----------------------------------------------------------------------===//
VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, Instruction *Inst, bool IsLatch,
                             BasicBlock *BB, VASTSeqOp *SeqOp)
  : Schedule(0),  InstIdx(InstIdx), Ptr(Inst), BB(BB, IsLatch), SeqOp(SeqOp) {}

VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, BasicBlock *BB)
  : Schedule(0),  InstIdx(InstIdx), Ptr(BB), BB(0, false), SeqOp(0) {}

VASTSchedUnit::VASTSchedUnit()
  : Schedule(0), InstIdx(0), Ptr(reinterpret_cast<BasicBlock*>(-1024)),
    BB(0), SeqOp(0)
{}

void VASTSchedUnit::EdgeBundle::addEdge(VASTDep NewEdge) {
 assert(NewEdge.getEdgeType() != VASTDep::FixedTiming
        && "Fixed timing constraint cannot be added!");
  unsigned InsertBefore = 0, Size = Edges.size();
  bool NeedToInsert = true;

  VASTDep::Types NewEdgeType = NewEdge.getEdgeType();
  unsigned NewDistance = NewEdge.getDistance();
  int      NewLatency = NewEdge.getLatency();

  while (InsertBefore < Size) {
    VASTDep &CurEdge = Edges[InsertBefore];
    // Keep the edges in ascending order.
    if (CurEdge.getDistance() > NewEdge.getDistance())
      break;

    VASTDep::Types CurEdgeType = CurEdge.getEdgeType();
    unsigned CurDistance = CurEdge.getDistance();
    int      CurLatency = CurEdge.getLatency();

    // Update the edge with the tighter constraint.
    if (CurDistance == NewDistance && CurLatency < NewLatency) {
      if (NewEdgeType == CurEdgeType) {
        NeedToInsert = false;
        CurEdge = NewEdge;
      }

      break;
    }

    // Now we have CurDistance < NewDistance, NewEdge is masked by CurEdge if
    // NewEdge has a smaller latency than CurEdge.
    if (NewLatency <= CurLatency && NewEdgeType == CurEdgeType) return;

    ++InsertBefore;
  }

  assert((InsertBefore == Edges.size()
          || (Edges[InsertBefore].getLatency() <= NewEdge.getLatency()
              && Edges[InsertBefore].getDistance() >= NewEdge.getDistance()))
         && "Bad insert position!");

  // Insert the new edge right before the edge with bigger iterative distance.
  if (NeedToInsert) Edges.insert(Edges.begin() + InsertBefore, NewEdge);
}

VASTDep VASTSchedUnit::EdgeBundle::getEdge(unsigned II) const {
  assert(Edges.size() && "Unexpected empty edge bundle!");
  VASTDep CurEdge = Edges.front();
  int Latency = CurEdge.getLatency(II);

  // Zero II means we should ignore the loop-carried dependencies.
  // if (II == 0) return *CurEdge;

  for (unsigned i = 1, e = Edges.size(); i != e; ++i) {
    VASTDep Edge = Edges[i];

    // Find the edge with biggest latency.
    int NewLatency = Edge.getLatency(II);
    if (NewLatency > Latency) {
      Latency = NewLatency;
      CurEdge = Edge;
    }
  }

  return CurEdge;
}

BasicBlock *VASTSchedUnit::getParent() const {
  assert(!isEntry() && !isExit() && "Call getParent on the wrong SUnit type!");

  if (BasicBlock *BB = Ptr.dyn_cast<BasicBlock*>())
    return BB;

  if (isa<PHINode>(getInst()) && isLatch()) return getIncomingBlock();

  return Ptr.get<Instruction*>()->getParent();
}

void VASTSchedUnit::scheduleTo(unsigned Step) {
  assert(Step && "Bad schedule!");
  Schedule = Step;
}

void VASTSchedUnit::resetSchedule() {
  Schedule = 0;
}

void VASTSchedUnit::print(raw_ostream &OS) const {
  if (isEntry()) {
    OS << "Entry Node";
    return;
  }

  if (isExit()) {
    OS << "Exit Node";
    return;
  }

  OS << '#' << InstIdx << ' ' << (isLaunch() ? "Launch" : "Latch");

  if (BasicBlock *BB = Ptr.dyn_cast<BasicBlock*>())
    OS << " BB: " << BB->getName();
  else {
    Instruction *Inst = Ptr.get<Instruction*>();
    OS << *Inst;

    if (isa<PHINode>(Inst) && isLatch())
      OS << " From: " << getIncomingBlock()->getName();

    if (isTerminator())
      if (BasicBlock *BB =  getTargetBlock())
        OS << " Targeting: " << BB->getName();
  }

  OS << " Scheduled to " << Schedule;
}

void VASTSchedUnit::viewNeighbourGraph() {
  SchedGraphWrapper W(this);
  W.SUs.insert(dep_begin(), dep_end());
  W.SUs.insert(use_begin(), use_end());
  ViewGraph(&W, "NeighbourGraph");
}

void VASTSchedUnit::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

//===----------------------------------------------------------------------===//
VASTSchedGraph::VASTSchedGraph(Function &F) : F(F) {
  // Create the entry SU.
  SUnits.push_back(new VASTSchedUnit(0, reinterpret_cast<BasicBlock*>(0)));

  // Create the exit SU.
  SUnits.push_back(new VASTSchedUnit(-1, reinterpret_cast<Instruction*>(0),
                                     false, 0, 0));
}

VASTSchedGraph::~VASTSchedGraph() {}

void VASTSchedGraph::resetSchedule() {
  for (iterator I = begin(), E = end(); I != E; ++I)
    I->resetSchedule();

  getEntry()->scheduleTo(1);
}

void VASTSchedGraph::viewGraph() {
  ViewGraph(this, "SchedulingGraph");
}

void VASTSchedGraph::verify() const {
  if (getEntry()->use_empty() && !getEntry()->dep_empty())
    llvm_unreachable("Broken dependencies on Entry!");

  if (getExit()->dep_empty() && !getExit()->use_empty())
    llvm_unreachable("Broken dependencies on Exit!");

  for (const_iterator I = llvm::next(SUnits.begin()), E = SUnits.back();
       I != E; ++I)
    if (I->dep_empty() || I->use_empty())
      llvm_unreachable("Broken dependencies!");
}

void VASTSchedGraph::prepareForScheduling() {
  getEntry()->scheduleTo(1);

  getExit()->InstIdx = size();
}

void VASTSchedGraph::topologicalSortSUs() {
  for (po_iterator<VASTSchedUnit*> I = po_begin(getEntry()),
       E = po_end(getEntry()); I != E; ++I) {
    VASTSchedUnit *U = *I;
    SUnits.splice(SUnits.begin(), SUnits, U);
  }

  unsigned Idx = 0;
  for (iterator I = begin(), E = end(); I != E; ++I)
    I->InstIdx = Idx++;

  assert(getEntry()->isEntry() && getExit()->isExit() && "Broken TopSort!");
}

//===----------------------------------------------------------------------===//
void VASTSchedGraph::print(raw_ostream &OS) const {
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    I->print(OS);
    OS << '\n';
  }
}

void VASTSchedGraph::dump() const {
  print(dbgs());
}

MutableArrayRef<VASTSchedUnit*> VASTSchedGraph::getSUInBB(BasicBlock *BB) {
  bb_iterator at = BBMap.find(BB);

  assert(at != BBMap.end() && "BB not found!");

  return MutableArrayRef<VASTSchedUnit*>(at->second);
}


ArrayRef<VASTSchedUnit*> VASTSchedGraph::getSUInBB(BasicBlock *BB) const {
  const_bb_iterator at = BBMap.find(BB);

  assert(at != BBMap.end() && "BB not found!");

  return ArrayRef<VASTSchedUnit*>(at->second);
}

//===----------------------------------------------------------------------===//
namespace {
struct VASTScheduling : public VASTModulePass {
  static char ID;
  typedef std::map<Value*, SmallVector<VASTSchedUnit*, 4> > IR2SUMapTy;
  IR2SUMapTy IR2SUMap;
  typedef std::map<Argument*, VASTSeqValue*> ArgMapTy;
  ArgMapTy ArgMap;

  VASTSchedGraph *G;
  TimingNetlist *TNL;
  VASTModule *VM;
  DependenceAnalysis *DA;
  LoopInfo *LI;

  VASTScheduling() : VASTModulePass(ID) {
    initializeVASTSchedulingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredID(BasicBlockTopOrderID);
    AU.addRequiredID(DatapathNamerID);
    AU.addRequired<TimingNetlist>();
    AU.addRequired<DependenceAnalysis>();
    AU.addRequired<LoopInfo>();
  }

  VASTSchedUnit *getOrCreateBBEntry(BasicBlock *BB);

  void buildFlowDependencies(VASTValue *Dst, VASTSeqValue *Src,
                             VASTSchedUnit *U, unsigned ExtraDelay);
  unsigned buildFlowDependencies(VASTSeqOp *Op, VASTSchedUnit *U);
  unsigned buildFlowDependencies(VASTSchedUnit *U);
  unsigned buildFlowDependenciesForSlotCtrl(VASTSchedUnit *U);
  VASTSchedUnit *getFlowDepSU(Value *V);

  void buildMemoryDependencies(Instruction *Src, Instruction *Dst);
  void buildLoopDependencies(Instruction *Src, Instruction *Dst);
  void buildMemoryDependencies(BasicBlock *BB);

  void fixSchedulingGraph();

  void buildSchedulingGraph();
  void buildSchedulingUnits(VASTSlot *S);

  void scheduleGlobal();

  bool runOnVASTModule(VASTModule &VM);

  void releaseMemory() {
    IR2SUMap.clear();
    ArgMap.clear();
    G = 0;
    TNL = 0;
  }
};
}

char VASTScheduling::ID = 0;

INITIALIZE_PASS_BEGIN(VASTScheduling,
                      "vast-scheduling", "Perfrom Scheduling on the VAST",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
  INITIALIZE_PASS_DEPENDENCY(BasicBlockTopOrder)
  INITIALIZE_PASS_DEPENDENCY(DependenceAnalysis)
  INITIALIZE_PASS_DEPENDENCY(LoopInfo);
INITIALIZE_PASS_END(VASTScheduling,
                    "vast-scheduling", "Perfrom Scheduling on the VAST",
                    false, true)

Pass *llvm::createVASTSchedulingPass() {
  return new VASTScheduling();
}

template<typename T>
static T *check(T *X) {
  assert(X && "Unexpected NULL ptr!");
  return X;
}

VASTSchedUnit *VASTScheduling::getFlowDepSU(Value *V) {
  bool IsPHI = isa<PHINode>(V);


  if (isa<Argument>(V)) return G->getEntry();

  IR2SUMapTy::const_iterator at = IR2SUMap.find(V);
  assert(at != IR2SUMap.end() && "Flow dependencies missed!");

  // Get the corresponding latch SeqOp.
  ArrayRef<VASTSchedUnit*> SUs(at->second);
  VASTSeqValue *SrcSeqVal = 0;
  for (unsigned i = 0; i < SUs.size(); ++i) {
    VASTSchedUnit *CurSU = SUs[i];
    // Are we got the VASTSeqVal corresponding to V?
    if (CurSU->isLatching(V)) {
      assert((SrcSeqVal == 0
              || SrcSeqVal == check(CurSU->getSeqOp())->getDef(0).getDst())
            && "All PHI latching SeqOp should define the same SeqOp!");
      SrcSeqVal = check(CurSU->getSeqOp())->getDef(0).getDst();

      if (IsPHI) continue;

      // We are done if we are looking for the Scheduling Unit for common
      // instruction.
      return CurSU;
    }

    if (IsPHI && CurSU->isPHI()) return CurSU;
  }

  (void) SrcSeqVal;

  llvm_unreachable("No source SU?");
  return 0;
}

void VASTScheduling::buildFlowDependencies(VASTValue *Dst, VASTSeqValue *Src,
                                           VASTSchedUnit *U, unsigned ExtraDelay)
{
  VASTLatch L = Src->latchFront();
  Value *V = L.Op->getValue();
  assert(V && "Cannot get the corresponding value!");
  assert((Src->size() == 1 || isa<PHINode>(V)) && "SeqVal not in SSA!");
  VASTSchedUnit *SrcSU
      // The static register is virtually defined at the entry slot. Because
      // we only write it when the function exit. Whe we read is the value from
      // last function execution.
    = Src->getValType() == VASTSeqValue::StaticRegister ? G->getEntry()
                                                        : getFlowDepSU(V);

  unsigned NumCylces = Src == Dst ? 0 : TNL->getDelay(Src, Dst).getNumCycles();

  U->addDep(SrcSU, VASTDep::CreateFlowDep(NumCylces + ExtraDelay));
}

unsigned VASTScheduling::buildFlowDependencies(VASTSeqOp *Op, VASTSchedUnit *U) {
  std::set<VASTSeqValue*> Srcs;
  typedef std::set<VASTSeqValue*>::iterator iterator;
  unsigned MuxDelay = 0;

  assert(Op->getNumSrcs() && "No operand for flow dependencies!");

  for (unsigned i = 0, e = Op->getNumSrcs(); i != e; ++i) {
    VASTLatch L = Op->getSrc(i);
    VASTValue *FI = VASTValPtr(L).get();
    VASTSeqValue *Dst = L.getDst();

    FI->extractSupporingSeqVal(Srcs);
    // The Srcs set will be empty if FI is not a constant.
    if (Srcs.empty()) continue;

    unsigned CurMuxDelay
      = TNL->getMuxDelay(Dst->size(), Dst->getBitWidth()).getNumCycles();
    MuxDelay = std::max(MuxDelay, CurMuxDelay);

    for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
      buildFlowDependencies(FI, *I, U, CurMuxDelay);

    Srcs.clear();
  }

  VASTValue *V = VASTValPtr(Op->getPred()).get();
  V->extractSupporingSeqVal(Srcs);
  for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
    buildFlowDependencies(V, *I, U, MuxDelay);

  // There is originally 1 cycle available for the selector mux, we need to
  // return the extra mux delay required by the selector mux.
  return std::max(int(MuxDelay) - 1, 0);
}

unsigned VASTScheduling::buildFlowDependenciesForSlotCtrl(VASTSchedUnit *U) {
  VASTSlotCtrl *SlotCtrl = cast<VASTSlotCtrl>(U->getSeqOp());
  std::set<VASTSeqValue*> Srcs;
  
  VASTValue *V = VASTValPtr(SlotCtrl->getPred()).get();
  V->extractSupporingSeqVal(Srcs);

  unsigned NumFIs = SlotCtrl->getTargetSlot()->pred_size();
  unsigned MuxDelay = TNL->getMuxDelay(NumFIs, 1).getNumCycles();

  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
    buildFlowDependencies(V, *I, U, MuxDelay);

  // There is originally 1 cycle available for the selector mux, we need to
  // return the extra mux delay required by the selector mux.
  return std::max(int(MuxDelay) - 1, 0);
}

unsigned VASTScheduling::buildFlowDependencies(VASTSchedUnit *U) {
  Instruction *Inst = U->getInst();

  if (U->isLaunch())
    return buildFlowDependencies(U->getSeqOp(), U);

  assert(U->isLatch() && "Unexpected scheduling unit type!");

  if (isa<PHINode>(Inst))
    return buildFlowDependencies(U->getSeqOp(), U);

  if (isa<TerminatorInst>(Inst))
    return buildFlowDependencies(U->getSeqOp(), U);

  VASTSeqInst *SeqInst = cast<VASTSeqInst>(U->getSeqOp());
  unsigned Latency = SeqInst->getCyclesFromLaunch();
  // This is a lowered signle-element- block RAM access.
  if (Latency == 0) {
    assert(isa<StoreInst>(Inst)
           && isa<GlobalVariable>(cast<StoreInst>(Inst)->getPointerOperand())
           && "Zero latency latching is not allowed!");
    return buildFlowDependencies(U->getSeqOp(), U);
  }

  // Simply build the dependencies from the launch instruction.
  SmallVectorImpl<VASTSchedUnit*> &SUs = IR2SUMap[Inst];
  assert(SUs.size() >= 1 && "Launching SU not found!");
  VASTSchedUnit *LaunchU = SUs.front();
  assert(LaunchU->isLaunch() && "Bad SU type!");

  U->addDep(LaunchU, VASTDep::CreateFixTimingConstraint(Latency));
  return 0;
}

VASTSchedUnit *VASTScheduling::getOrCreateBBEntry(BasicBlock *BB) {
  SmallVectorImpl<VASTSchedUnit*> &SUs = IR2SUMap[BB];

  // Simply return the BBEntry if it had already existed.
  if (!SUs.empty() && SUs.back()->isBBEntry())
    return SUs.back();

  VASTSchedUnit *Entry = G->createSUnit(BB);

  // Add the conditional dependencies from the branch instructions that
  // targeting this BB.
  // Please note that because we are visiting the BBs in topological order,
  // we are supposed to not introduce backedges here.
  for (unsigned i = 0; i < SUs.size(); ++i) {
    assert(!SUs[i]->isBBEntry() && "Unexpected BB entry!");
    assert(isa<TerminatorInst>(SUs[i]->getInst())
           && "Unexpected instruction type!");
    assert(SUs[i]->getTargetBlock() == BB && "Wrong target BB!");
    Entry->addDep(SUs[i], VASTDep::CreateCndDep());
  }

  // Also constraint the BB entry by the entry of the whole scheduling graph.
  // But this is actually not needed because we will set a lower bound on the
  // scheduling variable in the SDC scheduler.
  //Entry->addDep(G->getEntry(), VASTDep::CreateCtrlDep(0));

  if (SUs.empty()) {
    assert(pred_begin(BB) == pred_end(BB)
           && "No entry block do not have any predecessor?");
    // Dependency from the BB entry is not conditional.
    Entry->addDep(G->getEntry(), VASTDep::CreateFlowDep(0));
  }

  // Add the entry to the mapping.
  SUs.push_back(Entry);

  // Also create the SUnit for the PHI Nodes.
  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(), E = BB->getFirstNonPHI(); I != E; ++I) {
    PHINode *PN = cast<PHINode>(I);
    VASTSchedUnit *U = G->createSUnit(PN, false, 0, 0);

    // Schedule the PHI to the same slot with the entry if we are not perform
    // Software pipelining.
    U->addDep(Entry, VASTDep::CreateFixTimingConstraint(0));

    // Add the dependencies from the incoming values.
    SmallVectorImpl<VASTSchedUnit*> &Incomings = IR2SUMap[PN];
    for (unsigned i = 0; i < Incomings.size(); ++i) {
      VASTSchedUnit *Incoming = Incomings[i];
      assert(Incoming->isLatch() && "Bad incoming scheduling unit type!");
      U->addDep(Incoming, VASTDep::CreateCndDep());
    }

    Incomings.push_back(U);
  }

  return Entry;
}

void VASTScheduling::buildSchedulingUnits(VASTSlot *S) {
  typedef VASTSlot::op_iterator op_iterator;

  BasicBlock *BB = S->getParent();

  // If the BB is NULL, this slot should be the entry or the exit of the
  // state-transition graph.
  VASTSchedUnit *BBEntry = 0;
  if (BB == 0) BBEntry = G->getEntry();
  else         BBEntry = getOrCreateBBEntry(BB);

  for (op_iterator OI = S->op_begin(), OE = S->op_end(); OI != OE; ++OI) {
    VASTSeqOp *Op = *OI;
    Instruction *Inst = dyn_cast_or_null<Instruction>(Op->getValue());
    // We can safely ignore the SeqOp that does not correspond to any LLVM
    // Value, their will be rebuilt when we emit the scheduling.
    if (Inst == 0) {
      if (BB == 0) {
        if (Argument *Arg = dyn_cast_or_null<Argument>(Op->getValue())) {
          // Remember the corresponding SeqVal.
          bool inserted
            = ArgMap.insert(std::make_pair(Arg, Op->getDef(0).getDst())).second;
          assert(inserted && "SeqVal for argument not inserted!");
          (void) inserted;
        }
      }

      continue;
    }

    if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(Op)) {
      VASTSchedUnit *U = 0;
      bool IsLatch = SeqInst->getSeqOpType() == VASTSeqInst::Latch;

      if (PHINode *PN = dyn_cast<PHINode>(Inst)) {
        U = G->createSUnit(PN, IsLatch, BB, SeqInst);

        BasicBlock *LandingBlock = PN->getParent();
        ArrayRef<VASTSchedUnit*> Terminators(IR2SUMap[BB->getTerminator()]);
        for (unsigned i = 0; i < Terminators.size(); ++i) {
          VASTSchedUnit *T = Terminators[i];
          if (T->getTargetBlock() == LandingBlock) {
            // Schedule the incoming copy of the PHIs to the same slot that
            // the branch instruction branching to the same BB.
            U->addDep(T, VASTDep::CreateFixTimingConstraint(0));
            break;
          }
        }
        assert(!U->dep_empty()
               && "PHI not bind to the Branch that targeting the same block!");
      } else
        U = G->createSUnit(Inst, IsLatch, 0, SeqInst);

      unsigned ExtraMuxDelay = buildFlowDependencies(U);
      U->addDep(BBEntry, VASTDep::CreateCtrlDep(ExtraMuxDelay));
      IR2SUMap[Inst].push_back(U);
      continue;
    }

    if (VASTSlotCtrl *SlotCtrl = dyn_cast<VASTSlotCtrl>(Op)) {
      if (SlotCtrl->isBranch()) {
        // Handle the branch.
        BasicBlock *TargetBB = SlotCtrl->getTargetSlot()->getParent();
        VASTSchedUnit *U = G->createSUnit(Inst, true, TargetBB, SlotCtrl);
        IR2SUMap[Inst].push_back(U);
        // Also map the target BB to this terminator.
        IR2SUMap[TargetBB].push_back(U);

        unsigned ExtraMuxDelay = buildFlowDependenciesForSlotCtrl(U);
        // add extra dele
        U->addDep(BBEntry, VASTDep::CreateCtrlDep(ExtraMuxDelay));
        continue;
      }

      // This is a wait operation.
      VASTSchedUnit *U = G->createSUnit(Inst, true, BB, SlotCtrl);
      IR2SUMap[Inst].push_back(U);
      VASTSchedUnit *Launch = IR2SUMap[Inst].front();
      assert(Launch->isLaunch() && "Expect launch operation!");
      // The wait operation is 1 cycle after the launch operation.
      U->addDep(Launch, VASTDep::CreateFixTimingConstraint(1));
      continue;
    }
  }
}

//===----------------------------------------------------------------------===//
void VASTScheduling::buildMemoryDependencies(Instruction *Src, Instruction *Dst)
{
  // The control flow can reach from Src to Dst without traversing a loop back
  // edge.
  Dependence *D = DA->depends(Src, Dst, true);

  // No dependencies at all.
  if ((D == 0 || D->isInput()) && !isCall(Src) && !isCall(Dst))
    return;

  VASTSchedUnit *SrcU = IR2SUMap[Src].front(), *DstU = IR2SUMap[Dst].front();
  assert(SrcU->isLaunch() && DstU->isLaunch() && "Bad scheduling unit type!");

  unsigned Latency = 1;

  // We must flush the memory bus pipeline before starting the call.
  if (isa<CallInst>(Dst)) {
    VASTSchedUnit *SrcLatch = IR2SUMap[Src].back();
    // Make the call dependence on the latch operation instead.
    if (SrcLatch->isLatch()) {
      SrcU = SrcLatch;
      Latency = 0;
    }
  }

  DstU->addDep(SrcU, VASTDep::CreateMemDep(Latency, 0));
  ++NumMemDep;
}

void VASTScheduling::buildMemoryDependencies(BasicBlock *BB) {
  typedef BasicBlock::iterator iterator;
  SmallVector<Instruction*, 16> PiorMemInsts;

  for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
    Instruction *Inst = I;

    if (!isLoadStore(Inst) && !isCall(Inst)) continue;

    // The load/store to single element block RAM will be lowered to register
    // access by the VASTModuleBuilder.
    if (!IR2SUMap.count(Inst) || IR2SUMap[Inst].front()->isLatch()) {
      DEBUG(dbgs() << "Ignore " << *Inst << " in dependencies graph\n");
      continue;
    }

    for (unsigned i = 0, e = PiorMemInsts.size(); i < e; ++i)
      buildMemoryDependencies(PiorMemInsts[i], Inst);

    PiorMemInsts.push_back(Inst);
  }
}

//===----------------------------------------------------------------------===//
void VASTScheduling::fixSchedulingGraph() {
  // Try to fix the dangling nodes.
  typedef VASTSchedGraph::iterator iterator;
  for (iterator I = llvm::next(G->begin()), E = G->getExit(); I != E; ++I) {
    VASTSchedUnit *U = I;
    // Terminators will be handled later.
    if (U->isTerminator()) continue;

    BasicBlock *BB = U->getParent();

    // Constrain the SU by the exit of the same BB if it is not constrained yet.
    // bool ConstrainedByExit = false;

    //typedef VASTSchedUnit::use_iterator use_iterator;
    //for (use_iterator UI = U->use_begin(), UE = U->use_end(); UI != UE; ++UI) {
    //  VASTSchedUnit *User = *UI;
    //  if ((ConstrainedByExit = /*Assignment*/(User->getParent() == BB)))
    //    break;
    //}

    //if (ConstrainedByExit) continue;

    // Always constraints the latch with the exit SUs.
    // if (U->isLaunch()) continue;

    // Constrain the dangling nodes by all terminators.
    ArrayRef<VASTSchedUnit*> Exits(IR2SUMap[BB->getTerminator()]);
    for (unsigned i = 0; i < Exits.size(); ++i) {
      VASTSchedUnit *BBExit = Exits[i];
      // Ignore the return value latching operation here. We will add the fix
      // timing constraints between it and the actual terminator.
      if (!BBExit->isTerminator()) {
        assert(isa<ReturnInst>(BB->getTerminator())
               && "BBExit is not terminator!");
        // We need to add the dependencies even the BB return is not a
        // terminator. But becareful, do not add the cycle dependencies.
        if (BBExit == U) continue;
      }

      // The SU maybe a PHI incoming copy targeting a back edge.
      if (BBExit->getIdx() < U->getIdx()) {
        assert(isa<PHINode>(U->getInst()) && "Unexpected instruction type!");
        // Create the pseudo dependencies to the exit node.
        G->getExit()->addDep(U, VASTDep::CreateCtrlDep(0));
        continue;
      }

      // Allocate 1 cycles for the scheduling units that launching some operations.
      unsigned Latency = U->isLatch() ? 0 : 1;
      BBExit->addDep(U, VASTDep::CreateCtrlDep(Latency));
    }
  }

  // Also add the dependencies form the return instruction to the exit of
  // the scheduling graph.
  Function &F = *VM;
  bool AnyLinearOrder = false;

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    TerminatorInst *Inst = I->getTerminator();

    if ((isa<UnreachableInst>(Inst) || isa<ReturnInst>(Inst))) {
      ArrayRef<VASTSchedUnit*> SUs(IR2SUMap[Inst]);
      assert(!SUs.empty() && "Scheduling Units for terminator not built?");
      VASTSchedUnit *U = SUs[0];
      VASTSchedUnit *LastSU = U;

      for (unsigned i = 1; i < SUs.size(); ++i)
        LastSU = SUs[i];

      G->getExit()->addDep(LastSU, VASTDep::CreateCtrlDep(0));
      continue;
    }

    if (Loop *L = LI->getLoopFor(I)) {
      if (!L->isLoopExiting(I)) continue;

      // Synchronize the terminator of Loop exiting block. Otherwise we need
      // chain breaking.
      ArrayRef<VASTSchedUnit*> SUs(IR2SUMap[Inst]);
      VASTSchedUnit *BackEdgeOp = 0;
      for (unsigned i = 0; i < SUs.size(); ++i) {
        VASTSchedUnit *U = SUs[i];
        assert(U->isTerminator() && "Unexpected SU type!");
        if (U->getTargetBlock() == L->getHeader()) {
          BackEdgeOp = U;
          break;
        }
      }

      if (BackEdgeOp == 0) continue;

      // Add the linear order to make sure the BackEdgeOp is not scheduled
      // earlier than the exiting branch, otherwise the RAW dependency will be
      // broken. We can remove this once we have the chain breaker.
      for (unsigned i = 0; i < SUs.size(); ++i) {
        VASTSchedUnit *U = SUs[i];
        assert(U->isTerminator() && "Unexpected SU type!");
        if (U == BackEdgeOp) continue;
        assert(!U->isDependsOn(BackEdgeOp)
               && "Unexpected dependencies between terminators!");
        BackEdgeOp->addDep(U, VASTDep::CreateDep<VASTDep::LinearOrder>(0));
        AnyLinearOrder = true;
        ++NumForceBrSync;
      }
    }
  }

  // Prevent the scheduler from generating 1 slot loop, that is the loop can be
  // entirely folded into its predecessors. If this happen, the schedule emitter
  // will try to unroll the loop.
  SmallVector<Loop*, 64> Worklist(LI->begin(), LI->end());
  while (!Worklist.empty()) {
    Loop *L = Worklist.pop_back_val();

    // Also push the children of L into the work list.
    if (!L->empty()) Worklist.append(L->begin(), L->end());

    // Build the constraints from the entry to the branch of the backedges.
    BasicBlock *HeaderBB = L->getHeader();
    ArrayRef<VASTSchedUnit*> SUs(IR2SUMap[HeaderBB]);
    VASTSchedUnit *Header = 0;

    // First of all we need to locate the header.
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *SU = SUs[i];
      if (SU->isBBEntry()) {
        Header = SU;
        break;
      }
    }

    // Try to prevent the other part of the loop being folded across the
    // loop header.
    ArrayRef<VASTSchedUnit*> Terminators(IR2SUMap[HeaderBB->getTerminator()]);
    for (unsigned i = 0; i < Terminators.size(); ++i) {
      VASTSchedUnit *Terminator = Terminators[i];
      BasicBlock *TargetBB = Terminator->getTargetBlock();
      // Folding the block that is outside the loop through the header is
      // allowed.
      if (!L->contains(TargetBB)) continue;

      Terminator->addDep(Header, VASTDep::CreateCtrlDep(1));
    }
  }

  // DIRTYHACK: As we add linear order for the from the exiting branch to the
  // backedge branch we may destroy the SU ordering. Fix it by topologicalSortSUs
  if (AnyLinearOrder)  G->topologicalSortSUs();
}

void VASTScheduling::scheduleGlobal() {
  SDCScheduler Scheduler(*G, 1);

  Scheduler.addCFGFoldingConstraints(*TNL);
  Scheduler.addLinOrdEdge();

  // Build the step variables, and no need to schedule at all if all SUs have
  // been scheduled.
  if (Scheduler.createLPAndVariables()) {
    unsigned NumBB = 0;

    Function &F = *VM;
    typedef Function::iterator iterator;
    for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
      BasicBlock *BB = I;

      unsigned NumExits = 0;
      ArrayRef<VASTSchedUnit*> Exits(IR2SUMap[BB->getTerminator()]);
      for (unsigned i = 0; i < Exits.size(); ++i) {
        VASTSchedUnit *BBExit = Exits[i];
        // Ignore the return value latching operation here. We will add the fix
        // timing constraints between it and the actual terminator.
        if (!BBExit->isTerminator()) {
          assert(isa<ReturnInst>(BB->getTerminator()) && "BBExit is not terminator!");
          continue;
        }

        Scheduler.addObjectCoeff(BBExit, -1024.0);

        ++NumExits;
        ++NumBB;
      }

      ArrayRef<VASTSchedUnit*> SUs(IR2SUMap[BB]);
      VASTSchedUnit *BBEntry = 0;

      // First of all we need to locate the header.
      for (unsigned i = 0; i < SUs.size(); ++i) {
        VASTSchedUnit *SU = SUs[i];
        if (SU->isBBEntry()) {
          BBEntry = SU;
          break;
        }
      }

      assert(BBEntry && "Cannot find BB Entry!");

      Scheduler.addObjectCoeff(BBEntry, (NumExits) * 1024.0);
    }

    Scheduler.addObjectCoeff(G->getExit(), -1024.0 * (NumBB + 1));

    bool success = Scheduler.schedule();
    assert(success && "SDCScheduler fail!");
    (void) success;
  }

  DEBUG(G->viewGraph());
}

void VASTScheduling::buildSchedulingGraph() {
  Function &F = *VM;

  // Build the scheduling units according to the original scheduling.
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >
    RPO(VM->getStartSlot());

  typedef
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >::rpo_iterator
  slot_top_iterator;

  for (slot_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    buildSchedulingUnits(*I);

  // Build the memory dependencies.
  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I)
    buildMemoryDependencies(I);

  // Constraint all nodes that do not have a user by the terminator in its parent
  // BB.
  fixSchedulingGraph();

#ifndef NDEBUG
  G->verify();
#endif

  DEBUG(G->viewGraph());
}

bool VASTScheduling::runOnVASTModule(VASTModule &VM) {
  this->VM = &VM;

  OwningPtr<VASTSchedGraph> GPtr(new VASTSchedGraph(VM));
  G = GPtr.get();

  TNL = &getAnalysis<TimingNetlist>();
  DA = &getAnalysis<DependenceAnalysis>();
  LI = &getAnalysis<LoopInfo>();

  buildSchedulingGraph();

  scheduleGlobal();

  G->fixIntervalForCrossBBChains();

  DEBUG(G->viewGraph());

  G->emitSchedule(VM);

  return true;
}
