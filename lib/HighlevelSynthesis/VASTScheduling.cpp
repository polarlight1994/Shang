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

#include "Dataflow.h"
#include "PreSchedBinding.h"
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
#include "llvm/Analysis/BlockFrequencyInfo.h"
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

STATISTIC(NumIterations, "Number of Scheduling-Binding Iterations");

//===----------------------------------------------------------------------===//
VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, Instruction *Inst, bool IsLatch,
                             BasicBlock *BB, VASTSeqOp *SeqOp)
  : T(IsLatch ? Latch : Launch), II(1), Schedule(0), InstIdx(InstIdx), Inst(Inst),
    BB(BB), SeqOp(SeqOp) {}

VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, BasicBlock *BB, Type T)
  : T(T), II(0), Schedule(0),  InstIdx(InstIdx), Inst(0), BB(BB), SeqOp(0) {}

VASTSchedUnit::VASTSchedUnit(Type T, unsigned InstIdx, BasicBlock *Parent)
  : T(T), II(1), Schedule(0), InstIdx(InstIdx), Inst(0), BB(Parent), SeqOp(0)
{}

bool VASTSchedUnit::EdgeBundle::isLegalToAddFixTimngEdge(int Latency) const {
  assert(!Edges.empty() && "Unexpected empty bundle!");

  if (Edges.size() > 1)
    return false;

  VASTDep OldEdge = Edges.back();
  return OldEdge.getEdgeType() == VASTDep::FixedTiming
         && OldEdge.getLatency() == Latency;
}

void VASTSchedUnit::EdgeBundle::addEdge(VASTDep NewEdge) {
  if (NewEdge.getEdgeType() == VASTDep::FixedTiming) {
    assert(isLegalToAddFixTimngEdge(NewEdge.getLatency())
           && "Fixed timing constraint cannot be added!");
    return;
  }

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

bool VASTSchedUnit::EdgeBundle::hasValDep() const {
  bool HasValDep = Edges.front().getEdgeType() == VASTDep::ValDep;

  for (unsigned i = 1, e = Edges.size(); i != e; ++i)
    HasValDep |= Edges[i].getEdgeType() == VASTDep::ValDep;

  return HasValDep;
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

bool VASTSchedUnit::scheduleTo(unsigned Step) {
  bool Changed = Step != Schedule;
  assert(Step && "Bad schedule!");
  Schedule = Step;
  return Changed;
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

  if (Inst) {
    OS << ' ' << *Inst; //Inst->getName();

    if (isa<PHINode>(Inst) && isLatch())
      OS << " From: " << getIncomingBlock()->getName();

    if (isTerminator())
      if (BasicBlock *BB =  getTargetBlock())
        OS << " Targeting: " << BB->getName();
  } else if (BB)
    OS << " BB: " << BB->getName();

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
VASTSchedGraph::VASTSchedGraph(Function &F) : F(F), TotalSUs(2) {
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

  assert(Idx == size() && "Topological sort is not applied to all SU?");
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
  AU.addRequired<PreSchedBinding>();
  AU.addPreserved<PreSchedBinding>();
  AU.addRequired<DataflowAnnotation>();
  AU.addRequired<AliasAnalysis>();
  AU.addRequired<DominatorTree>();
  AU.addRequired<LoopInfo>();
  AU.addRequired<BranchProbabilityInfo>();
  // There is a bug in BlockFrequencyInfo :(
  // AU.addRequired<BlockFrequencyInfo>();
}

INITIALIZE_PASS_BEGIN(VASTScheduling,
                      "vast-scheduling", "Perfrom Scheduling on the VAST",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(PreSchedBinding)
  INITIALIZE_PASS_DEPENDENCY(DataflowAnnotation)
  INITIALIZE_PASS_DEPENDENCY(LoopInfo)
  INITIALIZE_PASS_DEPENDENCY(BranchProbabilityInfo)
  INITIALIZE_PASS_DEPENDENCY(BlockFrequencyInfo)
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

void VASTScheduling::buildFlowDependencies(VASTSchedUnit *DstU, Value *Src,
                                           bool IsLaunch, float delay) {
  assert(Src && "Not a valid source!");
  assert((!isa<Instruction>(Src)
          || DT->dominates(cast<Instruction>(Src)->getParent(), DstU->getParent()))
         && "Flow dependency should be a dominance edge!");

  VASTSchedUnit *SrcSU = 0;
  Instruction *SrcInst = dyn_cast<Instruction>(Src);

  if (!isChainingCandidate(Src)) {
    // Get the latch SU, if source cannot be chained.
    SrcSU = getFlowDepSU(Src);
    float slack = DF->getSlackFromLaunch(SrcInst);
    if (IsLaunch) {
      float DelayFromLaunch = 1.0f - slack;
      // TODO: Only set the upperbound to 1 for non-chaining candidate.
      delay -= std::max(1.0f, DelayFromLaunch);
      // Do not produce a negative delay!
      delay = std::max(0.0f, delay);
    } else if (slack > delay)
      // Try to fold the delay of current pipeline stage to the previous pipeline
      // stage, if the previous pipeline stage has enough slack.
      delay = 0.0f;
  } else {
    bool IsChainingEnableOnEdge = DF->getDelayFromLaunch(SrcInst) <= 1.0f &&
                                  isChainingCandidate(DstU->getInst());

    if (IsLaunch) {
      // Ignore the dependencies from FUInput if Src is not chaining candidate,
      // we will build the dependencies from the result.
      if (!IsChainingEnableOnEdge || Src == DstU->getInst())
        return;

      // We need to build the dependence from launch SU if Src is FUInput.
      SrcSU = getLaunchSU(Src);

      // Round down the delay for single cycle path from launch to launch to
      // enable chaining.
      if (DstU->isLaunch() && delay < 1.0f)
        delay = 0.0f;  //floor(delay);
    } else {
      // We will build the dependency from the launch SU (to launch SU) for
      // chaining candidates.
      if (IsChainingEnableOnEdge && getLaunchSU(Src))
        return;

      // Get the latch SU for the latch operation.
      SrcSU = getFlowDepSU(Src);
    }
  }

  assert(delay >= 0.0f && "Unexpected negative delay!");
  DstU->addDep(SrcSU, VASTDep::CreateFlowDep(ceil(delay)));
}

void VASTScheduling::buildFlowDependencies(Instruction *Inst, VASTSchedUnit *U) {
  Dataflow::SrcSet Srcs;

  DF->getFlowDep(U->getSeqOp(), Srcs);
  typedef Dataflow::SrcSet::iterator src_iterator;
  // Also calculate the path for the guarding condition.
  for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
    buildFlowDependencies(U, I->first, I->first.IsLauch(), I->second);
}

void
VASTScheduling::buildFlowDependenciesForPHILatch(PHINode *PHI, VASTSchedUnit *U) {
  Dataflow::SrcSet Srcs;
  typedef Dataflow::SrcSet::iterator src_iterator;

  DF->getIncomingFrom(DataflowInst(PHI, false), U->getParent(), Srcs);

  // Also calculate the path for the guarding condition.
  for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
    buildFlowDependencies(U, I->first, I->first.IsLauch(), I->second);
}

void VASTScheduling::buildFlowDependencies(VASTSchedUnit *U) {
  Instruction *Inst = U->getInst();

  if (U->isLaunch()) {
    buildFlowDependencies(Inst, U);
    return;
  }

  assert(U->isLatch() && "Unexpected scheduling unit type!");

  if (PHINode *PHI = dyn_cast<PHINode>(Inst)) {
    buildFlowDependenciesForPHILatch(PHI, U);
    return;
  }

  if (isa<TerminatorInst>(Inst)) {
    buildFlowDependencies(Inst, U);
    return;
  }

  VASTSeqInst *SeqInst = cast<VASTSeqInst>(U->getSeqOp());
  unsigned Latency = SeqInst->getCyclesFromLaunch();
  VASTSchedUnit *LaunchU = getLaunchSU(Inst);

  if (Latency == 0) {
    assert(isChainingCandidate(Inst) && "Unexpected 0 cycles from launch!");
    float delay = DF->getDelayFromLaunch(Inst);
    Latency = std::max<unsigned>(1, ceil(delay));
    // Set the correct cycle from Launch now.
    if (LaunchU)
      SeqInst->setCyclesFromLaunch(Latency);

    // Also build the flow dependencies to the latch SU to setup the upper bound
    // of combination delay in case of (single cycle) chaining.
    buildFlowDependencies(Inst, U);
  }

  // Simply build the dependencies from the launch instruction.
  if (LaunchU) {
    U->addDep(LaunchU, VASTDep::CreateFixTimingConstraint(Latency));
    LaunchU->setII(Latency);
  }
}

VASTSchedUnit *VASTScheduling::getLaunchSU(Value *V) {
  IR2SUMapTy::iterator I = IR2SUMap.find(V);

  if (I == IR2SUMap.end())
    return 0;

  ArrayRef<VASTSchedUnit*> SUs(I->second);

  VASTSchedUnit *LaunchU = SUs.front();
  if (!LaunchU->isLaunch())
    return 0;

  return LaunchU;
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

        buildFlowDependencies(U);

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
    // Ensure the WAR dependencies for the induction variables. Disable this if
    // we want to perform software pipelining.
    buildWARDepForPHIs(L);
  }
}

namespace {
struct IterativeSchedulingBinding {
  SDCScheduler Scheduler;
  PreSchedBinding &PSB;
  DominatorTree &DT;
  BlockFrequencyInfo &BFI;
  BranchProbabilityInfo &BPI;

  typedef ArrayRef<VASTSchedUnit*> SUArrayRef;
  typedef std::map<Value*, SmallVector<VASTSchedUnit*, 4> > IR2SUMapTy;
  IR2SUMapTy &IR2SUMap;

  enum State {
    Initial, Scheduling, Binding
  };

  State S;
  unsigned ScheduleViolation, BindingViolation;
  float TotalWeight;
  const float PerformanceFactor, ResourceFactor;

  IterativeSchedulingBinding(VASTSchedGraph &G, PreSchedBinding &PSB,
                             DominatorTree &DT, BlockFrequencyInfo &BFI,
                             BranchProbabilityInfo &BPI, IR2SUMapTy &IR2SUMap)
    : Scheduler(G, 1), PSB(PSB), DT(DT), BFI(BFI), BPI(BPI), IR2SUMap(IR2SUMap),
      ScheduleViolation(0), BindingViolation(0), TotalWeight(0.0),
      S(Initial), PerformanceFactor(64.0f), ResourceFactor(0.01f) {
    // Build the hard linear order.
    Scheduler.addLinOrdEdge(DT, IR2SUMap);
  }

  bool checkCompatibility() {
    bool AnyViolation = false;
    typedef CompGraphBase::cluster_iterator cluster_iterator;
    for (cluster_iterator I = PSB->cluster_begin(), E = PSB->cluster_end();
         I != E; ++I) {
      AnyViolation |= checkCompatibility(*I);
    }

    return AnyViolation;
  }

  static VASTSchedUnit *getLaunch(SUArrayRef SUs) {
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *SU = SUs[i];
      if (!SU->isLaunch())
        continue;

      return SU;
    }

    llvm_unreachable("SU not found!");
    return 0;
  }

  VASTSchedUnit *lookupSU(VASTSeqOp *Op) {
    Value *V = Op->getValue();
    assert(V && "Unexpected virtual node!");
    SUArrayRef SUs(IR2SUMap[V]);

    assert(SUs.size() && "Corresponding scheduling units not found!");
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *SU = SUs[i];
      if (SU->getSeqOp() == Op)
        return SU;
    }

    llvm_unreachable("SU not found!");
    return 0;
  }

  typedef CompGraphBase::ClusterType ClusterType;
  bool checkCompatibility(const ClusterType &Cluster);
  unsigned checkCompatibility(PSBCompNode *Src, SUArrayRef SrcSUs,
                              PSBCompNode *Dst, SUArrayRef DstSUs);

  typedef SmallVector<std::pair<VASTSchedUnit*, VASTSchedUnit*>, 8>
          ViolationPairs;
  unsigned
  checkLatchCompatibility(PSBCompNode *Src, PSBCompNode *Dst, SUArrayRef DstSUs,
                          ViolationPairs &Violations);

  void flushViolations(ViolationPairs &Violations, float Penalty) {
    while (!Violations.empty()) {
      VASTSchedUnit *Src = Violations.back().first,
                    *Dst = Violations.back().second;
      Violations.pop_back();

      updateSchedulingConstraint(Src, Dst, 0, Penalty);
    }
  }

  void updateBindingCost(PSBCompNode *Src, PSBCompNode *Dst, float Cost) {
    if (S != Binding)
      return;

    Src->increaseSchedulingCost(Dst, Cost);
    ++BindingViolation;
  }

  bool alap_less(const VASTSchedUnit *LHS, const VASTSchedUnit *RHS) const {
    if (LHS->getSchedule() < RHS->getSchedule())
      return true;

    if (RHS->getSchedule() < LHS->getSchedule())
      return false;

    // Ascending order using ALAP.
    if (Scheduler.getALAPStep(LHS) < Scheduler.getALAPStep(RHS)) return true;
    if (Scheduler.getALAPStep(LHS) > Scheduler.getALAPStep(RHS)) return false;

    // Tie breaker 1: ASAP.
    if (Scheduler.getASAPStep(LHS) < Scheduler.getASAPStep(RHS)) return true;
    if (Scheduler.getASAPStep(LHS) > Scheduler.getASAPStep(RHS)) return false;

    // Tie breaker 2: Original topological order.
    return LHS->getIdx() < RHS->getIdx();
  }

  void updateSchedulingConstraint(VASTSchedUnit *Src, VASTSchedUnit *Dst,
                                  unsigned C, float Penalty) {
    if (S != Scheduling)
      return;

    if (Penalty > 0)
      Scheduler.addSoftConstraint(Src, Dst, C, Penalty);

    ++ScheduleViolation;
  }

  bool performScheduling(VASTModule &VM);

  bool iterate(VASTModule &VM) {
    ScheduleViolation = BindingViolation = 0;
    // First of all, perform the scheduling.
    if (!performScheduling(VM))
      return false;

    // Perfrom binding after scheduing.
    S = Binding;

    // Check the bindings that are invalided by the scheduler.
    if (checkCompatibility()) {
      // And bind again.
      PSB->performBinding();
      // Perform scheduling after binding.
      S = Scheduling;
      return true;
    }

    return false;
  }
};
}

unsigned
IterativeSchedulingBinding::checkLatchCompatibility(PSBCompNode *Src,
                                                    PSBCompNode *Dst,
                                                    SUArrayRef DstSUs,
                                                    ViolationPairs &Violations) {
  unsigned NegativeSlack = 0;

  typedef PSBCompNode::kill_iterator kill_iterator;
  for (kill_iterator I = Src->kill_begin(), E = Src->kill_end(); I != E; ++I) {
    VASTSchedUnit *SrcKill = lookupSU(*I);

    for (unsigned i = 0; i < DstSUs.size(); ++i) {
      VASTSchedUnit *DstDef = DstSUs[i];
      if (!DstDef->isLatch())
        continue;

      // FIXME: Test dominance after global code motion is enabled.
      if (DstDef->getParent() != SrcKill->getParent())
        continue;

      if (SrcKill->getSchedule() <= DstDef->getSchedule())
        continue;

      DEBUG(dbgs() << "Latch SU not compatible:\n";
      SrcKill->dump();
      DstDef->dump();
      dbgs() << "\n\n");

      Violations.push_back(std::make_pair(SrcKill, DstDef));
      NegativeSlack += SrcKill->getSchedule() - DstDef->getSchedule();
    }
  }

  return NegativeSlack;
}

unsigned IterativeSchedulingBinding::checkCompatibility(PSBCompNode *Src,
                                                        SUArrayRef SrcSUs,
                                                        PSBCompNode *Dst,
                                                        SUArrayRef DstSUs) {
  assert(Src->Inst.IsLauch() == Dst->Inst.IsLauch()
         && "Unexpected binding launch/latch to the same physical unit!");
  // Get the benefit by getting the negative of the cost.
  float BindingBenefit = 0;
  if (Src->countSuccessor(Dst))
    BindingBenefit = - PSB->computeCost(Src, Dst);
  else
    BindingBenefit = - PSB->computeCost(Dst, Src);

  float Penalty = BindingBenefit * ResourceFactor * TotalWeight;

  if (!Src->Inst.IsLauch()) {
    // For latch (that define a variable), the conflict only introduced by
    // scheduling when their are in the same subtree of the dominator tree.
    // Otherwise their live interval will never overlap, because the use of a
    // variable are all dominated by the define, hence the live interval rooted
    // on two node without dominance relationship will never overlap.
    BasicBlock *SrcBlock = Src->getDomBlock(), *DstBlock = Dst->getDomBlock();
    if (SrcBlock == DstBlock) {
      ViolationPairs Src2DstSPairs;
      unsigned Src2DstSlack
        = checkLatchCompatibility(Src, Dst, DstSUs, Src2DstSPairs);
      ViolationPairs Dst2SrcPairs;
      unsigned Dst2SrcSlack
        = checkLatchCompatibility(Dst, Src, SrcSUs, Dst2SrcPairs);

      if (Src2DstSlack && Dst2SrcSlack) {
        if (Src2DstSlack < Dst2SrcSlack) {
          flushViolations(Src2DstSPairs, Penalty);
          return Src2DstSlack;
        }

        flushViolations(Dst2SrcPairs, Penalty);
        return Dst2SrcSlack;
      }

      return 0;
    }

    ViolationPairs Pairs;
    if (DT.dominates(Src->getDomBlock(), Dst->getDomBlock())) {
      if (unsigned Slack = checkLatchCompatibility(Src, Dst, DstSUs, Pairs)) {
        flushViolations(Pairs, Penalty);
        return Slack;
      }
    }

    if (DT.dominates(Dst->getDomBlock(), Src->getDomBlock())) {
      if (unsigned Slack = checkLatchCompatibility(Dst, Src, SrcSUs, Pairs)) {
        flushViolations(Pairs, Penalty);
        return Slack;
      }
    }

    return 0;
  }

  VASTSchedUnit *SrcSU = getLaunch(SrcSUs), *DstSU = getLaunch(DstSUs);
  // The execution time will never overlap if they are located in different BB
  // FIXME: Not true alter global code motion is enabled.
  if (SrcSU->getParent() != DstSU->getParent())
    return 0;

  // Src and Dst is compatible if Dst is scheduled after Src is finished.
  if (SrcSU->getSchedule() + SrcSU->getII() <= DstSU->getSchedule())
    return 0;

  // Or Src is scheduled after Dst is finished.
  if (SrcSU->getSchedule() >= DstSU->getSchedule() + DstSU->getII())
    return 0;
  
  DEBUG(dbgs() << "Launch SU not compatible:\n";
  SrcSU->dump();
  DstSU->dump();
  dbgs() << "\n\n");

  // Apply the constraint in the same way that we assign the linear order.
  if (!alap_less(SrcSU, SrcSU))
    std::swap(SrcSU, SrcSU);

  updateSchedulingConstraint(SrcSU, DstSU, SrcSU->getII(), Penalty);

  return SrcSU->getII();
}

bool
IterativeSchedulingBinding::checkCompatibility(const ClusterType &Cluster) {
  unsigned AnyViolation = false;

  for (unsigned i = 0; i < Cluster.size(); ++i) {
    PSBCompNode *Src = static_cast<PSBCompNode*>(Cluster[i]);
    ArrayRef<VASTSchedUnit*> SrcSUs(IR2SUMap[Src->Inst]);

    // Also check the compatibility between Src and its descendant
    for (unsigned j = i + 1; j < Cluster.size(); ++j)  {
      PSBCompNode *Desc = static_cast<PSBCompNode*>(Cluster[j]);
      ArrayRef<VASTSchedUnit*> DescSUs(IR2SUMap[Desc->Inst]);

      unsigned NegativeSlack = checkCompatibility(Src, SrcSUs, Desc, DescSUs);
      if (NegativeSlack == 0)
        continue;

      AnyViolation |= true;
      // Prevent the this descendant from being bind to the same physical unit
      // with its ancestor. Essentially, we want to separate this node from the
      // cluster.
      for (unsigned k = i; k < j; ++k)  {
        PSBCompNode *Ancestor = static_cast<PSBCompNode*>(Cluster[k]);
        float Penalty = (PerformanceFactor * NegativeSlack) / (j - k);
        updateBindingCost(Ancestor, Desc, Penalty);
      }
    }
  }

  return AnyViolation;
}

bool IterativeSchedulingBinding::performScheduling(VASTModule &VM) {
  // No need to modify the scheduling if the FU compatiblity is preserved.
  if (S == Scheduling && !checkCompatibility())
    return false;

  Scheduler->resetSchedule();

  if (S == Initial) {
    // Build the step variables, and no need to schedule at all if all SUs have
    // been scheduled.
    if (!Scheduler.createLPAndVariables())
      return false;

    Scheduler.addDependencyConstraints();
  }

  TotalWeight = 0.0f;

  Function &F = VM.getLLVMFunction();
  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    DEBUG(dbgs() << "Applying constraints to BB: " << BB->getName() << '\n');
    //BlockFrequency BF = BFI.getBlockFreq(BB);

    float ExitWeigthSum = 0;
    ArrayRef<VASTSchedUnit*> Exits(IR2SUMap[BB->getTerminator()]);
    for (unsigned i = 0; i < Exits.size(); ++i) {
      VASTSchedUnit *BBExit = Exits[i];
      // Ignore the return value latching operation here. We will add the fix
      // timing constraints between it and the actual terminator.
      if (!BBExit->isTerminator()) {
        assert(isa<ReturnInst>(BB->getTerminator()) && "BBExit is not terminator!");
        continue;
      }

      BranchProbability BP = BranchProbability::getOne();
      if (BasicBlock *TargetBB = BBExit->getTargetBlock())
        BP = BPI.getEdgeProbability(BB, TargetBB);

      // BlockFrequency CurBF = BF * BP;
      float ExitWeight = (PerformanceFactor * BP.getNumerator()) / BP.getDenominator();
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

  Scheduler.addObjectCoeff(Scheduler->getExit(), - 1.0 * (TotalWeight /*+ PerformanceFactor*/));

  Scheduler.addSoftConstraints();
  Scheduler.buildOptSlackObject(1.0);

  bool success = Scheduler.schedule();
  assert(success && "SDCScheduler fail!");

  // We had made some changes.
  return success;
}

void VASTScheduling::scheduleGlobal() {
  PreSchedBinding &PSB = getAnalysis<PreSchedBinding>();
  BranchProbabilityInfo &BPI = getAnalysis<BranchProbabilityInfo>();
  BlockFrequencyInfo &BFI = *reinterpret_cast<BlockFrequencyInfo*>(0);
  //getAnalysis<BlockFrequencyInfo>();

  IterativeSchedulingBinding ISB(*G, PSB, *DT, BFI, BPI, IR2SUMap);
  while (ISB.iterate(*VM)) {
    ++NumIterations;
    dbgs() << "Schedule Violations: " << ISB.ScheduleViolation << ' '
           << "Binding Violations:" << ISB.BindingViolation << '\n';
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
  DF = &getAnalysis<DataflowAnnotation>();
  AA = &getAnalysis<AliasAnalysis>();
  LI = &getAnalysis<LoopInfo>();
  DT = &getAnalysis<DominatorTree>();

  buildSchedulingGraph();

  scheduleGlobal();

  fixIntervalForCrossBBChains();

  DEBUG(G->viewGraph());

  emitSchedule();

  return true;
}
