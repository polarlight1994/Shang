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
#include "llvm/Analysis/AliasSetTracker.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
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

struct GlobalFlowAnalyzer {
  explicit GlobalFlowAnalyzer(DominatorTree &DT) : DT(DT) {}

  DominatorTree &DT;
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

  // Determinate the insertion points for the PHIs.
  template<typename DefMapTy>
  void determineInsertionPoint(BBSet &DFBlocks, const DefMapTy &DefMap) {
    initializeDomTreeLevel();

    // Determine in which blocks the FU's flow is alive.
    SmallPtrSet<BasicBlock*, 32> LiveInBlocks;
    compuateLiveInBlocks<DefMapTy>(LiveInBlocks, DefMap);

    // Use a priority queue keyed on dominator tree level so that inserted nodes
    // are handled from the bottom of the dominator tree upwards.
    typedef std::priority_queue<DomTreeNodePair, SmallVector<DomTreeNodePair, 32>,
                                DomTreeNodeCompare>
            IDFPriorityQueue;
    IDFPriorityQueue PQ;

    typedef typename DefMapTy::const_iterator def_iterator;
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

        for (succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE;
             ++SI) {
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
  }

  template<typename DefMapTy>
  void compuateLiveInBlocks(BBSet &LiveInBlocks, const DefMapTy &DefMap) {
    // To determine liveness, we must iterate through the predecessors of blocks
    // where the def is live.  Blocks are added to the worklist if we need to
    // check their predecessors.  Start with all the using blocks.
    // Because we *update*, i.e. read then write, the status in each define
    // block, the define block is also a live-in block.
    SmallVector<BasicBlock*, 64> LiveInBlockWorklist;

    typedef typename DefMapTy::const_iterator def_iterator;
    for (def_iterator I = DefMap.begin(), E = DefMap.end(); I != E; ++I)
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
      for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI) {
        BasicBlock *P = *PI;

        // The value is not live into a predecessor if it defines the value.
        if (DefMap.count(P)) continue;

        // Otherwise it is, add to the worklist.
        LiveInBlockWorklist.push_back(P);
      }
    }
  }
};

template<typename SubClass>
struct GlobalDependenciesBuilderBase  {
  GlobalFlowAnalyzer &GFA;
  IR2SUMapTy &IR2SUMap;
  DenseMap<BasicBlock*, VASTSchedUnit*> SSnks, SSrcs;

  GlobalDependenciesBuilderBase(GlobalFlowAnalyzer &GFA, IR2SUMapTy &IR2SUMap)
    : GFA(GFA), IR2SUMap(IR2SUMap) {}

  typedef DenseMap<BasicBlock*, SmallVector<VASTSchedUnit*, 8> > DefMapTy;
  DefMapTy DefMap;

  // The dominance frontiers of DefBlocks.
  BBSet DFBlocks;

  void addDef(VASTSchedUnit *SU, BasicBlock *BB) {
    DefMap[BB].push_back(SU);
  }

  // Build the dependency between Join Edges (the edges across the dominance
  // frontier).
  void buildDepOnJEdge(BasicBlock *DF) {
    for (pred_iterator I = pred_begin(DF), E = pred_end(DF); I != E; ++I) {
      BasicBlock *Incoming = *I;

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

        buildDepFromDom(Exit, Incoming);
        break;
      }
    }
  }

  // Build the dependency between the dominance edges.
  void buildDepFromDom(VASTSchedUnit *Dst, BasicBlock *SrcBB) {
    // Traversal the dominator tree bottom up and find the fisrt block in which
    // the FU is visited.
    DomTreeNode *Node = GFA.DT.getNode(SrcBB);
    while (Node) {
      BasicBlock *BB = Node->getBlock();
      if (DefMap.count(BB)) {
        VASTSchedUnit *Snk = static_cast<SubClass*>(this)->getSnkAt(BB);
        static_cast<SubClass*>(this)->buildDep(Snk, Dst);
        return;
      }

      if (DFBlocks.count(BB)) {
        static_cast<SubClass*>(this)->buildDepFromDFBlock(Dst, BB);
        return;
      }

      Node = Node->getIDom();
    }
  }

  // Build the dependencies to a specified scheduling unit.
  void buildDepOnDEdge(VASTSchedUnit *Dst, BasicBlock *SrcBB) {
    DomTreeNode *IDom = GFA.DT.getNode(SrcBB)->getIDom();
    // Ignore the unreachable BB.
    if (IDom == 0) return;

    buildDepFromDom(Dst, IDom->getBlock());
  }

  void constructGlobalFlow() {
    // Build the dependency across the basic block boundaries.
    GFA.determineInsertionPoint(DFBlocks, DefMap);

    // Build the dependency within each BB.
    typedef DefMapTy::iterator def_iterator;
    for (def_iterator I = DefMap.begin() , E = DefMap.end(); I != E; ++I) {
      BasicBlock *BB = I->first;
      VASTSchedUnit *Src = static_cast<SubClass*>(this)->getSrcAt(BB);

      // Do not build the edges across the DFBlocks.
      if (DFBlocks.count(BB)) {
        static_cast<SubClass*>(this)->buildDepFromDFBlock(Src, BB);
        continue;
      }

      buildDepOnDEdge(Src, BB);
    }

    // Build the dependencies to ensure the linear orders even states in different
    // blocks may be activated at the same time.
    for (BBSet::iterator I = DFBlocks.begin(), E = DFBlocks.end(); I != E; ++I)
      buildDepOnJEdge(*I);
  }
};

struct SingleFULinearOrder
  : public GlobalDependenciesBuilderBase<SingleFULinearOrder> {

  VASTNode *FU;
  const unsigned Parallelism;
  SchedulerBase &G;
  const DenseMap<BasicBlock*, VASTSchedUnit*> &Returns;

  void buildDep(VASTSchedUnit *Src, VASTSchedUnit *Dst) {
    unsigned IntialInterval = 1;
    VASTDep Edge = VASTDep::CreateDep<VASTDep::LinearOrder>(IntialInterval);
    Dst->addDep(Src, Edge);
  }

  void buildDepFromDFBlock(VASTSchedUnit *Dst, BasicBlock *DFBlock) {
    // At one hand, later we will add edges to make sure the FU accesses will
    // finish before the control flow reach (the entry of) the dominance
    // frontiers (i.e. DFBlocks). At the other hand, all operation is
    // constrained by the entry of their parent basic block, which means
    // (the entry of) the dominance frontiers implicitly predecease all
    // the blocks (as well as the FU access operations inside) dominated by
    // them. Hence, All FU accesses reachable to dominance frontiers
    // implicitly predecease all blocks (as well as the FU access operations
    // inside) dominated by the dominance frontiers, and we do not need to
    // add any dependencies in this case.
  }

  VASTSchedUnit *getSnkAt(BasicBlock *BB) {
    VASTSchedUnit *&Snk = SSnks[BB];

    if (Snk) return Snk;

    // Create the sink node if it is not created yet.
    Snk = G->createSUnit(BB, VASTSchedUnit::Virtual);
    DefMapTy::iterator at = DefMap.find(BB);
    assert(at != DefMap.end() && "Not a define block!");
    // At this point, the intra-BB linear order had already constructed, and
    // hence the back of the SU array is the 'last' N SU in the block.
    ArrayRef<VASTSchedUnit*> SUs(at->second);
    for (unsigned i = std::max<int>(0, SUs.size() - Parallelism), e = SUs.size();
         i < e; ++i)
      Snk->addDep(SUs[i], VASTDep::CreateCtrlDep(0));

    return Snk;
  }

  VASTSchedUnit *getSrcAt(BasicBlock *BB) {
    VASTSchedUnit *&Src = SSrcs[BB];

    if (Src) return Src;

    // Create the source node if it is not created yet.
    Src = G->createSUnit(BB, VASTSchedUnit::Virtual);
    // Make src depends on something.
    Src->addDep(G->getEntrySU(BB), VASTDep::CreateCtrlDep(0));

    DefMapTy::iterator at = DefMap.find(BB);
    assert(at != DefMap.end() && "Not a define block!");
    // At this point, the intra-BB linear order had already constructed, and
    // hence the front of the SU array is the 'first' N SUs in the block.
    ArrayRef<VASTSchedUnit*> SUs(at->second);
    for (unsigned i = 0, e = std::min<unsigned>(SUs.size(), Parallelism);
         i < e; ++i)
      SUs[i]->addDep(Src, VASTDep::CreateCtrlDep(0));

    return Src;
  }

  void buildLinearOrderInBB(MutableArrayRef<VASTSchedUnit*> SUs);

  SingleFULinearOrder(VASTNode *FU, unsigned Parallelism, SchedulerBase &G,
                      IR2SUMapTy &IR2SUMap, GlobalFlowAnalyzer &GFA,
                      DenseMap<BasicBlock*, VASTSchedUnit*> &ReturnBlocks)
    : GlobalDependenciesBuilderBase(GFA, IR2SUMap), FU(FU),
      Parallelism(Parallelism), G(G), Returns(ReturnBlocks) {}

  void buildLinearOrder();

  virtual void dump() const {
    typedef DefMapTy::const_iterator def_iterator;
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
  DenseMap<BasicBlock*, VASTSchedUnit*> ReturnBlocks;
  GlobalFlowAnalyzer GFA;

  BasicLinearOrderGenerator(SchedulerBase &G, DominatorTree &DT,
                            IR2SUMapTy &IR2SUMap)
    : G(G), IR2SUMap(IR2SUMap), GFA(DT) {}

  // The FUs whose accesses need to be synchronized, and the basic blocks in
  // which the FU is accessed.
  DenseMap<VASTNode*, SingleFULinearOrder*> Builders;

  void buildFUInfo();

  void buildLinearOrder();

  ~BasicLinearOrderGenerator() { DeleteContainerSeconds(Builders); }
};
}

void GlobalFlowAnalyzer::initializeDomTreeLevel() {
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
      ReturnBlocks.insert(std::make_pair(BB, Returns.front()));
    }

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

        S = new SingleFULinearOrder(FU, Parallelism, G, IR2SUMap, GFA,
                                    ReturnBlocks);
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
  typedef DefMapTy::iterator def_iterator;
  for (def_iterator I = DefMap.begin(), E = DefMap.end(); I != E; ++I)
    buildLinearOrderInBB(I->second);

  // Create synchronization points at the return block by pretending the return
  // to be a read operation.
  typedef DenseMap<BasicBlock*, VASTSchedUnit*>::const_iterator ret_iterator;
  for (ret_iterator I = Returns.begin(), E = Returns.end(); I != E; ++I) {
    BasicBlock *ReturnBlock = I->first;
    VASTSchedUnit *ReturnSU = I->second;
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
}

//===----------------------------------------------------------------------===//
STATISTIC(NumMemDep, "Number of Memory Dependencies Added");

namespace {
struct AliasRegionDepBuilder
  : public GlobalDependenciesBuilderBase<AliasRegionDepBuilder> {
  VASTSchedGraph &G;

  AliasRegionDepBuilder(VASTSchedGraph &G, GlobalFlowAnalyzer &GFA,
                        IR2SUMapTy &IR2SUMap)
    : GlobalDependenciesBuilderBase(GFA, IR2SUMap), G(G) {}

  void initializeRegion(AliasSet &AS);

  void buildDepFromDFBlock(VASTSchedUnit *Dst, BasicBlock *DFBlock) {}

  VASTSchedUnit *getSnkAt(BasicBlock *BB) {
    VASTSchedUnit *&Snk = SSnks[BB];

    if (Snk) return Snk;

    // Create the sink node if it is not created yet.
    Snk = G.createSUnit(BB, VASTSchedUnit::Virtual);

    DefMapTy::iterator at = DefMap.find(BB);
    assert(at != DefMap.end() && "Not a define block!");
    ArrayRef<VASTSchedUnit*> SUs(at->second);
    for (unsigned i = 0; i < SUs.size(); ++i)
      Snk->addDep(SUs[i], VASTDep::CreateCtrlDep(0));

    return Snk;
  }

  VASTSchedUnit *getSrcAt(BasicBlock *BB) {
    VASTSchedUnit *&Src = SSrcs[BB];

    if (Src) return Src;

    // Create the source node if it is not created yet.
    Src = G.createSUnit(BB, VASTSchedUnit::Virtual);
    // Make src depends on something.
    Src->addDep(G.getEntrySU(BB), VASTDep::CreateCtrlDep(0));

    DefMapTy::iterator at = DefMap.find(BB);
    assert(at != DefMap.end() && "Not a define block!");
    ArrayRef<VASTSchedUnit*> SUs(at->second);
    for (unsigned i = 0; i < SUs.size(); ++i)
      SUs[i]->addDep(Src, VASTDep::CreateCtrlDep(0));

    return Src;
  }

  static void buildDep(VASTSchedUnit *Src, VASTSchedUnit *Dst) {
    Dst->addDep(Src, VASTDep::CreateMemDep(1, 0));
  }
};

/// MemoryDepBuilder - Divide the memories into a set of alias regions, then
/// for each region, perform the flow analysis on the instructions that visit
/// the region and build the dependencies.
struct MemoryDepBuilder {
  VASTSchedGraph &G;
  IR2SUMapTy &IR2SUMap;
  GlobalFlowAnalyzer GFA;
  AliasAnalysis &AA;
  AliasSetTracker AST;

  MemoryDepBuilder(VASTSchedGraph &G, IR2SUMapTy &IR2SUMap, DominatorTree &DT,
                   AliasAnalysis &AA)
    : G(G), IR2SUMap(IR2SUMap), GFA(DT), AA(AA), AST(AA) {}

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
      addDef(SU, Inst->getParent());
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

    AliasRegionDepBuilder Builder(G, GFA, IR2SUMap);
    Builder.initializeRegion(*AS);
    Builder.constructGlobalFlow();
  }
}

void VASTScheduling::buildMemoryDependencies() {
  MemoryDepBuilder(*G, IR2SUMap, *DT, *AA).buildDependencies();
}
