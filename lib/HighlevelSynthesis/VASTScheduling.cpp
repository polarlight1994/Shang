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

#include "llvm/IR/DataLayout.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-scheduling-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, Instruction *Inst, bool IsLatch,
                             BasicBlock *BB, VASTSeqOp *SeqOp)
  : T(IsLatch ? Latch : Launch), Schedule(0),  InstIdx(InstIdx), Inst(Inst),
    BB(BB), SeqOp(SeqOp) {}

VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, BasicBlock *BB, Type T)
  : T(T), Schedule(0),  InstIdx(InstIdx), Inst(0), BB(BB), SeqOp(0) {}

VASTSchedUnit::VASTSchedUnit(Type T, unsigned InstIdx, BasicBlock *Parent)
  : T(T), Schedule(0), InstIdx(InstIdx), Inst(0), BB(Parent), SeqOp(0)
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

bool VASTSchedUnit::requireLinearOrder() const {
  VASTSeqOp *Op = getSeqOp();

  if (Op == 0) return false;

  if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(Op)) {
    if (SeqInst->num_srcs() == 0) return false;

    // Ignore the Latch, they will not cause a resource conflict.
    if (SeqInst->isLatch()) return false;

    Instruction *I = dyn_cast<Instruction>(SeqInst->getValue());

    // Linear order is required for the accesses to memory bus.
    if (I->mayReadOrWriteMemory()) return true;

    // The launch operation to enable a module also requires linear order.
    VASTSelector *Sel = SeqInst->getSrc(SeqInst->num_srcs() - 1).getSelector();
    return Sel->isEnable();
  }

  return false;
}

Instruction *VASTSchedUnit::getInst() const {
  assert(Inst && "Inst not available!");
  return Inst;
}

BasicBlock *VASTSchedUnit::getParent() const {
  assert(!isEntry() && !isExit() && "Call getParent on the wrong SUnit type!");

  if (Inst == 0) return BB;

  if (isa<PHINode>(getInst()) && isLatch()) return getIncomingBlock();

  return Inst->getParent();
}

void VASTSchedUnit::scheduleTo(unsigned Step) {
  assert(Step && "Bad schedule!");
  Schedule = Step;
}

void VASTSchedUnit::resetSchedule() {
  Schedule = 0;
}

void VASTSchedUnit::print(raw_ostream &OS) const {
  OS << '#' << InstIdx << ' ';

  if (isEntry()) {
    OS << "Entry Node";
    return;
  }

  if (isExit()) {
    OS << "Exit Node";
    return;
  }

  if (isBBEntry()) OS << "BB Entry\t";
  else if (isVirtual()) OS << "Virtual\t";

  OS << (isLaunch() ? "Launch" : "Latch")
     << " Parent: " << getParent()->getName();

  if (BB)
    OS << " BB: " << BB->getName();
  else if (Inst) {
    OS << ' ' << *Inst; //Inst->getName();

    if (isa<PHINode>(Inst) && isLatch())
      OS << " From: " << getIncomingBlock()->getName();

    if (isTerminator())
      if (BasicBlock *BB =  getTargetBlock())
        OS << " Targeting: " << BB->getName();
  }

  OS << " Scheduled to " << Schedule << ' ' << this;
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
  SUnits.push_back(new VASTSchedUnit(VASTSchedUnit::Entry, 0, 0));
  // Create the exit SU.
  SUnits.push_back(new VASTSchedUnit(VASTSchedUnit::Exit, -1, 0));
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

void VASTSchedGraph::topsortCone(VASTSchedUnit *Root,
                                 std::set<VASTSchedUnit*> &Visited,
                                 BasicBlock *BB) {
  if (!Visited.insert(Root).second) return;

  typedef VASTSchedUnit::dep_iterator ChildIt;
  std::vector<std::pair<VASTSchedUnit*, ChildIt> > WorkStack;

  WorkStack.push_back(std::make_pair(Root, Root->dep_begin()));

  while (!WorkStack.empty()) {
    VASTSchedUnit *U = WorkStack.back().first;
    ChildIt I = WorkStack.back().second;

    // Visit the current node if all its dependencies are visited.
    if (U->isBBEntry() || I == U->dep_end()) {
      WorkStack.pop_back();
      SUnits.splice(SUnits.end(), SUnits, U);
      continue;
    }

    VASTSchedUnit *Child = *I;
    ++WorkStack.back().second;

    if (Child->isEntry() || Child->getParent() != BB)
      continue;

    // Do not visit the same node twice!
    if (!Visited.insert(Child).second) continue;

    WorkStack.push_back(std::make_pair(Child, Child->dep_begin()));
  }
}

void VASTSchedGraph::topologicalSortSUs() {
  VASTSchedUnit *Entry = getEntry(), *Exit = getExit();
  assert(Entry->isEntry() && Exit->isExit() && "Bad order!");

  std::set<VASTSchedUnit*> Visited;
  SUnits.splice(SUnits.end(), SUnits, Entry);

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    Visited.clear();
    BasicBlock *BB = I;

    MutableArrayRef<VASTSchedUnit*> SUs(getSUInBB(BB));
    for (unsigned i = 0; i < SUs.size(); ++i)
      topsortCone(SUs[i], Visited, BB);
  }

  SUnits.splice(SUnits.end(), SUnits, Exit);

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
char VASTScheduling::ID = 0;

VASTScheduling::VASTScheduling() : VASTModulePass(ID) {
  initializeVASTSchedulingPass(*PassRegistry::getPassRegistry());
}

void VASTScheduling::getAnalysisUsage(AnalysisUsage &AU) const  {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequiredID(BasicBlockTopOrderID);
  AU.addRequired<TimingNetlist>();
  AU.addRequired<AliasAnalysis>();
  AU.addRequired<DominatorTree>();
  AU.addRequired<LoopInfo>();
  AU.addRequired<BranchProbabilityInfo>();
}

INITIALIZE_PASS_BEGIN(VASTScheduling,
                      "vast-scheduling", "Perfrom Scheduling on the VAST",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(LoopInfo)
  INITIALIZE_PASS_DEPENDENCY(BranchProbabilityInfo)
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
              || SrcSeqVal == check(CurSU->getSeqOp())->getDef(0))
            && "All PHI latching SeqOp should define the same SeqOp!");
      SrcSeqVal = check(CurSU->getSeqOp())->getDef(0);

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

float VASTScheduling::slackFromPrevStage(VASTSeqOp *Op) {
  VASTSeqInst *SrcOp = dyn_cast_or_null<VASTSeqInst>(Op);

  if (SrcOp == 0 || !SrcOp->isLatch()) return 0.0f;

  TimingNetlist::RegDelaySet Srcs;
  VASTLatch CurLatch = SrcOp->getSrc(0);
  VASTValPtr LatchSrc = CurLatch;
  TNL->extractDelay(CurLatch.getSelector(), LatchSrc.get(), Srcs);

  // We do not have any delay information from the previous pipeline stage.
  if (Srcs.empty()) return 0.0f;

  // Do not know how to compute the slack ...
  if (!SrcOp->isLatch() || SrcOp->getCyclesFromLaunch() == 0) return 0.0f;

  float MinimalSlack = 1.0f;
  // For a latch from FU, there is at least 1 cycle available.
  float CycleSlack = 1.0f;
  typedef TimingNetlist::RegDelaySet::iterator src_iterator;
  for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    float CurDelay = I->second;
    float CurSlack = std::max<float>(ceil(CurDelay), CycleSlack) - CurDelay;
    MinimalSlack = std::min(MinimalSlack, CurSlack);
  }

  return MinimalSlack;
}

void
VASTScheduling::buildFlowDependencies(VASTSchedUnit *DstU, VASTSeqValue *Src,
                                      float delay) {
  assert(Src && "Not a valid source!");

  Value *V = Src->getLLVMValue();
  assert(V && "Expect LLVM Value for flow dependencies!");
  assert((Src->num_fanins() == 1 || isa<PHINode>(V)) && "SeqVal not in SSA!");
  assert((!isa<Instruction>(V)
          || DT->dominates(cast<Instruction>(V)->getParent(), DstU->getParent()))
         && "Flow dependency should be a dominance edge!");
  // The static register is virtually defined at the entry slot. Because
  // we only write it when the function exit. Whe we read is the value from
  // last function execution.
  VASTSchedUnit *SrcSU = Src->isStatic() ? G->getEntry() : getFlowDepSU(V);

  // Try to fold the delay of current pipeline stage to the previous pipeline
  // stage, if the previous pipeline stage has enough slack.
  if (slackFromPrevStage(SrcSU->getSeqOp()) > delay)
    delay = 0.0f;

  assert(!Src->isFUOutput() && "Unexpected FU output!");
  DstU->addDep(SrcSU, VASTDep::CreateFlowDep(ceil(delay)));
}

void VASTScheduling::buildFlowDependencies(VASTSeqOp *Op, VASTSchedUnit *U) {
  TimingNetlist::RegDelaySet Srcs;
  typedef TimingNetlist::RegDelaySet::iterator src_iterator;

  assert(Op->num_srcs() && "No operand for flow dependencies!");

  VASTValue *Cnd = VASTValPtr(Op->getGuard()).get();

  for (unsigned i = 0, e = Op->num_srcs(); i != e; ++i) {
    VASTLatch L = Op->getSrc(i);
    VASTValue *FI = VASTValPtr(L).get();
    // FIXME: Assert the selector is in SSA form!
    VASTSelector *Sel = L.getSelector();

    // Extract the delay from the fanin and the guarding condition.
    TNL->extractDelay(Sel, FI, Srcs);
    TNL->extractDelay(Sel, Cnd, Srcs);
  }

  // Also calculate the path for the guarding condition.
  for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
    buildFlowDependencies(U, I->first, I->second);
}

void VASTScheduling::buildFlowDependenciesForSlotCtrl(VASTSchedUnit *U) {
  VASTSlotCtrl *SlotCtrl = cast<VASTSlotCtrl>(U->getSeqOp());
  TimingNetlist::RegDelaySet Srcs;
  typedef TimingNetlist::RegDelaySet::iterator src_iterator;
  
  VASTValue *Cnd = VASTValPtr(SlotCtrl->getGuard()).get();
  VASTRegister *Slot = SlotCtrl->getTargetSlot()->getRegister();
  VASTSelector *Sel = Slot ? Slot->getSelector() : 0;

  TNL->extractDelay(Sel, Cnd, Srcs);

  for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
    buildFlowDependencies(U, I->first, I->second);
}

void VASTScheduling::buildFlowDependencies(VASTSchedUnit *U) {
  Instruction *Inst = U->getInst();

  if (U->isLaunch()) {
    buildFlowDependencies(U->getSeqOp(), U);
    return;
  }

  assert(U->isLatch() && "Unexpected scheduling unit type!");

  if (isa<PHINode>(Inst)) {
    buildFlowDependencies(U->getSeqOp(), U);
    return;
  }

  if (isa<TerminatorInst>(Inst)) {
    buildFlowDependencies(U->getSeqOp(), U);
    return;
  }

  VASTSeqInst *SeqInst = cast<VASTSeqInst>(U->getSeqOp());
  unsigned Latency = SeqInst->getCyclesFromLaunch();
  // This is a lowered signle-element- block RAM access.
  if (Latency == 0) {
    assert(isa<StoreInst>(Inst)
           && isa<GlobalVariable>(cast<StoreInst>(Inst)->getPointerOperand())
           && "Zero latency latching is not allowed!");
    buildFlowDependencies(U->getSeqOp(), U);
    return;
  }

  // Simply build the dependencies from the launch instruction.
  SmallVectorImpl<VASTSchedUnit*> &SUs = IR2SUMap[Inst];
  assert(SUs.size() >= 1 && "Launching SU not found!");
  VASTSchedUnit *LaunchU = SUs.front();
  assert(LaunchU->isLaunch() && "Bad SU type!");

  U->addDep(LaunchU, VASTDep::CreateFixTimingConstraint(Latency));
}

VASTSchedUnit *VASTScheduling::getOrCreateBBEntry(BasicBlock *BB) {
  SmallVectorImpl<VASTSchedUnit*> &SUs = IR2SUMap[BB];

  // Simply return the BBEntry if it had already existed.
  if (!SUs.empty() && SUs.back()->isBBEntry())
    return SUs.back();

  VASTSchedUnit *Entry = G->createSUnit(BB, VASTSchedUnit::BlockEntry);

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
    // No need to add the dependency edges from the incoming values, because
    // the SU is anyway scheduled to the same slot as the entry of the BB.
    // And we will build the conditional dependencies for the conditional
    // CFG edge between BBs.
    IR2SUMap[PN].push_back(U);
  }

  return Entry;
}

void VASTScheduling::buildSchedulingUnits(VASTSlot *S) {
  // Ignore the subgroups.
  if (S->IsSubGrp) return;

  BasicBlock *BB = S->getParent();

  // If the BB is NULL, this slot should be the entry or the exit of the
  // state-transition graph.
  VASTSchedUnit *BBEntry = 0;
  if (BB == 0) BBEntry = G->getEntry();
  else         BBEntry = getOrCreateBBEntry(BB);

  std::vector<VASTSeqOp*> Ops;
  typedef VASTSlot::subgrp_iterator subgrp_iterator;
  for (subgrp_iterator SI = S->subgrp_begin(), SE = S->subgrp_end();
       SI != SE; ++SI) {
    // Collect the operation in the current slot and the subgroups.
    Ops.insert(Ops.end(), SI->op_begin(), SI->op_end());
  }

  typedef std::vector<VASTSeqOp*>::iterator op_iterator;
  for (op_iterator OI = Ops.begin(), OE = Ops.end(); OI != OE; ++OI) {
    VASTSeqOp *Op = *OI;
    Instruction *Inst = dyn_cast_or_null<Instruction>(Op->getValue());
    // We can safely ignore the SeqOp that does not correspond to any LLVM
    // Value, their will be rebuilt when we emit the scheduling.
    if (Inst == 0) {
      if (BB == 0) {
        if (Argument *Arg = dyn_cast_or_null<Argument>(Op->getValue())) {
          // Remember the corresponding SeqVal.
          bool inserted
            = ArgMap.insert(std::make_pair(Arg, Op->getDef(0))).second;
          assert(inserted && "SeqVal for argument not inserted!");
          (void) inserted;
        }
      }

      continue;
    }

    if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(Op)) {
      VASTSchedUnit *U = 0;
      bool IsLatch = SeqInst->isLatch();

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

      buildFlowDependencies(U);

      U->addDep(BBEntry, VASTDep::CreateCtrlDep(0));
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

        buildFlowDependenciesForSlotCtrl(U);

        U->addDep(BBEntry, VASTDep::CreateCtrlDep(0));
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
void VASTScheduling::buildControlFlowEdges() {
  Function &F = G->getFunction();
  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;

    SmallVectorImpl<VASTSchedUnit*> &SUs = IR2SUMap[BB];
    VASTSchedUnit *Entry = 0;
    for (unsigned i = 0; i < SUs.size(); ++i)
      if (SUs[i]->isBBEntry())
        Entry = SUs[i];

    for (unsigned i = 0; i < SUs.size(); ++i) {
      if (SUs[i]->isBBEntry())
        continue;

      assert(isa<TerminatorInst>(SUs[i]->getInst())
             && "Unexpected instruction type!");
      assert(SUs[i]->getTargetBlock() == BB && "Wrong target BB!");
      Entry->addDep(SUs[i], VASTDep::CreateCndDep());
    }
  }
}
void VASTScheduling::preventInfinitUnrolling(Loop *L) {
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

  // Try to prevent the other part of the loop from being folded across the
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

void VASTScheduling::preventImplicitPipelining(Loop *L) {
  BasicBlock *Header = L->getHeader();

  SmallVector<VASTSchedUnit*, 8> BackEdges, ExitingEdges;
  typedef Loop::block_iterator block_iterator;

  for (block_iterator I = L->block_begin(), E = L->block_end(); I != E; ++I) {
    BasicBlock *BB = *I;
    TerminatorInst *Inst = BB->getTerminator();

    // Ignore the Return and Unreachable, which will wait for the whole function.
    if (!isa<BranchInst>(Inst) && !isa<SwitchInst>(Inst)) continue;

    BackEdges.clear();
    ExitingEdges.clear();

    ArrayRef<VASTSchedUnit*> Terminators(IR2SUMap[Inst]);
    for (unsigned i = 0; i < Terminators.size(); ++i) {
      VASTSchedUnit *Terminator = Terminators[i];
      assert(Terminator->isTerminator() && "Unexpected scheduling unit type!");
      BasicBlock *Dst = Terminator->getTargetBlock();

      // Collect the branching operations targetting the header of the loop,
      // the corresponding edges are backedges.
      if (Dst == Header)
        BackEdges.push_back(Terminator);
      // Also collect the extining edges.
      else
        ExitingEdges.push_back(Terminator);
    }

    // Add dependencies to prevent the the backedges being activatived before
    // we reach the exiting edges.
    typedef SmallVector<VASTSchedUnit*, 8>::iterator iterator;
    for (iterator BI = BackEdges.begin(), BE = BackEdges.end(); BI != BE; ++BI) {
      VASTSchedUnit *BackEdge = *BI;
      for (iterator EI = ExitingEdges.begin(), EE = ExitingEdges.end();
           EI != EE; ++EI) {
        VASTSchedUnit *Exiting = *EI;
        if (Exiting->isDependsOn(BackEdge)) {
          assert(Exiting->getEdgeFrom(BackEdge).getLatency() == 0
                 && "Not able to handle contradictory constraint!");
          Exiting->removeDep(BackEdge);
          // If we require both Exiting >= BackEdge and BackEdge >= Exiting,
          // then we can simply use Exiting == BackEdge
          BackEdge->addDep(Exiting, VASTDep::CreateFixTimingConstraint(0));
          continue;
        }

        BackEdge->addDep(Exiting, VASTDep::CreateCtrlDep(0));
      }
    }
  }
}

void VASTScheduling::fixSchedulingGraph() {
  // Try to fix the dangling nodes.
  typedef VASTSchedGraph::iterator iterator;
  for (iterator I = llvm::next(G->begin()), E = G->getExit(); I != E; ++I) {
    VASTSchedUnit *U = I;

    // Ignore the entries.
    if (U->isBBEntry()) continue;

    // Terminators will be handled later.
    if (U->isTerminator()) continue;

    // Ignore the virtual nodes.
    if (U->isVirtual()) continue;

    Instruction *Inst = U->getInst();
    // Returns will be handled later, too.
    if (Inst && (isa<UnreachableInst>(Inst) || isa<ReturnInst>(Inst)))
      continue;

#ifdef ENABLE_FINE_GRAIN_CFG_SCHEDULING
    // At least constrain the scheduling unit with something.
    if (U->use_empty()) G->getExit()->addDep(U, VASTDep::CreateCtrlDep(0));
#else
    // Only need to create the pseudo dependencies to the exit node. Because
    // the PHI node will always be scheduled to the same slot as the
    // terminator.
    if (U->isPHILatch() || U->isPHI()) {
      G->getExit()->addDep(U, VASTDep::CreateCtrlDep(0));
      continue;
    }

    BasicBlock *BB = U->getParent();

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

      // The only chance that U's idx is bigger than BBExit's idx is when U is
      // a PHI node, which is handled above.
      assert(BBExit->getIdx() >= U->getIdx() && "Unexpected index order!");

      // Protected by the later linear order builder, we do not need add
      // dependence to wait the launch operation finish here.
      BBExit->addDep(U, VASTDep::CreateCtrlDep(0));
    }
#endif
  }

  // Also add the dependencies form the return instruction to the exit of
  // the scheduling graph.
  Function &F = VM->getLLVMFunction();

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    TerminatorInst *Inst = I->getTerminator();

    if ((isa<UnreachableInst>(Inst) || isa<ReturnInst>(Inst))) {
      ArrayRef<VASTSchedUnit*> SUs(IR2SUMap[Inst]);
      assert(!SUs.empty() && "Scheduling Units for terminator not built?");
      VASTSchedUnit *LastSU = SUs[0];

      for (unsigned i = 1; i < SUs.size(); ++i) {
        VASTSchedUnit *U = SUs[i];
        U->addDep(LastSU, VASTDep::CreateFixTimingConstraint(0));
        LastSU = U;
      }

      G->getExit()->addDep(LastSU, VASTDep::CreateCtrlDep(0));
      continue;
    }
  }

  SmallVector<Loop*, 64> Worklist(LI->begin(), LI->end());
  while (!Worklist.empty()) {
    Loop *L = Worklist.pop_back_val();

    // Also push the children of L into the work list.
    if (!L->empty()) Worklist.append(L->begin(), L->end());

    // Prevent the scheduler from generating 1 slot loop, in which case the loop
    // can be entirely folded into its predecessors. If this happen, the schedule
    // emitter will try to unroll the loop.
    preventInfinitUnrolling(L);

    // If the branch operations for the backedges are scheduled before the whole
    // loop body is finished, we will get a pipelined loop, which is not what we
    // want as the SDC scheduling cannot correctly model the modulo resource
    // reservation at the moment.
    preventImplicitPipelining(L);
  }
}

void VASTScheduling::scheduleGlobal() {
  SDCScheduler Scheduler(*G, 1);

  Scheduler.addLinOrdEdge(*DT, IR2SUMap);

  // Build the step variables, and no need to schedule at all if all SUs have
  // been scheduled.
  if (Scheduler.createLPAndVariables()) {
    unsigned TotalWeight = 0;

    Function &F = VM->getLLVMFunction();
    typedef Function::iterator iterator;
    for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
      BasicBlock *BB = I;
      DEBUG(dbgs() << "Applying constraints to BB: " << BB->getName() << '\n');

      unsigned ExitWeigthSum = 0;
      ArrayRef<VASTSchedUnit*> Exits(IR2SUMap[BB->getTerminator()]);
      for (unsigned i = 0; i < Exits.size(); ++i) {
        VASTSchedUnit *BBExit = Exits[i];
        // Ignore the return value latching operation here. We will add the fix
        // timing constraints between it and the actual terminator.
        if (!BBExit->isTerminator()) {
          assert(isa<ReturnInst>(BB->getTerminator()) && "BBExit is not terminator!");
          continue;
        }

        unsigned ExitWeight = 1024;
        BranchProbability BP = BranchProbability::getOne();
        if (BasicBlock *TargetBB = BBExit->getTargetBlock())
          BP = BPI->getEdgeProbability(BB, TargetBB);

        ExitWeight = ExitWeight * BP.getNumerator() / BP.getDenominator();
        Scheduler.addObjectCoeff(BBExit, - 1.0 * ExitWeight);
        DEBUG(dbgs().indent(4) << "Setting Exit Weight: " << ExitWeight
                               << ' ' << BP << '\n');

        ExitWeigthSum += ExitWeight;
        TotalWeight += ExitWeight;
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

      assert(ExitWeigthSum && "Unexpected zero weight!");
      Scheduler.addObjectCoeff(BBEntry, ExitWeigthSum);
      DEBUG(dbgs().indent(2) << "Setting Entry Weight: "
                             << ExitWeigthSum << '\n');
    }

    Scheduler.addObjectCoeff(G->getExit(), - 1.0 * (TotalWeight + 1024));

    bool success = Scheduler.schedule();
    assert(success && "SDCScheduler fail!");
    (void) success;
  }

  DEBUG(G->viewGraph());
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

  buildControlFlowEdges();

  buildMemoryDependencies();

  // Constraint all nodes that do not have a user by the terminator in its parent
  // BB.
  fixSchedulingGraph();

#ifndef NDEBUG
  G->verify();
#endif

  // Rebuild the topological order after the verification process, because the
  // topological sort implicitly eliminate the SUs that do not have any
  // dependencies.
  G->topologicalSortSUs();

  DEBUG(G->viewGraph());
}

bool VASTScheduling::runOnVASTModule(VASTModule &VM) {
  this->VM = &VM;
  Function &F = VM.getLLVMFunction();

  OwningPtr<VASTSchedGraph> GPtr(new VASTSchedGraph(F));
  G = GPtr.get();

  // Initialize the analyses
  TNL = &getAnalysis<TimingNetlist>();
  AA = &getAnalysis<AliasAnalysis>();
  LI = &getAnalysis<LoopInfo>();
  BPI = &getAnalysis<BranchProbabilityInfo>();
  DT = &getAnalysis<DominatorTree>();

  buildSchedulingGraph();

  scheduleGlobal();

  fixIntervalForCrossBBChains();

  DEBUG(G->viewGraph());

  emitSchedule();

  return true;
}
