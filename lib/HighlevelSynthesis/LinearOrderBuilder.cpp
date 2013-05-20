//===-- LinearOrderBuilder.cpp - Build the Linear Order for SDC scheduler -===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The linear order builder in this file not only build the "always occur"
// linear orders, but also build the conditional linear order.
//
// In the fine-grain CFG scheduling, the nodes can be scheduled across the
// boundaries, i.e. the terminators(sinks), of the BasicBlocks. In this case, the
// conditional dependencies, which are original implicitly preserved by the
// the dependencies from/to the source/sink of the BasicBlocks, are not
// necessarily preserved anymore. Fortunately, the conditional flow-dependencies
// are preserved by PHIs, however, the conditional "linear order" for ensuring
// functional units usage constraints, require additional processing.
// To prevent the operation in overlapped states from accessing the same
// functional unit, model each access to such functional units as updating,
// i.e. read then write, the status of the functional unit. Then we preform the
// dataflow analysis and calculate the SSA form for these updates. At last, we
// build the dependencies between accesses to the functional units based on the
// def-use chain between the updates in the SSA form.
//
//===----------------------------------------------------------------------===//

#include "SDCScheduler.h"

#include "shang/VASTSeqValue.h"

#include "llvm/Analysis/Dominators.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#define DEBUG_TYPE "shang-linear-order-builder"
#include "llvm/Support/Debug.h"

#include <queue>

using namespace llvm;

namespace {
typedef std::map<Value*, SmallVector<VASTSchedUnit*, 4> > IR2SUMapTy;

typedef SmallPtrSet<BasicBlock*, 32> BBSet;
typedef DomTreeNode::iterator dt_child_iterator;

typedef std::pair<DomTreeNode*, unsigned> DomTreeNodePair;
struct DomTreeNodeCompare {
  bool operator()(const DomTreeNodePair &LHS, const DomTreeNodePair &RHS) {
    return LHS.second < RHS.second;
  }
};

struct alap_less {
  SchedulerBase &S;
  alap_less(SchedulerBase &s) : S(s) {}
  bool operator() (const VASTSchedUnit *LHS, const VASTSchedUnit *RHS) const {
    // Ascending order using ALAP.
    if (S.getALAPStep(LHS) < S.getALAPStep(RHS)) return true;
    if (S.getALAPStep(LHS) > S.getALAPStep(RHS)) return false;

    // Tie breaker 1: ASAP.
    if (S.getASAPStep(LHS) < S.getASAPStep(RHS)) return true;
    if (S.getASAPStep(LHS) > S.getASAPStep(RHS)) return false;

    // Tie breaker 2: Original topological order.
    return LHS->getIdx() < RHS->getIdx();
  }
};

struct SingleFULinearOrder {
  VASTSelector *Sel;
  SchedulerBase &G;
  IR2SUMapTy &IR2SUMap;
  DominatorTree &DT;
  const DenseMap<DomTreeNode*, unsigned> &DomLevels;

  typedef DenseMap<BasicBlock*, SmallVector<VASTSchedUnit*, 8> > DefMapTy;
  DefMapTy DefMap;

  void addVisitingSU(VASTSchedUnit *SU, BasicBlock *BB) {
    DefMap[BB].push_back(SU);
  }

  // Determinate the insertion points for the PHIs.
  void determineInsertionPoint(BBSet &DFBlocks);
  void compuateLiveInBlocks(BBSet &LiveInBlocks);

  // Build the linear order between Join Edges (the edges across the dominance
  // frontier).
  void buildLinearOrderOnJEdge(BasicBlock *DF);

  // Build the linear order between the dominance edges.
  void buildLinearOrderOnDEdge(VASTSchedUnit *FirstSU, BasicBlock *BB);

  // Build the dependencies to a specified scheduling unit.
  void buildLinearOrdingFromDom(VASTSchedUnit *SU, BasicBlock *UseBB);

  void buildLinearOrderInBB(BasicBlock *BB, MutableArrayRef<VASTSchedUnit*> SUs);

  SingleFULinearOrder(VASTSelector *Sel, SchedulerBase &G,
                      IR2SUMapTy &IR2SUMap, DominatorTree &DT,
                      const DenseMap<DomTreeNode*, unsigned> &DomLevels)
    : Sel(Sel), G(G), IR2SUMap(IR2SUMap), DT(DT), DomLevels(DomLevels) {}

  void buildLinearOrder();
};

struct BasicLinearOrderGenerator {
  SchedulerBase &G;
  DominatorTree &DT;
  IR2SUMapTy &IR2SUMap;
  BasicLinearOrderGenerator(SchedulerBase &G, DominatorTree &DT,
                            IR2SUMapTy &IR2SUMap)
    : G(G), DT(DT), IR2SUMap(IR2SUMap) {}

  // The FUs whose accesses need to be synchronized, and the basic blocks in
  // which the FU is accessed.
  DenseMap<VASTSelector*, SingleFULinearOrder*> Builders;
  /// DomLevels - Maps DomTreeNodes to their level in the dominator tree.
  /// Please refer the following paper for more detials.
  ///
  ///   Sreedhar and Gao. A linear time algorithm for placing phi-nodes.
  ///   In Proceedings of the 22nd ACM SIGPLAN-SIGACT Symposium on Principles of
  ///   Programming Languages
  ///   POPL '95. ACM, New York, NY, 62-73.
  ///
  /// Also refer PromoteMemoryToRegister.cpp
  ///
  DenseMap<DomTreeNode*, unsigned> DomLevels;
  void initializeDomTreeLevel();

  void buildFUInfo();

  void buildLinearOrder();

  ~BasicLinearOrderGenerator() { DeleteContainerSeconds(Builders); }
};
}

void BasicLinearOrderGenerator::buildFUInfo() {
  typedef VASTSchedGraph::bb_iterator bb_iterator;
  for (bb_iterator I = G->bb_begin(), E = G->bb_end(); I != E; ++I) {
    BasicBlock *BB = I->first;
    MutableArrayRef<VASTSchedUnit*> SUs(I->second);

    // Iterate the scheduling units in the same BB to assign linear order.
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *SU = SUs[i];

      if (!SU->requireLinearOrder()) continue;

      VASTSeqOp *Op = SU->getSeqOp();
      assert(Op && "Only the SU corresponds to a VASTSeqOp requires"
                   " linear order!");

      VASTSelector *Sel = Op->getSrc(Op->num_srcs() - 1).getSelector();
      SingleFULinearOrder *&S = Builders[Sel];

      // Create the Synchronizer if it is not yet created.
      if (S == 0) S = new SingleFULinearOrder(Sel, G, IR2SUMap, DT, DomLevels);

      // Add the FU visiting information.
      S->addVisitingSU(SU, BB);
    }
  }
}

void SingleFULinearOrder::determineInsertionPoint(BBSet &DFBlocks) {
  // Determine in which blocks the FU's flow is alive.
  SmallPtrSet<BasicBlock*, 32> LiveInBlocks;
  compuateLiveInBlocks(LiveInBlocks);

  // Use a priority queue keyed on dominator tree level so that inserted nodes
  // are handled from the bottom of the dominator tree upwards.
  typedef std::priority_queue<DomTreeNodePair, SmallVector<DomTreeNodePair, 32>,
                              DomTreeNodeCompare>
          IDFPriorityQueue;
  IDFPriorityQueue PQ;

  typedef DefMapTy::const_iterator def_iterator;
  for (def_iterator I = DefMap.begin(), E = DefMap.end(); I != E; ++I) {
    if (DomTreeNode *Node = DT.getNode(I->first))
      PQ.push(std::make_pair(Node, DomLevels.lookup(Node)));
  }

  SmallPtrSet<DomTreeNode*, 32> Visited;
  SmallVector<DomTreeNode*, 32> Worklist;
  while (!PQ.empty()) {
    DomTreeNodePair RootPair = PQ.top();
    PQ.pop();
    DomTreeNode *Root = RootPair.first;
    unsigned RootLevel = RootPair.second;

    // Walk all dominator tree children of Root, inspecting their CFG edges with
    // targets elsewhere on the dominator tree. Only targets whose level is at
    // most Root's level are added to the iterated dominance frontier of the
    // definition set.

    Worklist.clear();
    Worklist.push_back(Root);

    while (!Worklist.empty()) {
      DomTreeNode *Node = Worklist.pop_back_val();
      BasicBlock *BB = Node->getBlock();

      for (succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE;
           ++SI) {
        DomTreeNode *SuccNode = DT.getNode(*SI);

        // Quickly skip all CFG edges that are also dominator tree edges instead
        // of catching them below.
        if (SuccNode->getIDom() == Node)
          continue;

        unsigned SuccLevel = DomLevels.lookup(SuccNode);
        if (SuccLevel > RootLevel)
          continue;

        if (!Visited.insert(SuccNode))
          continue;

        BasicBlock *SuccBB = SuccNode->getBlock();
        if (!LiveInBlocks.count(SuccBB))
          continue;

        // Insert the block into the IDF set, i.e. the blocks into which the PHIs
        // are inserted.
        DFBlocks.insert(SuccBB);

        if (!DefMap.count(SuccBB))
          PQ.push(std::make_pair(SuccNode, SuccLevel));
      }

      for (dt_child_iterator CI = Node->begin(), CE = Node->end();
           CI != CE; ++CI)
        if (!Visited.count(*CI)) Worklist.push_back(*CI);
    }
  }
}

void SingleFULinearOrder::compuateLiveInBlocks(BBSet &LiveInBlocks) {
  // To determine liveness, we must iterate through the predecessors of blocks
  // where the def is live.  Blocks are added to the worklist if we need to
  // check their predecessors.  Start with all the using blocks.
  // Because we *update*, i.e. read then write, the status in each define block,
  // the define block is also a live-in block.
  SmallVector<BasicBlock*, 64> LiveInBlockWorklist;

  typedef DefMapTy::const_iterator def_iterator;
  for (def_iterator I = DefMap.begin(), E = DefMap.end(); I != E; ++I)
    LiveInBlockWorklist.push_back(I->first);

  // Now that we have a set of blocks where the phi is live-in, recursively add
  // their predecessors until we find the full region the value is live.
  while (!LiveInBlockWorklist.empty()) {
    BasicBlock *BB = LiveInBlockWorklist.pop_back_val();

    // The block really is live in here, insert it into the set.  If already in
    // the set, then it has already been processed.
    if (!LiveInBlocks.insert(BB))
      continue;

    // Since the value is live into BB, it is either defined in a predecessor or
    // live into it to.  Add the preds to the worklist unless they are a
    // defining block.
    for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI) {
      BasicBlock *P = *PI;

      // The value is not live into a predecessor if it defines the value.
      if (DefMap.count(P)) continue;

      // Otherwise it is, add to the worklist.
      LiveInBlockWorklist.push_back(P);
    }
  }
}

void
SingleFULinearOrder::buildLinearOrdingFromDom(VASTSchedUnit *SU,
                                              BasicBlock *UseBB) {
  // Traversal the dominator tree bottom up and find the fisrt block in which
  // the FU is visited.
  DefMapTy::iterator at = DefMap.find(UseBB);
  while (at == DefMap.end()) {
    DomTreeNode *IDom = DT.getNode(UseBB)->getIDom();
    // No operation that dominates UseBB accesses the FU.
    if (IDom == 0) return;

    UseBB = IDom->getBlock();
    at = DefMap.find(UseBB);
  }

  ArrayRef<VASTSchedUnit*> SUs(at->second);
  unsigned IntialInterval = 1;
  VASTDep Edge = VASTDep::CreateDep<VASTDep::LinearOrder>(IntialInterval);
  SU->addDep(SUs.back(), Edge);
}

void SingleFULinearOrder::buildLinearOrderOnJEdge(BasicBlock *DF) {
  for (pred_iterator I = pred_begin(DF), E = pred_end(DF); I != E; ++I) {
    BasicBlock *Incoming = *I;

    // Ignore the dominate edges, only handle the join edges here.
    if (DT.dominates(Incoming, DF)) continue;

    // Find the branching operation targeting the dominance frontier, wait until
    // all operations that dominate and reachable (not killed) to the incoming
    // block finish before we leave the block.
    // TODO: We can also insert a "Join" operation, and wait the join operation
    // just before we access the functional unit.
    ArrayRef<VASTSchedUnit*> Exits(IR2SUMap[Incoming->getTerminator()]);
    for (unsigned i = 0; i < Exits.size(); ++i) {
      VASTSchedUnit *Exit = Exits[i];
      assert(Exit->isTerminator() && "Expect terminator!");
      if (Exit->getTargetBlock() != DF) continue;

      buildLinearOrdingFromDom(Exit, Incoming);
      break;
    }
  }
}

void SingleFULinearOrder::buildLinearOrderOnDEdge(VASTSchedUnit *FirstSU,
                                                  BasicBlock *BB) {
  DomTreeNode *IDom = DT.getNode(BB)->getIDom();
  // Ignore the unreachable BB.
  if (IDom == 0) return;

  buildLinearOrdingFromDom(FirstSU, IDom->getBlock());
}

void
SingleFULinearOrder::buildLinearOrderInBB(BasicBlock *BB,
                                          MutableArrayRef<VASTSchedUnit*> SUs) {
  // Sort the schedule units.
  std::sort(SUs.begin(), SUs.end(), alap_less(G));

  VASTSchedUnit *EalierSU = SUs.front();

  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *LaterSU = SUs[i];

    // Build a dependence edge from EalierSU to LaterSU.
    // TODO: Add an new kind of edge: Constraint Edge, and there should be
    // hard constraint and soft constraint.
    unsigned IntialInterval = 1;
    VASTDep Edge = VASTDep::CreateDep<VASTDep::LinearOrder>(IntialInterval);
    LaterSU->addDep(EalierSU, Edge);

    EalierSU = LaterSU;
  }
}

void SingleFULinearOrder::buildLinearOrder() {
  // Build the linear order within each BB.
  typedef DefMapTy::iterator def_iterator;
  for (def_iterator I = DefMap.begin(), E = DefMap.end(); I != E; ++I)
    buildLinearOrderInBB(I->first, I->second);

  // Build the linear order within each BB.
  typedef DefMapTy::iterator def_iterator;
  for (def_iterator I = DefMap.begin(), E = DefMap.end(); I != E; ++I)
    buildLinearOrderOnDEdge(I->second.front(), I->first);

  // The dominance frontiers of DefBlocks.
  BBSet DFBlocks;

  // Build the linear order across the basic block boundaries.
  determineInsertionPoint(DFBlocks);

  // Build the dependencies to ensure the linear orders even states in different
  // blocks may be activated at the same time.
  for (BBSet::iterator I = DFBlocks.begin(), E = DFBlocks.end(); I != E; ++I)
    buildLinearOrderOnJEdge(*I);
}

void BasicLinearOrderGenerator::initializeDomTreeLevel() {
  assert(DomLevels.empty() && "DomLevels had already been initialized!");
  SmallVector<DomTreeNode*, 32> Worklist;

  DomTreeNode *Root = DT.getRootNode();
  DomLevels[Root] = 0;
  Worklist.push_back(Root);

  while (!Worklist.empty()) {
    DomTreeNode *Node = Worklist.pop_back_val();
    unsigned ChildLevel = DomLevels[Node] + 1;
    for (dt_child_iterator CI = Node->begin(), CE = Node->end(); CI != CE; ++CI)
    {
      DomLevels[*CI] = ChildLevel;
      Worklist.push_back(*CI);
    }
  }
}

void BasicLinearOrderGenerator::buildLinearOrder() {
  // Collect the functional units which require dependencies to avoid multiple
  // accesses in the overlap slots, and the blocks in which the FU is accessed.
  buildFUInfo();

  // Calculate the BB to insert PHI nodes for each FU.
  typedef DenseMap<VASTSelector*, SingleFULinearOrder*>::const_iterator
          iterator;
  for (iterator I = Builders.begin(), E = Builders.end(); I != E; ++I) {
    if (DomLevels.empty()) initializeDomTreeLevel();

    SingleFULinearOrder *Builder = I->second;
    Builder->buildLinearOrder();
  }
}

void SDCScheduler::addLinOrdEdge(DominatorTree &DT, IR2SUMapTy &IR2SUMap) {
  buildTimeFrameAndResetSchedule(true);
  BasicLinearOrderGenerator(*this, DT, IR2SUMap).buildLinearOrder();
  G.topologicalSortSUs();
}
