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
#include "ScheduleDOT.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTModulePass.h"

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Support/CFG.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-scheduling-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumMemDep, "Number of Memory Dependencies Added");

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

VASTDep &VASTSchedUnit::EdgeBundle::getEdge(unsigned II) {
  assert(Edges.size() && "Unexpected empty edge bundle!");
  VASTDep *CurEdge = &Edges.front();
  int Latency = CurEdge->getLatency(II);

  // Zero II means we should ignore the loop-carried dependencies.
  // if (II == 0) return *CurEdge;

  for (unsigned i = 1, e = Edges.size(); i != e; ++i) {
    VASTDep &Edge = Edges[i];

    // Find the edge with biggest latency.
    int NewLatency = Edge.getLatency(II);
    if (NewLatency > Latency) {
      Latency = NewLatency;
      CurEdge = &Edge;
    }
  }

  return *CurEdge;
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

    if (isa<TerminatorInst>(Inst) && getTargetBlock())
      OS << " Targeting" << getTargetBlock()->getName();
  }

  OS << " Scheduled to " << Schedule;
}

void VASTSchedUnit::dump() const {
  print(dbgs());
}

//===----------------------------------------------------------------------===//
VASTSchedGraph::VASTSchedGraph() {
  // Create the entry SU.
  SUnits.push_back(new VASTSchedUnit(0, reinterpret_cast<BasicBlock*>(0)));

  // Create the exit SU.
  SUnits.push_back(new VASTSchedUnit(-1, reinterpret_cast<Instruction*>(0),
                                     false, 0, 0));
}

VASTSchedGraph::~VASTSchedGraph() {}

void VASTSchedGraph::schedule() {
  scheduleSDC();
}

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

//===----------------------------------------------------------------------===//
void SUBBMap::buildMap(VASTSchedGraph &G) {
  typedef VASTSchedGraph::iterator iterator;
  for (iterator I = llvm::next(G.begin()), E = G.getExit(); I != E; ++I)
    Map[I->getParent()].push_back(I);
}

MutableArrayRef<VASTSchedUnit*> SUBBMap::getSUInBB(BasicBlock *BB) {
  std::map<BasicBlock*, std::vector<VASTSchedUnit*> >::iterator
    at = Map.find(BB);

  assert(at != Map.end() && "BB not found!");

  return MutableArrayRef<VASTSchedUnit*>(at->second);
}

//===----------------------------------------------------------------------===//
namespace {
struct VASTScheduling : public VASTModulePass {
  static char ID;
  typedef std::map<Value*, SmallVector<VASTSchedUnit*, 4> > IR2SUMapTy;
  IR2SUMapTy IR2SUMap;
  VASTSchedGraph *G;
  TimingNetlist *TNL;
  VASTModule *VM;
  DependenceAnalysis *DA;

  VASTScheduling() : VASTModulePass(ID) {
    initializeVASTSchedulingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredID(BasicBlockTopOrderID);
    AU.addRequired<TimingNetlist>();
    AU.addRequired<DependenceAnalysis>();
  }

  VASTSchedUnit *getOrCreateBBEntry(BasicBlock *BB);

  void buildFlowDependencies(Instruction *I, VASTSchedUnit *U);
  void buildFlowDependencies(VASTSchedUnit *U);
  bool addFlowDepandency(Value *V, VASTSchedUnit *U);

  void buildMemoryDependencies(Instruction *Src, Instruction *Dst);
  void buildLoopDependencies(Instruction *Src, Instruction *Dst);
  void buildMemoryDependencies(BasicBlock *BB);

  void fixDanglingNodes();

  void buildSchedulingGraph();
  void buildSchedulingUnits(VASTSlot *S);

  bool runOnVASTModule(VASTModule &VM);

  void releaseMemory() {
    IR2SUMap.clear();
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
  INITIALIZE_PASS_DEPENDENCY(BasicBlockTopOrder)
  INITIALIZE_PASS_DEPENDENCY(DependenceAnalysis)
INITIALIZE_PASS_END(VASTScheduling,
                    "vast-scheduling", "Perfrom Scheduling on the VAST",
                    false, true)

Pass *llvm::createVASTSchedulingPass() {
  return new VASTScheduling();
}

bool VASTScheduling::addFlowDepandency(Value *V, VASTSchedUnit *U) {
  if (Argument *Arg = dyn_cast<Argument>(V)) {
    // Lookup the VASTValue corresponding to Arg.
    (void) Arg;

    U->addDep(G->getEntry(), VASTDep::CreateFlowDep(0));
    return true;
  }

  IR2SUMapTy::const_iterator at = IR2SUMap.find(V);

  if (at == IR2SUMap.end()) return false;

  // Get the corresponding latch SeqOp.
  ArrayRef<VASTSchedUnit*> SUs(at->second);
  for (unsigned i = 0; i < SUs.size(); ++i)
    if (SUs[i]->isLatching(V)) {
      U->addDep(SUs[i], VASTDep::CreateFlowDep(0));
      return true;
    }

  llvm_unreachable("Source SU not found!");
  return false;
}

void VASTScheduling::buildFlowDependencies(Instruction *I, VASTSchedUnit *U) {
  // Ignore the trivial case.
  if (I == 0) return;

  SmallVector<std::pair<Instruction*, Instruction::op_iterator>, 8> VisitStack;
  std::set<Value*> Visited;

  VisitStack.push_back(std::make_pair(I, I->op_begin()));

  while (!VisitStack.empty()) {
    Instruction *CurInst = VisitStack.back().first;

    if (CurInst->op_end() == VisitStack.back().second) {
      VisitStack.pop_back();
      continue;
    }

    Value *ChildNode = *VisitStack.back().second++;

    // Are we reach the leaf of the dependencies tree?
    if (addFlowDepandency(ChildNode, U))
      continue;

    if (Instruction *ChildInst = dyn_cast<Instruction>(ChildNode))
      VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
  }
}

void VASTScheduling::buildFlowDependencies(VASTSchedUnit *U) {
  Instruction *Inst = U->getInst();

  if (U->isLaunch()) {
    buildFlowDependencies(Inst, U);
    return;
  }

  assert(U->isLatch() && "Unexpected scheduling unit type!");

  if (ReturnInst *Ret = dyn_cast<ReturnInst>(Inst)) {
    buildFlowDependencies(Ret, U);
    // Also add the dependencies form the return instruction to the exit of
    // the scheduling graph.
    G->getExit()->addDep(U, VASTDep::CreateCndDep());
    return;
  }

  if (BranchInst *Br = dyn_cast<BranchInst>(Inst)) {
    // Add the dependencies from the condition.
    if (Br->isConditional() && !addFlowDepandency(Br->getCondition(), U))
        buildFlowDependencies(dyn_cast<Instruction>(Br->getCondition()), U);

    return;
  }

  if (SwitchInst *SW = dyn_cast<SwitchInst>(Inst)) {
    // Add the dependencies from the condition.
    if (!addFlowDepandency(SW->getCondition(), U))
      buildFlowDependencies(dyn_cast<Instruction>(SW->getCondition()), U);

    return;
  }

  // Add the dependencies from the incoming value.
  if (PHINode *PN = dyn_cast<PHINode>(Inst)) {
    BasicBlock *IncomingBB = U->getParent();
    BasicBlock *PNParent = PN->getParent();
    Value *V = PN->DoPHITranslation(PNParent, IncomingBB);
    if (!addFlowDepandency(V, U))
      buildFlowDependencies(dyn_cast<Instruction>(V), U);
    return;
  }

  // Nothing to do with Unreachable.
  if (isa<UnreachableInst>(Inst)) return;

  // Simply build the dependencies from the launch instruction.
  SmallVectorImpl<VASTSchedUnit*> &SUs = IR2SUMap[Inst];
  assert(SUs.size() >= 1 && "Launching SU not found!");
  VASTSchedUnit *LaunchU = SUs.front();
  assert(LaunchU->isLaunch() && "Bad SU type!");

  unsigned Latency = cast<VASTSeqInst>(U->getSeqOp())->getCyclesFromLaunch();
  U->addDep(LaunchU, VASTDep::CreateFixTimingConstraint(Latency));
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
    if (Inst == 0) continue;

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

      U->addDep(BBEntry, VASTDep::CreateCtrlDep(0));

      buildFlowDependencies(U);

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

        U->addDep(BBEntry, VASTDep::CreateCtrlDep(0));
        buildFlowDependencies(U);
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

static bool isCall(const Instruction *Inst) {
  const CallInst *CI = dyn_cast<CallInst>(Inst);

  if (CI == 0) return false;

  if (const IntrinsicInst *Intr = dyn_cast<IntrinsicInst>(CI)) {
    switch (Intr->getIntrinsicID()) {
    default: break;
    case Intrinsic::uadd_with_overflow:
    case Intrinsic::lifetime_end:
    case Intrinsic::lifetime_start: return false;
    }
  }

  return true;
}

static bool isLoadStore(const Instruction *Inst) {
  return isa<LoadInst>(Inst) || isa<StoreInst>(Inst);
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

    for (unsigned i = 0, e = PiorMemInsts.size(); i < e; ++i)
      buildMemoryDependencies(PiorMemInsts[i], Inst);

    PiorMemInsts.push_back(Inst);
  }
}

//===----------------------------------------------------------------------===//
void VASTScheduling::fixDanglingNodes() {
  typedef VASTSchedGraph::iterator iterator;
  for (iterator I = llvm::next(G->begin()), E = G->getExit(); I != E; ++I) {
    VASTSchedUnit *U = I;
    // Terminators will be handled later.
    if (U->isTerminator()) continue;

    BasicBlock *BB = U->getParent();

    // Constrain the SU by the exit of the same BB if it is not constrained yet.
    bool ConstrainedByExit = false;

    typedef VASTSchedUnit::use_iterator use_iterator;
    for (use_iterator UI = U->use_begin(), UE = U->use_end(); UI != UE; ++UI)
      if ((ConstrainedByExit = /*Assignment*/((*UI)->getParent() == BB)))
        break;

    if (ConstrainedByExit) continue;

    VASTSchedUnit *BBExit = IR2SUMap[BB->getTerminator()].front();

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

  // Assign constraint to the terminators in the same BB so that their are
  // scheduled to the same slot.
  Function &F = *VM;

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    Instruction *Inst = I->getTerminator();
    ArrayRef<VASTSchedUnit*> SUs(IR2SUMap[Inst]);
    assert(!SUs.empty() && "Scheduling Units for terminator not built?");
    VASTSchedUnit *U = SUs[0];
    for (unsigned i = 1; i < SUs.size(); ++i)
      SUs[i]->addDep(U, VASTDep::CreateFixTimingConstraint(0));
  }
}

void VASTScheduling::buildSchedulingGraph() {
  // Build the scheduling units according to the original scheduling.
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >
    RPO(VM->getStartSlot());

  typedef
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >::rpo_iterator
  slot_top_iterator;

  for (slot_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    buildSchedulingUnits(*I);

  // Build the memory dependencies.
  Function &F = *VM;
  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I)
    buildMemoryDependencies(I);

  // Connect the conditional dependencies.


  // Build the dependencies edges.
  // When the BranchInst looping back, we need to wait for the last instruction?
  // Or just build another conditional dependencies?

  // Constraint all nodes that do not have a user by the terminator in its parent
  // BB.
  fixDanglingNodes();

#ifndef NDEBUG
  G->verify();
#endif

  DEBUG(G->viewGraph());
}

bool VASTScheduling::runOnVASTModule(VASTModule &VM) {
  this->VM = &VM;

  OwningPtr<VASTSchedGraph> GPtr(new VASTSchedGraph());
  G = GPtr.get();

  TimingNetlist &TNL = getAnalysis<TimingNetlist>();
  (void) TNL;

  DA = &getAnalysis<DependenceAnalysis>();

  buildSchedulingGraph();

  G->schedule();

  G->emitSchedule(VM);

  return true;
}
