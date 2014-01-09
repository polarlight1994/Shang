//===----- VASTScheduling.cpp - Scheduling Graph on VAST  -------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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

#include "VASTScheduling.h"
#include "ScheduleDOT.h"

#include "vast/Passes.h"
#include "vast/VASTModule.h"
#include "vast/VASTSeqValue.h"
#include "vast/VASTModulePass.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/PostOrderIterator.h"
#define DEBUG_TYPE "vast-scheduling-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(NumBBReassignments,
          "Number of scheduling units that are asssigned to differnt Basic Blocks");
//===----------------------------------------------------------------------===//
VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, Instruction *Inst, Type T,
                             BasicBlock *BB, VASTSeqOp *SeqOp)
  : T(T), II(1), Schedule(0), InstIdx(InstIdx), Inst(Inst),
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

bool VASTSchedUnit::EdgeBundle::hasDataDependency() const {
  VASTDep::Types T = Edges.front().getEdgeType();

  if ((T == VASTDep::ValDep) || (T == VASTDep::MemDep))
    return true;

  for (unsigned i = 1, e = Edges.size(); i != e; ++i) {
    T = Edges[i].getEdgeType();
    if ((T == VASTDep::ValDep) || (T == VASTDep::MemDep))
      return true;
  }

  return false;
}

int VASTSchedUnit::EdgeBundle::getDFLatency() const {
  VASTDep::Types T = Edges.front().getEdgeType();
  int Latency = -1;

  if (T == VASTDep::ValDep)
    Latency = std::max(Edges.front().getLatency(), Latency);

  for (unsigned i = 1, e = Edges.size(); i != e; ++i) {
    T = Edges[i].getEdgeType();
    if (T == VASTDep::ValDep)
      Latency = std::max(Edges[i].getLatency(), Latency);
  }

  return Latency;
}

BasicBlock *
VASTSchedUnit::getFlowDominator(DominatorTree &DT) const {
  assert(!isPHI() && !isBBEntry() && "Cannot calculate Flow Dominator!");

  BasicBlock *Dst = DT.getRoot();

  for (const_dep_iterator I = dep_begin(), E = dep_end(); I != E; ++I) {
    if (I.getDFLatency() < 0 )
      continue;

    const VASTSchedUnit *Dep = *I;

    // Ignore the dependencies from these virtual nodes.
    if (Dep->isEntry() || Dep->isSyncJoin())
      continue;

    BasicBlock *SrcBB = Dep->getParent();
    if (DT.dominates(Dst, SrcBB)) {
      Dst = SrcBB;
      continue;
    }

    assert(DT.dominates(SrcBB, Dst)
           && "Operands not in a path of the dominator tree!");
  }

  return Dst;
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

  if (Inst == 0)
    return BB;

  if (isPHILatch())
    return getIncomingBlock();

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
  else if (isVNode()) OS << "VNode\t";
  else if (isSyncJoin()) OS << "SyncJoin\t";

  OS << (isLaunch() ? "Launch" : "Latch")
     << " Parent: " << getParent()->getName();

  if (Inst) {
    OS << ' ' << *Inst;//Inst->getName();

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

static int idx_less(VASTSchedUnit *const *LHS, VASTSchedUnit *const *RHS) {
  if ((*LHS)->getIdx() < (*RHS)->getIdx()) return -1;
  if ((*LHS)->getIdx() > (*RHS)->getIdx()) return 1;

  return 0;
}

void VASTSchedGraph::sortSUsByIdx() {
  sortSUs(idx_less);
}

VASTSchedUnit *
VASTSchedGraph::createSUnit(BasicBlock *BB, VASTSchedUnit::Type T) {
  assert((T == VASTSchedUnit::BlockEntry || isBBReachable(BB)) &&
         "Cannot create node for unreachable block!");
  VASTSchedUnit *U = new VASTSchedUnit(TotalSUs++, BB, T);
  // Insert the newly create SU before the exit.
  SUnits.insert(SUnits.back(), U);
  // Also put the scheduling unit in the BBMap.
  assert(BB && "Expect a parent BB!");
  assert((T == VASTSchedUnit::BlockEntry || T == VASTSchedUnit::VNode ||
          T == VASTSchedUnit::SyncJoin)
         && "Unexpected type!");

  BBMap[U->getParent()].push_back(U);

  return U;
}

VASTSchedUnit *
VASTSchedGraph::createSUnit(Instruction *Inst, VASTSchedUnit::Type T,
                            BasicBlock *BB, VASTSeqOp *SeqOp) {
  VASTSchedUnit *U = new VASTSchedUnit(TotalSUs++, Inst, T, BB, SeqOp);
  // Insert the newly create SU before the exit.
  SUnits.insert(SUnits.back(), U);
  // Also put the scheduling unit in the BBMap.
  BBMap[U->getParent()].push_back(U);

  return U;
}

bool VASTSchedGraph::reassignParentBB(VASTSchedUnit *SU, BasicBlock *BB,
                                      DominatorTree *DT) {
  VASTSchedUnit *BBEntry = getEntrySU(BB);
  BasicBlock *NewBB = BB;

  unsigned SUSchedule = SU->getSchedule();
  DomTreeNode *Node = DT->getNode(BB);

  // Walk up the dominator tree, until we found a BB with a eariler schedule
  // than the current SU.
  while (SUSchedule < BBEntry->getSchedule()) {
    assert(Node && "Bad scheduling eariler than the entry of CDFG?");
    Node = Node->getIDom();
    NewBB = Node->getBlock();
    BBEntry = getEntrySU(NewBB);
  }

  // The entry BB is not changed at all.
  if (NewBB == BB)
    return false;

  BBMap[NewBB].push_back(SU);
  ++NumBBReassignments;

  return true;
}

void VASTSchedGraph::finalizeScheduling(DominatorTree *DT) {
  typedef std::map<BasicBlock*, std::vector<VASTSchedUnit*> >::iterator
    iterator;

  for (iterator I = BBMap.begin(), E = BBMap.end(); I != E; ++I) {
    BasicBlock *BB = I->first;
    std::vector<VASTSchedUnit*> &SUs = I->second;
    assert(SUs.front()->isBBEntry() && "Bad SU order, did you sort the SUs?");

    for (unsigned i = 1; i < SUs.size(); /*++i*/) {
      if (!(SUs[i]->isVirtual() || reassignParentBB(SUs[i], BB, DT))) {
        ++i;
        continue;
      }

      SUs.erase(SUs.begin() + i);
    }
  }
}

void VASTSchedGraph::viewGraph() {
  ViewGraph(this, "SchedulingGraph");
}

void
VASTSchedGraph::verifyBBEntry(const VASTSchedUnit *SU) const {
  BasicBlock *BB = SU->getParent();
  std::set<BasicBlock*> Preds;

  for (pred_iterator I = pred_begin(BB), E = pred_end(BB); I != E; ++I) {
    BasicBlock *Pred = *I;

    if (isBBReachable(Pred))
      Preds.insert(Pred);
  }

  typedef VASTSchedUnit::const_dep_iterator dep_iterator;
  for (dep_iterator DI = SU->dep_begin(), DE = SU->dep_end(); DI != DE; ++DI) {
    if (DI.getEdgeType() != VASTDep::Conditional)
      continue;

    const VASTSchedUnit *Dep = *DI;

    if (!Dep->isTerminator())
      continue;

    assert(Dep->getTargetBlock() == BB && "Bad terminator!");

    Preds.erase(Dep->getParent());
  }

  if (LLVM_LIKELY(Preds.empty()))
    return;

  DEBUG(dbgs() << "Predecessors of BB " << BB->getName() << " missing: ";
  typedef std::set<BasicBlock*>::iterator iterator;
  for (iterator I = Preds.begin(), E = Preds.end(); I != E; ++I)
    dbgs() << (*I)->getName() << ", ";
  );

  llvm_unreachable("Incomplete CFG dependencies in scheduling graph!");
}

void VASTSchedGraph::verify() const {
  if (LLVM_UNLIKELY(getEntry()->use_empty() && !getEntry()->dep_empty()))
    llvm_unreachable("Broken dependencies on Entry!");

  if (LLVM_UNLIKELY(getExit()->dep_empty() && !getExit()->use_empty()))
    llvm_unreachable("Broken dependencies on Exit!");

  for (const_iterator I = llvm::next(SUnits.begin()), E = SUnits.back();
       I != E; ++I) {
    if (LLVM_UNLIKELY(I->dep_empty() || I->use_empty()))
      llvm_unreachable("Broken dependencies!");

    if (I->isBBEntry())
      verifyBBEntry(I);
  }
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

    ++WorkStack.back().second;

    if (I.getEdgeType() == VASTDep::Synchronize ||
        I.getEdgeType() == VASTDep::Conditional)
      continue;

    VASTSchedUnit *Child = *I;

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

  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    Visited.clear();
    BasicBlock *BB = *I;

    bb_iterator at = BBMap.find(BB);
    if (at == BBMap.end())
      continue;

    MutableArrayRef<VASTSchedUnit*> SUs(at->second);
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
  AU.addRequired<Dataflow>();
  // AU.addRequired<PreSchedBinding>();
  // AU.addPreserved<PreSchedBinding>();
  AU.addRequired<AliasAnalysis>();
  AU.addRequired<DominatorTree>();
  AU.addRequired<PostDominatorTree>();
  AU.addRequired<LoopInfo>();
  AU.addRequired<BranchProbabilityInfo>();
  AU.addRequired<BlockFrequencyInfo>();
}

INITIALIZE_PASS_BEGIN(VASTScheduling,
                      "vast-scheduling", "Perfrom Scheduling on the VAST",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(PreSchedBinding)
  INITIALIZE_PASS_DEPENDENCY(DataflowAnnotation)
  INITIALIZE_PASS_DEPENDENCY(LoopInfo)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
  INITIALIZE_PASS_DEPENDENCY(PostDominatorTree)
  INITIALIZE_PASS_DEPENDENCY(BranchProbabilityInfo)
  INITIALIZE_PASS_DEPENDENCY(BlockFrequencyInfo)
INITIALIZE_PASS_END(VASTScheduling,
                    "vast-scheduling", "Perfrom Scheduling on the VAST",
                    false, true)

Pass *vast::createVASTSchedulingPass() {
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

    if (isa<BasicBlock>(V) && CurSU->isBBEntry())
      return CurSU;

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

void VASTScheduling::buildFlowDependencies(VASTSchedUnit *DstU, DataflowValue Src,
                                           Dataflow::delay_type delay_dist) {
  assert(Src && "Not a valid source!");
  assert((!isa<Instruction>(Src)
          || DT->dominates(cast<Instruction>(Src)->getParent(), DstU->getParent()))
         && "Flow dependency should be a dominance edge!");
  assert((!isa<BasicBlock>(Src)
          || DT->dominates(cast<BasicBlock>(Src), DstU->getParent()))
         && "Flow dependency should be a dominance edge!");

  float delay = delay_dist.expected();

  VASTSchedUnit *SrcSU = 0;
  Instruction *SrcInst = dyn_cast<Instruction>(Src);

  if (!isChainingCandidate(Src)) {
    // Get the latch SU, if source cannot be chained.
    SrcSU = getFlowDepSU(Src);
    if (Src.IsLauch()) {
      float DelayFromLaunch = DF->getDelayFromLaunch(SrcInst).expected();
      delay -= std::max(1.0f, DelayFromLaunch);
      // Do not produce a negative delay!
      delay = std::max(0.0f, delay);
    } else if (SrcSU->isBBEntry()) {
      // There is already 1 cycle slack from the enable signal to DstU.
      delay = std::max(0.0f, delay - 1.0f);
    } else if (DF->getSlackFromLaunch(SrcInst).expected() > delay) {
      // Try to fold the delay of current pipeline stage to the previous pipeline
      // stage, if the previous pipeline stage has enough slack.
      delay = 0.0f;
    }
  } else {
    bool IsChainingEnableOnEdge
      = DF->getDelayFromLaunch(SrcInst).expected() <= 1.0f &&
        isChainingCandidate(DstU->getInst());

    if (Src.IsLauch()) {
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
  DstU->addDep(SrcSU, VASTDep::CreateFlowDep(ceil(delay), 0));
}

void VASTScheduling::buildFlowDependencies(Instruction *Inst, VASTSchedUnit *U) {
  Dataflow::SrcSet Srcs;

  DF->getFlowDep(U->getSeqOp(), Srcs);

  typedef Dataflow::SrcSet::iterator src_iterator;
  // Also calculate the path for the guarding condition.
  for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    if (Instruction *Inst = dyn_cast<Instruction>(I->first)) {
      if (DF->isBlockUnreachable(Inst->getParent()) || !IR2SUMap.count(Inst))
        continue;
    } else if (isa<BasicBlock>(I->first))
      // The dependencies from BasicBlocks are control dependencies, we will
      // calculate them based on post dominance frontier later.
      continue;

    buildFlowDependencies(U, I->first, I->second);
  }
}

void
VASTScheduling::buildFlowDependenciesConditionalInst(Instruction *Inst,
                                                     BasicBlock *Target,
                                                     VASTSchedUnit *U) {
  Dataflow::SrcSet Srcs;
  typedef Dataflow::SrcSet::iterator src_iterator;

  DF->getIncomingFrom(DataflowInst(Inst, false), Target, Srcs);

  // Also calculate the path for the guarding condition.
  for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    if (Instruction *Inst = dyn_cast<Instruction>(I->first)) {
      if (DF->isBlockUnreachable(Inst->getParent()) || !IR2SUMap.count(Inst))
        continue;
    } else if (isa<BasicBlock>(I->first))
      // The dependencies from BasicBlocks are control dependencies, we will
      // calculate them based on post dominance frontier later.
      continue;

    buildFlowDependencies(U, I->first, I->second);
  }
}

void VASTScheduling::buildFlowDependencies(VASTSchedUnit *U) {
  Instruction *Inst = U->getInst();

  if (U->isLaunch()) {
    buildFlowDependencies(Inst, U);
    return;
  }

  if (PHINode *PHI = dyn_cast<PHINode>(Inst)) {
    buildFlowDependenciesConditionalInst(PHI, U->getParent(), U);
    return;
  }

  assert(U->isLatch() && "Unexpected scheduling unit type!");
  if (isa<TerminatorInst>(Inst)) {
    if (U->isTerminator())
      buildFlowDependenciesConditionalInst(Inst, U->getTargetBlock(), U);
    else
      buildFlowDependencies(Inst, U);

    return;
  }

  VASTSeqInst *SeqInst = cast<VASTSeqInst>(U->getSeqOp());
  unsigned Latency = SeqInst->getCyclesFromLaunch();
  VASTSchedUnit *LaunchU = getLaunchSU(Inst);

  if (Latency == 0) {
    assert(isChainingCandidate(Inst) && "Unexpected 0 cycles from launch!");
    float delay = DF->getDelayFromLaunch(Inst).expected();
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
  for (unsigned i = 0, e = SUs.size(); i != e; ++i)
    if (SUs[i]->isBBEntry())
      return SUs[i];

  VASTSchedUnit *Entry = G->createSUnit(BB, VASTSchedUnit::BlockEntry);

  // Also constraint the BB entry by the entry of the whole scheduling graph.
  // But this is actually not needed because we will set a lower bound on the
  // scheduling variable in the SDC scheduler.
  //Entry->addDep(G->getEntry(), VASTDep::CreateCtrlDep(0));

  if (SUs.empty()) {
    assert(pred_begin(BB) == pred_end(BB)
           && "No entry block do not have any predecessor?");
    // Dependency from the BB entry is not conditional.
    Entry->addDep(G->getEntry(), VASTDep::CreateFixTimingConstraint(0));
  }

  // Add the entry to the mapping.
  SUs.push_back(Entry);

  // Also create the SUnit for the PHI Nodes.
  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(); isa<PHINode>(I); ++I) {
    PHINode *PN = cast<PHINode>(I);
    IR2SUMapTy::iterator J = IR2SUMap.find(PN);

    // Ignore the dead PHINodes.
    if (J == IR2SUMap.end())
      continue;

    VASTSchedUnit *U = G->createSUnit(PN, VASTSchedUnit::SyncJoin, 0, 0);

    // No need to add the dependency edges from the incoming values, because
    // the SU is anyway scheduled to the same slot as the entry of the BB.
    // And we will build the conditional dependencies for the conditional
    // CFG edge between BBs.
    J->second.push_back(U);
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
    VASTSlot *SubGrp = *SI;

    // Collect the operation in the current slot and the subgroups.
    Ops.insert(Ops.end(), SubGrp->op_begin(), SubGrp->op_end());
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

      VASTSchedUnit::Type T = SeqInst->isLatch() ? VASTSchedUnit::Latch
                                                 : VASTSchedUnit::Launch;
      if (isa<PHINode>(Inst)) {
        U = G->createSUnit(Inst, T, BB, SeqInst);
      } else {
        U = G->createSUnit(Inst, T, 0, SeqInst);
      }

      buildFlowDependencies(U);

      // No need to add control dependencies even the instruction have side
      // effect, we are going to calculate the controlling block later.
      // if (Inst->mayHaveSideEffects())
      //   U->addDep(BBEntry, VASTDep::CreateCtrlDep(0));

      if (U->isPHILatch()) {
        // Since the PHI latches are predicated, their depends on the parent BB,
        // i.e. the PHI latches are implicitly guard by the 'condition' of their
        // parent BB.
        U->addDep(BBEntry, VASTDep::CreateCtrlDep(0));
      }

      IR2SUMap[Inst].push_back(U);
      continue;
    }

    if (VASTSlotCtrl *SlotCtrl = dyn_cast<VASTSlotCtrl>(Op)) {
      if (SlotCtrl->isBranch()) {
        // Handle the branch.
        BasicBlock *TargetBB = SlotCtrl->getTargetSlot()->getParent();
        VASTSchedUnit *U =
          G->createSUnit(Inst, VASTSchedUnit::Latch, TargetBB, SlotCtrl);
        IR2SUMap[Inst].push_back(U);
        // Also map the target BB to this terminator.
        IR2SUMap[TargetBB].push_back(U);

        buildFlowDependencies(U);

        // The branch operations are predicated, hence their depends on their
        // parent BB.
        U->addDep(BBEntry, VASTDep::CreateCtrlDep(0));
        continue;
      }

      // This is a wait operation.
      VASTSchedUnit *U = G->createSUnit(Inst, VASTSchedUnit::Latch, BB, SlotCtrl);
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

void VASTScheduling::tightReturns(BasicBlock *BB) {
  TerminatorInst *Inst = BB->getTerminator();

  if (!(isa<UnreachableInst>(Inst) || isa<ReturnInst>(Inst)))
    return;

  ArrayRef<VASTSchedUnit*> SUs(IR2SUMap[Inst]);
  assert(!SUs.empty() && "Scheduling Units for terminator not built?");
  VASTSchedUnit *LastSU = SUs[0];

  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *U = SUs[i];
    U->addDep(LastSU, VASTDep::CreateFixTimingConstraint(0));
    LastSU = U;
  }

  G->getExit()->addDep(LastSU, VASTDep::CreateCtrlDep(0));
}

void VASTScheduling::buildSyncEdgeForPHIs(BasicBlock *BB) {
  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(); isa<PHINode>(I); ++I) {
    PHINode *PN = cast<PHINode>(I);

    IR2SUMapTy::iterator J = IR2SUMap.find(PN);

    // Ignore the dead PHINodes.
    if (J == IR2SUMap.end())
      continue;

    VASTSchedUnit *PHIJoin = 0;
    // Find the join node
    ArrayRef<VASTSchedUnit*> SUs(J->second);
    for (unsigned i = 0; i < SUs.size(); ++i) {
      if (!SUs[i]->isPHI())
        continue;
      
      PHIJoin = SUs[i];
      break;
    }

    // Build the dependence edge to the join node.
    assert(PHIJoin && "PHIJoin not built?");

    for (unsigned i = 0; i < SUs.size(); ++i) {
      if (SUs[i]->isPHI())
        continue;

      PHIJoin->addDep(SUs[i], VASTDep::CreateSyncDep());
    }
  }
}

void VASTScheduling::fixSchedulingGraph() {
  VASTSchedUnit *Exit = G->getExit();
  // Try to fix the dangling nodes.
  typedef VASTSchedGraph::iterator iterator;
  for (iterator I = llvm::next(G->begin()), E = Exit; I != E; ++I) {
    VASTSchedUnit *U = I;

    // Ignore the entries.
    if (U->isBBEntry())
      continue;

    // Terminators will be handled later.
    if (U->isTerminator())
      continue;

    // Ignore the virtual nodes.
    if (U->isVirtual() && !U->isPHI())
      continue;

    Instruction *Inst = U->getInst();
    // Returns will be handled later, too.
    if (Inst && (isa<UnreachableInst>(Inst) || isa<ReturnInst>(Inst)))
      continue;

    // At least constrain the scheduling unit with something.
    if (U->use_empty())
      Exit->addDep(U, VASTDep::CreateCtrlDep(0));
  }

  // Also add the dependencies form the return instruction to the exit of
  // the scheduling graph.
  Function &F = *VM->getFunction();

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    buildSyncEdgeForPHIs(I);
    tightReturns(I);
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

void VASTScheduling::buildSchedulingGraph() {
  // Build the scheduling units according to the original scheduling.
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >
    RPO(VM->getStartSlot());

  typedef
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >::rpo_iterator
  slot_top_iterator;

  for (slot_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    buildSchedulingUnits(*I);

  buildMemoryDependencies();

  buildControlFlowEdges();

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
  Function &F = *VM.getFunction();

  OwningPtr<VASTSchedGraph> GPtr(new VASTSchedGraph(F));
  G = GPtr.get();

  // Initialize the analyses
  DF = &getAnalysis<Dataflow>();
  DT = &getAnalysis<DominatorTree>();
  LI = &getAnalysis<LoopInfo>();

  buildSchedulingGraph();

  scheduleGlobal();

#ifndef NDEBUG
  verifyCrossBBDeps();
#endif

  DEBUG(F.viewCFGOnly());
  DEBUG(G->viewGraph());

  emitSchedule();

  return true;
}
