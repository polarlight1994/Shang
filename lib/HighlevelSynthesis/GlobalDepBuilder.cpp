//===--------- GlobalDepBuilder.cpp - Build the dependencies on CFG -------===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Build the CFG-wide dependencies based on the flow analysis.
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

#include "shang/Utilities.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTMemoryPort.h"

#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/AliasSetTracker.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/type_traits.h"
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

struct SingleFULinearOrder;

template<typename AccessSetTy, typename DominatorTreeTy>
struct GlobalFlowAnalyzer {
  explicit GlobalFlowAnalyzer(DominatorTreeTy &DT) : DT(*DT.DT) {}
  static const bool IsPostDominators
    = is_same<DominatorTreeTy, PostDominatorTree>::value;
  typedef typename conditional<IsPostDominators,
                               GraphTraits<Inverse<BasicBlock*> >,
                               GraphTraits<BasicBlock*> >::type
          GT;

  typedef typename conditional<IsPostDominators,
                               GraphTraits<BasicBlock*>,
                               GraphTraits<Inverse<BasicBlock*> > >::type
          InverseGT;

  DominatorTreeBase<BasicBlock> &DT;
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
  void initializeDomTreeLevel() {
    if (!DomLevels.empty()) return;

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

  // Determinate the insertion points for the PHIs.
  void determineDominanceFrontiers(BBSet &DFBlocks, const AccessSetTy &DefMap,
                                   const AccessSetTy &UseMap) {
    initializeDomTreeLevel();

    // Determine in which blocks the FU's flow is alive.
    SmallPtrSet<BasicBlock*, 32> LiveInBlocks;
    computeLiveInBlocks(LiveInBlocks, DefMap, UseMap);

    // Use a priority queue keyed on dominator tree level so that inserted nodes
    // are handled from the bottom of the dominator tree upwards.
    typedef std::priority_queue<DomTreeNodePair, SmallVector<DomTreeNodePair, 32>,
                                DomTreeNodeCompare>
            IDFPriorityQueue;
    IDFPriorityQueue PQ;

    typedef typename AccessSetTy::const_iterator def_iterator;
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

      // Walk all dominator tree children of Root, inspecting their CFG edges
      // with targets elsewhere on the dominator tree. Only targets whose level
      // is at most Root's level are added to the iterated dominance frontier of
      // the definition set.
      Worklist.clear();
      Worklist.push_back(Root);

      while (!Worklist.empty()) {
        DomTreeNode *Node = Worklist.pop_back_val();
        BasicBlock *BB = Node->getBlock();

        typedef typename GT::ChildIteratorType child_iterator;
        for (child_iterator SI = GT::child_begin(BB), SE = GT::child_end(BB);
             SI != SE; ++SI) {
          DomTreeNode *SuccNode = DT.getNode(*SI);

          // Quickly skip all CFG edges that are also dominator tree edges
          // instead of catching them below.
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

          // Insert the block into the IDF set, i.e. the blocks into which the
          // PHIs are inserted.
          DFBlocks.insert(SuccBB);

          if (!DefMap.count(SuccBB))
            PQ.push(std::make_pair(SuccNode, SuccLevel));
        }

        for (dt_child_iterator CI = Node->begin(), CE = Node->end();
             CI != CE; ++CI)
          if (!Visited.count(*CI)) Worklist.push_back(*CI);
      }
    }

    DEBUG(dbgs() << "DefBlocks:\n";
    for (def_iterator I = DefMap.begin(), E = DefMap.end(); I != E; ++I)
      dbgs().indent(2) << I->first->getName() << '\n';

    dbgs() << "UseBlocks:\n";
    for (def_iterator I = UseMap.begin(), E = UseMap.end(); I != E; ++I)
      dbgs().indent(2) << I->first->getName() << '\n';

    dbgs() << "DFBlocks:\n";
    typedef BBSet::iterator df_iterator;
    for (df_iterator I = DFBlocks.begin(), E = DFBlocks.end(); I != E; ++I) {
      dbgs().indent(2) << (*I)->getName() << '\n';
    }

    dbgs() << "\n\n";);
  }

  void computeLiveInBlocks(BBSet &LiveInBlocks, const AccessSetTy &DefMap,
                           const AccessSetTy &UseMap) {
    // To determine liveness, we must iterate through the predecessors of blocks
    // where the def is live.  Blocks are added to the worklist if we need to
    // check their predecessors.  Start with all the using blocks.
    // Because we *update*, i.e. read then write, the status in each define
    // block, the define block is also a live-in block.
    SmallVector<BasicBlock*, 64> LiveInBlockWorklist;

    // Add Both Def and Use to liveins, because the WAR dependencies cannot be
    // resolved by renaming here. At the same time, do not add the same block
    // twice.
    typedef typename AccessSetTy::const_iterator iterator;
    for (iterator I = UseMap.begin(), E = UseMap.end(); I != E; ++I)
      LiveInBlockWorklist.push_back(I->first);

    for (iterator I = DefMap.begin(), E = DefMap.end(); I != E; ++I)
      if (!UseMap.count(I->first))
        LiveInBlockWorklist.push_back(I->first);

    // Now that we have a set of blocks where the phi is live-in, recursively
    // add their predecessors until we find the full region the value is live.
    while (!LiveInBlockWorklist.empty()) {
      BasicBlock *BB = LiveInBlockWorklist.pop_back_val();

      // The block really is live in here, insert it into the set.  If already
      // in the set, then it has already been processed.
      if (!LiveInBlocks.insert(BB))
        continue;

      // Since the value is live into BB, it is either defined in a predecessor
      // or live into it to.  Add the preds to the worklist unless they are a
      // defining block.
      typedef typename InverseGT::ChildIteratorType parent_iterator;
      for (parent_iterator PI = InverseGT::child_begin(BB),
           E = InverseGT::child_end(BB); PI != E; ++PI) {
        BasicBlock *P = *PI;

        // The value is not live into a predecessor if it defines the value.
        if (DefMap.count(P)) continue;

        // Otherwise it is, add to the worklist.
        LiveInBlockWorklist.push_back(P);
      }
    }
  }
};

typedef DenseMap<BasicBlock*, SmallVector<VASTSchedUnit*, 8> > AccessMapTy;
typedef GlobalFlowAnalyzer<AccessMapTy, DominatorTree> SyncPointAnalysis;

template<typename SubClass>
struct GlobalDependenciesBuilderBase  {
  AccessMapTy DefMap, UseMap;

  SyncPointAnalysis &SyncAnalysis;

  IR2SUMapTy &IR2SUMap;
  VASTSchedGraph &G;

  GlobalDependenciesBuilderBase(SyncPointAnalysis &SyncAnalysis, IR2SUMapTy &IR2SUMap,
                                VASTSchedGraph &G)
    : SyncAnalysis(SyncAnalysis), IR2SUMap(IR2SUMap), G(G) {}

  ~GlobalDependenciesBuilderBase() {
#ifndef NDEBUG
    verifyCreatedNodes();
#endif
  }

  // The dominance frontiers of DefBlocks, hence there is a pseudo write (def)
  // at the beginning of the block.
  BBSet DFBlocks;

  void addDef(VASTSchedUnit *SU, BasicBlock *BB) {
    DefMap[BB].push_back(SU);
  }

  void addUse(VASTSchedUnit *SU, BasicBlock *BB) {
    UseMap[BB].push_back(SU);
  }

  typedef std::map<BasicBlock*, VASTSchedUnit*> SyncMapTy;
  SyncMapTy Srcs, Snks;

  static void verifyCreatedNode(std::pair<BasicBlock*, VASTSchedUnit*> I) {
    if (LLVM_UNLIKELY(I.second->dep_empty() || I.second->use_empty()))
      llvm_unreachable("Broken dependencies!");
  }

  void verifyCreatedNodes() const {
    std::for_each(Srcs.begin(), Srcs.end(), verifyCreatedNode);
    std::for_each(Snks.begin(), Snks.end(), verifyCreatedNode);
  }

  VASTSchedUnit *getOrCreateSyncNode(BasicBlock *BB, SyncMapTy &Map,
                                     VASTSchedUnit::Type T) {
    VASTSchedUnit *&Node = Map[BB];

    if (Node == NULL)
      Node = G.createSUnit(BB, T);

    return Node;
  }

  VASTSchedUnit *getOrCreateSyncJoin(BasicBlock *BB) {
    VASTSchedUnit *Node = getOrCreateSyncNode(BB, Srcs, VASTSchedUnit::SyncJoin);

    return Node;
  }

  VASTSchedUnit *getOrCreateSyncBarrier(BasicBlock *BB) {
    VASTSchedUnit *Node = getOrCreateSyncNode(BB, Snks, VASTSchedUnit::VNode);

    return Node;
  }

  void
  buildDepFromDFBlock(BasicBlock *BB, SmallVectorImpl<VASTSchedUnit*> &BUSUs) {
    VASTSchedUnit *Entry = getOrCreateSyncJoin(BB);
    // Do not allow the SUs exceeding the entry of dominance frontier. Because
    // the scheduler cannot preserve the inter-BB dependencies in this case.
    while (!BUSUs.empty())
      BUSUs.pop_back_val()->addDep(Entry, VASTDep::CreateCtrlDep(0));
  }

  void collectSUsInBlock(AccessMapTy &Map, BasicBlock *BB,
                         SmallVectorImpl<VASTSchedUnit*> &SUs) {
    AccessMapTy::iterator at = Map.find(BB);
    if (at == Map.end())
      return;

    // At this point, the intra-BB linear order had already constructed, and
    // hence the back of the SU array is the 'last' N SU in the block.
    ArrayRef<VASTSchedUnit*> SUsInBlock(at->second);
    assert(SUsInBlock.size() && "Unexpected empty SU vector!!");
    SUs.append(SUsInBlock.begin(), SUsInBlock.end());
  }

  void buildCtrlDepToSyncPoint(BasicBlock *BB, ArrayRef<VASTSchedUnit*> TDSUs,
                               SmallVectorImpl<VASTSchedUnit*> &BUSUs) {
    std::set<BasicBlock*> Succs(succ_begin(BB), succ_end(BB));
    set_intersect(Succs, DFBlocks);
    if (Succs.empty())
      return;

    VASTSchedUnit *Snk = getOrCreateSyncBarrier(BB);

    typedef std::set<BasicBlock*>::iterator iterator;
    for (iterator I = Succs.begin(), E = Succs.end(); I != E; ++I) {
      BasicBlock *Succ = *I;
      getOrCreateSyncJoin(Succ)->addDep(Snk, VASTDep::CreateSyncDep());
    }

    for (unsigned i = 0, e = BUSUs.size(); i < e; ++i)
      static_cast<SubClass*>(this)->buildDep(TDSUs[i], Snk);

    BUSUs.push_back(Snk);
  }

  void buildDependenciesBottonUp(DomTreeNode *Node) {
    SmallVector<VASTSchedUnit*, 8> BUSUs, TDSUs;
    BasicBlock *BB = Node->getBlock();

    // Collect the SUs for which we are going to build dependencies BottonUp.
    static_cast<SubClass*>(this)->collectSUsForBottonUpDeps(BB, BUSUs);
    if (succ_begin(BB) != succ_end(BB)) {
      static_cast<SubClass*>(this)->collectSUsForTopDownDeps(BB, TDSUs);
      buildCtrlDepToSyncPoint(BB, TDSUs, BUSUs);
    }

    if (BUSUs.empty())
      return;

    if (DFBlocks.count(BB))
      buildDepFromDFBlock(BB, BUSUs);

    Node = Node->getIDom();
    while (Node && !BUSUs.empty()) {
      BasicBlock *BB = Node->getBlock();
      Node = Node->getIDom();

      TDSUs.clear();
      static_cast<SubClass*>(this)->collectSUsForTopDownDeps(BB, TDSUs);

      static_cast<SubClass*>(this)->buildDependencies(BUSUs, TDSUs);

      // If the current BB is synchronization point (i.e. PHI node for flow
      // dependencies), add control dependencies to the SUs that are not
      // consumed by the previous dependencies building function.
      if (DFBlocks.count(BB))
        buildDepFromDFBlock(BB, BUSUs);
    }

    // If we have nodes that reach the root of the dominator tree without
    // finding any dependencies, we can simply making the entry node of the
    // graph as the dependence of these nodes.
    VASTSchedUnit *Entry = G.getEntry();
    while (!BUSUs.empty()) {
      VASTSchedUnit *SU = BUSUs.pop_back_val();

      SU->addDep(Entry, VASTDep::CreateCtrlDep(0));
    }
  }

  void constructGlobalFlow() {
    // Build the dependency across the basic block boundaries.
    SyncAnalysis.determineDominanceFrontiers(DFBlocks, DefMap, UseMap);

    DomTreeNode *Root = SyncAnalysis.DT.getRootNode();

    typedef po_iterator<DomTreeNode*> iterator;
    for (iterator I = po_begin(Root), E = po_end(Root); I != E; ++I) {
      DomTreeNode *Node = *I;
      buildDependenciesBottonUp(Node);
    }
  }
};

struct SingleFULinearOrder
  : public GlobalDependenciesBuilderBase<SingleFULinearOrder> {

  VASTNode *FU;
  const unsigned Parallelism;
  SchedulerBase &G;
  ArrayRef<VASTSchedUnit*> Returns;

  void buildDep(VASTSchedUnit *Src, VASTSchedUnit *Dst) {
    unsigned IntialInterval = 1;
    VASTDep Edge = VASTDep::CreateDep<VASTDep::LinearOrder>(IntialInterval);
    Dst->addDep(Src, Edge);
  }

  void buildDependencies(SmallVectorImpl<VASTSchedUnit*> &BUSUs,
                         ArrayRef<VASTSchedUnit*> TDSUs) {
    if (TDSUs.empty())
      return;

    while (!BUSUs.empty()) {
      VASTSchedUnit *Dst = BUSUs.pop_back_val();

      for (unsigned i = 0; i < TDSUs.size(); ++i)
        buildDep(TDSUs[i], Dst);
    }
  }

  void collectSUsForTopDownDeps(BasicBlock *BB,
                                SmallVectorImpl<VASTSchedUnit*> &TDSUs) {
    AccessMapTy::iterator at = DefMap.find(BB);
    if (at == DefMap.end())
      return;

    // At this point, the intra-BB linear order had already constructed, and
    // hence the back of the SU array is the 'last' N SU in the block.
    ArrayRef<VASTSchedUnit*> SUs(at->second);
    for (unsigned i = std::max<int>(0, SUs.size() - Parallelism), e = SUs.size();
         i < e; ++i)
      TDSUs.push_back(SUs[i]);
  }

  void collectSUsForBottonUpDeps(BasicBlock *BB,
                                 SmallVectorImpl<VASTSchedUnit*> &BUSUs) {
    AccessMapTy::iterator at = DefMap.find(BB);
    if (at == DefMap.end())
      return;

    // At this point, the intra-BB linear order had already constructed, and
    // hence the front of the SU array is the 'first' N SUs in the block.
    ArrayRef<VASTSchedUnit*> SUs(at->second);
    for (unsigned i = 0, e = std::min<unsigned>(SUs.size(), Parallelism);
         i < e; ++i)
      BUSUs.push_back(SUs[i]);
  }

  void buildLinearOrderInBB(MutableArrayRef<VASTSchedUnit*> SUs);

  SingleFULinearOrder(VASTNode *FU, unsigned Parallelism, SchedulerBase &G,
                      IR2SUMapTy &IR2SUMap, SyncPointAnalysis &SyncAnalysis,
                      ArrayRef<VASTSchedUnit*> ReturnBlocks)
    : GlobalDependenciesBuilderBase(SyncAnalysis, IR2SUMap, *G), FU(FU),
      Parallelism(Parallelism), G(G), Returns(ReturnBlocks) {}

  void buildLinearOrder();

  virtual void dump() const {
    typedef AccessMapTy::const_iterator def_iterator;
    dbgs() << "Defs:\n\t";
    for (def_iterator I = DefMap.begin(), E = DefMap.end(); I != E; ++I)
      dbgs() << I->first->getName() << ", ";

    dbgs() << "\nDFs:\n\t";
    typedef BBSet::const_iterator bb_iterator;
    for (bb_iterator I = DFBlocks.begin(), E = DFBlocks.end(); I != E; ++I)
      dbgs() << (*I)->getName() << ", ";
  }
};

struct BasicLinearOrderGenerator {
  SchedulerBase &G;
  IR2SUMapTy &IR2SUMap;
  SmallVector<VASTSchedUnit*, 8> ReturnSUs;
  SyncPointAnalysis SyncAnalysis;

  BasicLinearOrderGenerator(SchedulerBase &G, DominatorTree &DT,
                            IR2SUMapTy &IR2SUMap)
    : G(G), IR2SUMap(IR2SUMap), SyncAnalysis(DT) {}

  // The FUs whose accesses need to be synchronized, and the basic blocks in
  // which the FU is accessed.
  DenseMap<VASTNode*, SingleFULinearOrder*> Builders;

  void buildFUInfo();

  void buildLinearOrder();

  ~BasicLinearOrderGenerator() { DeleteContainerSeconds(Builders); }
};
}

void BasicLinearOrderGenerator::buildFUInfo() {
  typedef VASTSchedGraph::bb_iterator bb_iterator;
  for (bb_iterator I = G->bb_begin(), E = G->bb_end(); I != E; ++I) {
    BasicBlock *BB = I->first;
    TerminatorInst *Inst = BB->getTerminator();

    // Also collect the return operation, we need to wait all operation finish
    // before we return. This can be achieve by simulating a read operation
    // in the return block.
    if ((isa<UnreachableInst>(Inst) || isa<ReturnInst>(Inst))) {
      ArrayRef<VASTSchedUnit*> Returns(IR2SUMap[Inst]);
      ReturnSUs.push_back(Returns.front());
    }
  }

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

      VASTNode *FU = Op->getSrc(0).getSelector()->getParent();
      SingleFULinearOrder *&S = Builders[FU];

      // Create the Synchronizer if it is not yet created.
      if (S == 0) {
        unsigned Parallelism = 1;
        if (VASTMemoryBus *Bus = dyn_cast<VASTMemoryBus>(FU))
          if (Bus->isDualPort()) Parallelism = 2;

        S = new SingleFULinearOrder(FU, Parallelism, G, IR2SUMap, SyncAnalysis,
                                    ReturnSUs);
      }

      // Add the FU visiting information.
      S->addDef(SU, BB);
    }
  }
}

void
SingleFULinearOrder::buildLinearOrderInBB(MutableArrayRef<VASTSchedUnit*> SUs) {
  // Sort the schedule units.
  std::sort(SUs.begin(), SUs.end(), alap_less(G));

  for (unsigned i = Parallelism; i < SUs.size(); ++i) {
    // Allow parallelism of N by building linear order from N operation before
    // the current operation, so that the operation in between EalierSU and
    // LaterSU can execute in parallel with the current operation.
    VASTSchedUnit *LaterSU = SUs[i];
    VASTSchedUnit *EalierSU = SUs[i - Parallelism];
    // Build a dependence edge from EalierSU to LaterSU.
    // TODO: Add an new kind of edge: Constraint Edge, and there should be
    // hard constraint and soft constraint.
    buildDep(EalierSU, LaterSU);
  }
}

void SingleFULinearOrder::buildLinearOrder() {
  // Build the linear order within each BB.
  typedef AccessMapTy::iterator def_iterator;
  for (def_iterator I = DefMap.begin(), E = DefMap.end(); I != E; ++I)
    buildLinearOrderInBB(I->second);

  // Create synchronization points at the return block by pretending the return
  // to be a read operation.
  for (unsigned i = 0; i < Returns.size(); ++i) {
    VASTSchedUnit *ReturnSU = Returns[i];
    BasicBlock *ReturnBlock = ReturnSU->getParent();
    SmallVectorImpl<VASTSchedUnit*> &ExistSUs = DefMap[ReturnBlock];

    // If there is no FU access operation in the block, simply put the ReturnSU
    // to the block and the later algorithm will pretending this operation to be
    // a FU access operation and happily build the linear dependencies to
    // synchronize the 'real' FU access operation before the return operation.
    if (ExistSUs.empty()) {
      ExistSUs.push_back(ReturnSU);
      continue;
    }

    // Otherwise simply build an edge from the last FU access operation in the
    // same block, so that they are finished before we return.
    buildDep(ExistSUs.back(), ReturnSU);
  }

  constructGlobalFlow();
}

void BasicLinearOrderGenerator::buildLinearOrder() {
  // Collect the functional units which require dependencies to avoid multiple
  // accesses in the overlap slots, and the blocks in which the FU is accessed.
  buildFUInfo();

  // Calculate the BB to insert PHI nodes for each FU.
  typedef DenseMap<VASTNode*, SingleFULinearOrder*>::const_iterator
          iterator;
  for (iterator I = Builders.begin(), E = Builders.end(); I != E; ++I) {
    SingleFULinearOrder *Builder = I->second;
    Builder->buildLinearOrder();
  }
}

void SDCScheduler::addLinOrdEdge(DominatorTree &DT, IR2SUMapTy &IR2SUMap) {
  buildTimeFrameAndResetSchedule(true);
  BasicLinearOrderGenerator(*this, DT, IR2SUMap).buildLinearOrder();
  G.topologicalSortSUs();
  buildTimeFrameAndResetSchedule(true);
}

//===----------------------------------------------------------------------===//
STATISTIC(NumMemDep, "Number of Memory Dependencies Added");

namespace {
struct AliasRegionDepBuilder
  : public GlobalDependenciesBuilderBase<AliasRegionDepBuilder> {
    AliasAnalysis &AA;

  AliasRegionDepBuilder(VASTSchedGraph &G, AliasAnalysis &AA,
                        SyncPointAnalysis &SyncAnalysis, IR2SUMapTy &IR2SUMap)
    : GlobalDependenciesBuilderBase(SyncAnalysis, IR2SUMap, G), AA(AA) {}

  void initializeRegion(AliasSet &AS);

  void collectSUsForTopDownDeps(BasicBlock *BB,
                                SmallVectorImpl<VASTSchedUnit*> &TDSUs) {
    collectSUsInBlock(DefMap, BB, TDSUs);
    collectSUsInBlock(UseMap, BB, TDSUs);
  }

  void collectSUsForBottonUpDeps(BasicBlock *BB,
                                 SmallVectorImpl<VASTSchedUnit*> &BUSUs) {
    collectSUsInBlock(DefMap, BB, BUSUs);
    collectSUsInBlock(UseMap, BB, BUSUs);
  }

  void buildDependencies(SmallVectorImpl<VASTSchedUnit*> &BUSUs,
                         ArrayRef<VASTSchedUnit*> TDSUs);

  static void buildDep(VASTSchedUnit *Src, VASTSchedUnit *Dst) {
    if (Dst->isVNode())
      // Need to wait for 1 cycle for the SU in other side of the
      // synchronization point.
      Dst->addDep(Src, VASTDep::CreateCtrlDep(1));
    else
      Dst->addDep(Src, VASTDep::CreateMemDep(1, 0));
  }
};

/// MemoryDepBuilder - Divide the memories into a set of alias regions, then
/// for each region, perform the flow analysis on the instructions that visit
/// the region and build the dependencies.
struct MemoryDepBuilder {
  VASTSchedGraph &G;
  IR2SUMapTy &IR2SUMap;
  SyncPointAnalysis SyncAnalysis;
  AliasAnalysis &AA;
  AliasSetTracker AST;

  MemoryDepBuilder(VASTSchedGraph &G, IR2SUMapTy &IR2SUMap, DominatorTree &DT,
                   AliasAnalysis &AA)
    : G(G), IR2SUMap(IR2SUMap), SyncAnalysis(DT), AA(AA), AST(AA) {}

  void buildLocalDependencies(BasicBlock *BB);
  void buildDependency(Instruction *Src, Instruction *Dst);

  void buildDependencies();
};
}
//===----------------------------------------------------------------------===//
void AliasRegionDepBuilder::initializeRegion(AliasSet &AS) {
  DEBUG(dbgs() << "AST:\n");

  for (AliasSet::iterator AI = AS.begin(), AE = AS.end(); AI != AE; ++AI) {
    Value *V = AI.getPointer();

    DEBUG(dbgs().indent(2) << *V << "\n");

    // Get the loads/stores that refer the pointer
    typedef Value::use_iterator use_iterator;
    for (use_iterator I = V->use_begin(), E = V->use_end(); I != E; ++I) {
      Instruction *Inst = dyn_cast<Instruction>(*I);

      if (Inst == 0 || !isLoadStore(Inst)) continue;

      VASTSchedUnit *SU = IR2SUMap[Inst].front();
      assert(SU->isLaunch() && "Bad scheduling unit type!");
      // Remember the scheduling unit and the corresponding basic block.
      if (Inst->mayWriteToMemory())
        addDef(SU, Inst->getParent());
      else
        addUse(SU, Inst->getParent());
    }
  }

  DEBUG(dbgs() << "\n");
}

//===----------------------------------------------------------------------===//
static
AliasAnalysis::Location getPointerLocation(Instruction *I, AliasAnalysis *AA) {
  if (LoadInst *LI = dyn_cast<LoadInst>(I))
    return AA->getLocation(LI);

  if (StoreInst *SI = dyn_cast<StoreInst>(I))
    return AA->getLocation(SI);

  llvm_unreachable("Unexpected instruction type!");
  return AliasAnalysis::Location();
}

static bool isNoAlias(Instruction *Src, Instruction *Dst, AliasAnalysis *AA) {
  return AA->isNoAlias(getPointerLocation(Src, AA), getPointerLocation(Dst, AA));
}

static
bool isHasDependencies(Instruction *Src, Instruction *Dst, AliasAnalysis *AA) {
  return Src->mayWriteToMemory() && Dst->mayWriteToMemory() &&
         !AA->isNoAlias(getPointerLocation(Src, AA), getPointerLocation(Dst, AA));
}

void
AliasRegionDepBuilder::buildDependencies(SmallVectorImpl<VASTSchedUnit*> &BUSUs,
                                         ArrayRef<VASTSchedUnit*> TDSUs) {
  for (unsigned i = 0, e = BUSUs.size(); i != e; ++i) {
    bool AnyAlias = false;
    VASTSchedUnit *Dst = BUSUs[i];
    bool IsDstVNode = Dst->isVNode();

    for (unsigned j = 0; j < TDSUs.size(); ++j) {
      VASTSchedUnit *Src = TDSUs[j];
      if (!IsDstVNode && !isHasDependencies(Dst->getInst(), Src->getInst(), &AA))
        continue;
      
      buildDep(Src, Dst);
      AnyAlias |= true;
    }

    if (AnyAlias && !IsDstVNode) {
      BUSUs.erase(BUSUs.begin() + i);
      --i;
      --e;
    }
  }
}

void MemoryDepBuilder::buildDependency(Instruction *Src, Instruction *Dst) {
  // If either of them are call instruction, we need a dependencies, because
  // we are not sure the memory locations accessed by the call.
  if (!isCall(Src) && !isCall(Dst)) {
    // Ignore the RAR dependencies.
    if (!Src->mayWriteToMemory() && !Dst->mayWriteToMemory()) return;

    // There is no dependencies if the memory loactions do not alias each other.
    if (isNoAlias(Src, Dst, &AA)) return;
  }

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

void MemoryDepBuilder::buildLocalDependencies(BasicBlock *BB) {
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
      buildDependency(PiorMemInsts[i], Inst);

    PiorMemInsts.push_back(Inst);
    // Collect the instructions to build the alias regions.
    AST.add(Inst);
  }
}

void MemoryDepBuilder::buildDependencies() {
  Function &F = G.getFunction();

  // Build the memory dependencies inside basic blocks.
  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I)
    buildLocalDependencies(I);

  // Build the memory dependencies flow.
  // TODO: Ignore RAR dependencies.
  for (AliasSetTracker::iterator I = AST.begin(), E = AST.end(); I != E; ++I) {
    AliasSet *AS = I;

    // Ignore the set that does not contain any load/store.
    if (AS->isForwardingAliasSet() || !(AS->isMod() || AS->isRef()))
      continue;

    AliasRegionDepBuilder Builder(G, AA, SyncAnalysis, IR2SUMap);
    Builder.initializeRegion(*AS);
    Builder.constructGlobalFlow();
  }
}

void VASTScheduling::buildMemoryDependencies() {
  MemoryDepBuilder MDB(*G, IR2SUMap, *DT, getAnalysis<AliasAnalysis>());
  MDB.buildDependencies();
}

namespace {
struct LoopWARDepBuilder {
  Loop *L;
  DominatorTree *DT;
  IR2SUMapTy &IR2SUMap;

  // Mapping a basic block to the scheduling units that are dominated by the
  // BB and update the PHI node.
  std::map<BasicBlock*, std::set<VASTSchedUnit*> > DomUpdateSUs;
  // User of the PHI node in a BasicBlock.
  std::map<BasicBlock*, std::vector<VASTSchedUnit*> > Users;
  void addUser(VASTSchedUnit *SU) {
    if (SU->isExit()) return;

    BasicBlock *Parent = SU->getParent();
    Users[Parent].push_back(SU);
  }

  void addPHI(VASTSchedUnit *SU) {
    typedef VASTSchedUnit::use_iterator iterator;
    for (iterator I = SU->use_begin(), E = SU->use_end(); I != E; ++I)
      addUser(*I);
  }

  void addUpdater(VASTSchedUnit *SU) {
    DomUpdateSUs[SU->getParent()].insert(SU);
  }

  void buildDepandencies();
  void rememberEdge(BasicBlock *SrcBB, BasicBlock *DstBB);
  VASTSchedUnit *getCFGEdge(BasicBlock *SrcBB, BasicBlock *DstBB);
  bool propagateDomUpdateSUs(BasicBlock *BB);
  void buildDepandencies(BasicBlock *BB);

  LoopWARDepBuilder(Loop *L, DominatorTree *DT, IR2SUMapTy &IR2SUMap)
    : L(L), DT(DT), IR2SUMap(IR2SUMap) {}
};
}

void LoopWARDepBuilder::buildDepandencies(BasicBlock *BB) {
  std::map<BasicBlock*, std::vector<VASTSchedUnit*> >::iterator J
    = Users.find(BB);

  if (J == Users.end())
    return;

  std::map<BasicBlock*, std::set<VASTSchedUnit*> >::iterator K
    = DomUpdateSUs.find(BB);

  if (K == DomUpdateSUs.end())
    return;
  
  std::set<VASTSchedUnit*> &UpdateSUs = K->second;
  ArrayRef<VASTSchedUnit*> CurUsers(J->second);

  typedef std::set<VASTSchedUnit*>::iterator update_iterator;
  for (update_iterator I = UpdateSUs.begin(), E = UpdateSUs.end(); I != E; ++I) {
    VASTSchedUnit *Updater = *I;

    for (unsigned i = 0; i < CurUsers.size(); ++i) {
      VASTSchedUnit *U = CurUsers[i];

      if (U == Updater)
        continue;

      // We may introduce dependence cycles here. But it is ok since the cycle
      // is not a negative cycle. Detecting the cycles require some SCC algorithm,
      // for now, simply create generic dependency so that the scheduler do not
      // complain these cycles.
      Updater->addDep(U, VASTDep::CreateDep<VASTDep::Generic>(0));
    }
  }
  
}

static void VerifyCtrlDep(VASTSchedUnit *SU) {
  BasicBlock *ParentBB = SU->getParent();

  typedef VASTSchedUnit::dep_iterator dep_iterator;
  // Build the constraint for Dst_SU_startStep - Src_SU_endStep >= Latency.
  for (dep_iterator DI = SU->dep_begin(), DE = SU->dep_end(); DI != DE; ++DI) {
    VASTSchedUnit *Dep = *DI;

    if (Dep->isBBEntry() && Dep->getParent() == ParentBB)
      return;
  }

  llvm_unreachable("Control Dependencies for PHI updates missed!");
}

bool LoopWARDepBuilder::propagateDomUpdateSUs(BasicBlock *BB) {
  bool AnyUpdateSU = false;

  for (succ_iterator I = succ_begin(BB), E = succ_end(BB); I != E; ++I) {
    BasicBlock *Succ = *I;

    std::map<BasicBlock*, std::set<VASTSchedUnit*> >::iterator J
      = DomUpdateSUs.find(Succ);
    if (J == DomUpdateSUs.end())
      continue;

    AnyUpdateSU |= true;
    // Make sure the SUs in the update set are dominated by the current BB.
    // If current BB does not dominate the Succ, we cannot directly propagate
    // the DomUpdateSUs of Succ to current BB, because the current BB does not
    // dominate these update SUs. Instead, we add the branching SU targeting
    // Succ to the update set, so that when we build a dependencies to the
    // branching SU, we have an implicitly dependencies to the update SUs though
    // the branching SU.
    //    A
    //    | /
    //    B
    //    |
    // (update set)
    // For example, we need to build dependencies from SUs in A to the SU in
    // update sets dominated by B. However, A does not dominate B, thereby A
    // does not dominate the SUs in update set of B. What we do is we build
    // a dependencies to the entry of B. Because SUs in update set of B will
    // never be scheduled before B (i.e. there are implicit dependencies from
    // B to SUs in the update set.), and we build a dependencies to B,
    // now we have a chain of dependencies from USs in A to update set.
    std::set<VASTSchedUnit*> &SuccUpdateSet = J->second;
    assert(!SuccUpdateSet.empty() && "Unexpected empty update set!");

    if (!DT->properlyDominates(BB, Succ)) {

      // Remember the branch from current BB to Succ BB as the dominated updater.
      // and we will build dependencies from the PHI users in this BB to the
      // branch operation, then we also build a control dependencies from the
      // entry of Succ to all updates that dominated by Succ. By doing this,
      // we build dependency edge User->(branch/entry)->updater.
      if (Succ != L->getHeader())
        rememberEdge(BB, Succ);

#ifndef NDEBUG
      std::for_each(SuccUpdateSet.begin(), SuccUpdateSet.end(), VerifyCtrlDep);
#endif

      continue;
    }


    DomUpdateSUs[BB].insert(SuccUpdateSet.begin(), SuccUpdateSet.end());
  }

  return AnyUpdateSU || DomUpdateSUs.count(BB);
}

VASTSchedUnit *
LoopWARDepBuilder::getCFGEdge(BasicBlock *SrcBB, BasicBlock *DstBB) {
  ArrayRef<VASTSchedUnit*> Exits(IR2SUMap[SrcBB->getTerminator()]);
  for (unsigned i = 0; i < Exits.size(); ++i) {
    VASTSchedUnit *Exit = Exits[i];
    assert(Exit->isTerminator() && "Expect terminator!");
    if (Exit->getTargetBlock() != DstBB) continue;

    return Exit;
  }

  llvm_unreachable("Cannot find edge!");
  return 0;
}

void LoopWARDepBuilder::rememberEdge(BasicBlock *SrcBB, BasicBlock *DstBB) {
  DomUpdateSUs[SrcBB].insert(getCFGEdge(SrcBB, DstBB));
}

void LoopWARDepBuilder::buildDepandencies() {
  BasicBlock *Header = L->getHeader();
  std::vector<std::pair<BasicBlock*, succ_iterator> > WorkStack;
  std::set<BasicBlock*> Visited;

  WorkStack.push_back(std::make_pair(Header, succ_begin(Header)));
  Visited.insert(Header);

  while (!WorkStack.empty()) {
    BasicBlock *CurBlock = WorkStack.back().first;
    succ_iterator CurIt = WorkStack.back().second;

    if (CurIt == succ_end(CurBlock)) {
      WorkStack.pop_back();

      // Update the dominated updating SUs of the current block.
      if (propagateDomUpdateSUs(CurBlock))
        // Build the WAR dependencies.
        buildDepandencies(CurBlock);

      continue;
    }

    BasicBlock *Child = *CurIt;
    ++WorkStack.back().second;

    // Do not visit a block twice!
    if (!Visited.insert(Child).second)
      continue;

    if (!L->contains(Child))
      continue;

    WorkStack.push_back(std::make_pair(Child, succ_begin(Child)));
  }
}

void VASTScheduling::buildWARDepForPHIs(Loop *L) {
  BasicBlock *Header = L->getHeader();

  // Collect the user of the PHI nodes, for them we build the WAR dependencies.
  typedef BasicBlock::iterator iterator;
  for (iterator I = Header->begin(), E = Header->getFirstNonPHI(); I != E; ++I){
    PHINode *PN = cast<PHINode>(I);

    LoopWARDepBuilder WARDepBuilder(L, DT, IR2SUMap);

    IR2SUMapTy::iterator J = IR2SUMap.find(PN);

    // Ignore the dead PHINodes.
    if (J == IR2SUMap.end())
      continue;

    ArrayRef<VASTSchedUnit*> SUs(J->second);
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *SU = SUs[i];
      // Collect the user of the PHI node.
      if (SU->isPHI()) {
        WARDepBuilder.addPHI(SU);
        continue;
      }

      // Collect the update SU from the BackEdge.
      BasicBlock *Incoming = SU->getIncomingBlock();
      if (L->contains(Incoming)) {
        assert(Incoming == SU->getParent()
               && "Bad parent for PHI incoming node!");
        WARDepBuilder.addUpdater(SU);
        continue;
      }

      Loop *ParentLoop = L->getParentLoop();
      if (ParentLoop && ParentLoop->contains(Incoming))
        assert(DT->dominates(Incoming, L->getHeader())
               && "Cannot handle irregular loop!");
    }

    WARDepBuilder.buildDepandencies();
  }
}
