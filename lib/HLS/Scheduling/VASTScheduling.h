//===------- VASTScheduling.h - Scheduling Graph on VAST  -------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the VASTSSchedUnit and VASTSchedGraph. With these class we
// perform scheduling in High-level Synthesis. Please note that the scheduling
// is based on LLVM IR. After scheduling we will annotate the schedule of the
// LLVM Instructions in form of metadata. And we will rebuild the VAST according
// to the schedule.
//
//===----------------------------------------------------------------------===//
//

#ifndef VAST_SCHEDULING_H
#define VAST_SCHEDULING_H

#include "vast/Dataflow.h"
#include "vast/VASTSeqOp.h"
#include "vast/VASTModulePass.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/ilist_node.h"
#include <map>

namespace llvm {
class raw_ostream;
class Loop;
class DominatorTree;
class AliasAnalysis;
class LoopInfo;
class BranchProbabilityInfo;
}

namespace vast {
using namespace llvm;

class VASTModule;
class SchedulerBase;

class VASTDep {
public:
  enum Types {
    ValDep      = 0,
    MemDep      = 1,
    CtrlDep     = 2,
    FixedTiming = 3,
    LinearOrder = 4,
    Conditional = 5,
    Synchronize = 6,
    // Generic dependency that may form cycles.
    Generic     = 7
  };
private:
  uint8_t EdgeType : 3;
  // Iterate distance.
  int16_t Distance : 13;
  // The latancy of this edge.
  uint16_t Latancy : 13;
  uint16_t ExtraCycles : 3;

public:
  VASTDep(enum Types T, unsigned latancy, int Dst, unsigned ExtraCycles)
    : EdgeType(T), Distance(Dst), Latancy(latancy), ExtraCycles(ExtraCycles) {}

  Types getEdgeType() const { return Types(EdgeType); }

  inline int getExtraCycles() const {
    return unsigned(ExtraCycles);
  }

  // Compute the latency considering the distance between iterations in a loop.
  inline int getLatency(unsigned II = 0) const {
    return Latancy - int(II) * getDistance();
  }

  // Get the distance between iterations in a loop.
  int getDistance() const {
    return Distance;
  }
  bool isLoopCarried() const {
    return getDistance() != 0;
  }

  inline bool operator==(const VASTDep &RHS) const {
    return RHS.EdgeType == EdgeType
           && RHS.Latancy == Latancy
           && RHS.Distance == Distance;
  }

  void print(raw_ostream &OS) const;

  template<int DISTANCE>
  static VASTDep CreateMemDep(int Latency) {
    return VASTDep(MemDep, Latency, DISTANCE);
  }

  static VASTDep CreateMemDep(int Latency, int Distance) {
    return VASTDep(MemDep, Latency, Distance, 0);
  }

  static VASTDep CreateFlowDep(int Latency, unsigned ExtraCycles) {
    return VASTDep(ValDep, Latency, 0, ExtraCycles);
  }

  static VASTDep CreateCtrlDep(int Latency) {
    return VASTDep(CtrlDep, Latency, 0, 0);
  }

  static VASTDep CreateFixTimingConstraint(int Latency) {
    return VASTDep(FixedTiming, Latency, 0, 0);
  }

  template<Types Type>
  static VASTDep CreateDep(int Latency) {
    return VASTDep(Type, Latency, 0, 0);
  }

  static VASTDep CreateCndDep() {
    return VASTDep(Conditional, 0, 0, 0);
  }

  static VASTDep CreateSyncDep() {
    return VASTDep(Synchronize, 0, 0, 0);
  }
};

//
class VASTSchedUnit : public ilist_node<VASTSchedUnit> {
public:
  enum Type {
    // Entry and Exit of the whole scheduling graph.
    Entry, Exit,
    // The supper source node of the basic block.
    BlockEntry,
    Launch, Latch,
    // Generic prupose virtual node
    VNode,
    // Synchronization nodes.
    SyncJoin,
    // Invalide node for the ilist sentinel
    Invalid
  };
private:
  const Type T : 4;
  // Initial Ineterval of the functional unit.
  uint32_t II : 8;
  uint32_t Schedule : 20;
  uint32_t InstIdx;

  // EdgeBundle allow us add/remove edges between VASTSUnit more easily.
  struct EdgeBundle {
    SmallVector<VASTDep, 1> Edges;

    explicit EdgeBundle(VASTDep E) : Edges(1, E)  {}

    bool isLegalToAddFixTimngEdge(int Latency) const;

    void addEdge(VASTDep NewEdge);
    VASTDep getEdge(unsigned II = 0) const;

    // TODO: Print all edges in the edge bundle.
    void printDetials(raw_ostream &OS) const;

    int getDFLatency() const;
    bool hasDataDependency() const;
  };

  typedef DenseMap<VASTSchedUnit*, EdgeBundle> DepSet;
public:
  template<class IteratorType, bool IsConst>
  class VASTSchedUnitDepIterator : public IteratorType {
    typedef VASTSchedUnitDepIterator<IteratorType, IsConst> Self;
    typedef typename conditional<IsConst, const VASTSchedUnit, VASTSchedUnit>::type
            NodeType;
    typedef typename conditional<IsConst, const VASTDep, VASTDep>::type
            EdgeType;
    typedef typename conditional<IsConst, const EdgeBundle, EdgeBundle>::type
            EdgeBundleType;

    EdgeBundleType &getEdgeBundle() const {
      return IteratorType::operator->()->second;
    }
  public:
    VASTSchedUnitDepIterator(IteratorType i) : IteratorType(i) {}

    NodeType *operator*() const {
      return IteratorType::operator->()->first;
    }

    NodeType *operator->() const { return operator*(); }
    EdgeType getEdge(unsigned II = 0) const {
      return getEdgeBundle().getEdge(II);
    }

    Self& operator++() {                // Preincrement
      IteratorType::operator++();
      return *this;
    }

    Self operator++(int) { // Postincrement
      return IteratorType::operator++(0);
    }

    // Forwarding the function from the Edge.
    VASTDep::Types getEdgeType(unsigned II = 0) const {
      return getEdge(II).getEdgeType();
    }
    inline int getLatency(unsigned II = 0) const {
      return getEdge(II).getLatency(II);
    }
    bool isLoopCarried(unsigned II = 0) const {
      return getEdge(II).isLoopCarried();
    }
    bool isCrossBB() const {
      return IteratorType::operator->()->second.IsCrossBB;
    }

    int getDistance(unsigned II = 0) const { return getEdge(II).getDistance(); }

    int getDFLatency() const { return getEdgeBundle().getDFLatency(); }
    bool hasDataDependency() const {
      return getEdgeBundle().hasDataDependency();
    }
  };

  typedef VASTSchedUnitDepIterator<DepSet::const_iterator, true> const_dep_iterator;
  typedef VASTSchedUnitDepIterator<DepSet::iterator, false> dep_iterator;
private:
  // Remember the dependencies of the scheduling unit.
  DepSet Deps;

  // The scheduling units that using this scheduling unit.
  typedef std::set<VASTSchedUnit*> UseListTy;
  UseListTy UseList;

  void addToUseList(VASTSchedUnit *User) { UseList.insert(User); }

  Instruction *Inst;
  BasicBlock *BB;
  VASTSeqOp *SeqOp;

  friend struct ilist_sentinel_traits<VASTSchedUnit>;
  friend class VASTSchedGraph;

  /// Finding dependencies edges from a specified node.
  dep_iterator getDepIt(const VASTSchedUnit *A) {
    return Deps.find(const_cast<VASTSchedUnit*>(A));
  }

  const_dep_iterator getDepIt(const VASTSchedUnit *A) const {
    return Deps.find(const_cast<VASTSchedUnit*>(A));
  }
public:
  // Create the virtual SUs.
  VASTSchedUnit(Type T = Invalid, unsigned InstIdx = 0, BasicBlock *Parent = 0);
  VASTSchedUnit(unsigned InstIdx, Instruction *Inst, Type T, BasicBlock *BB,
                VASTSeqOp *SeqOp);
  VASTSchedUnit(unsigned InstIdx, BasicBlock *BB, Type T);

  void setII(unsigned II) {
    this->II = std::max(this->II, II);
  }

  unsigned getII() const { return II; }

  bool isEntry() const { return T == Entry; }
  bool isExit() const { return T == Exit; }
  bool isBBEntry() const { return T == BlockEntry; }
  bool isSync() const { return T == SyncJoin; }
  bool isVNode() const { return T == VNode; }
  bool isSyncJoin() const { return T == SyncJoin; }
  bool isVirtual() const {
    return isEntry() || isExit() || isSync() || isVNode();
  }

  bool isPHI() const {
    return Inst && isa<PHINode>(getInst()) && isSyncJoin();
  }

  bool isPHILatch() const {
    return SeqOp && isa<PHINode>(getInst()) && isLatch();
  }

  Instruction *getInst() const;
  VASTSeqOp *getSeqOp() const { return SeqOp; }

  bool isLatch() const { return T == Latch; }
  bool isLaunch() const { return T == Launch; }
  bool isTerminator() const {
    return SeqOp && isa<VASTSlotCtrl>(SeqOp) && isa<TerminatorInst>(getInst());
  }

  BasicBlock *getIncomingBlock() const {
    assert(isPHILatch() && "Call getIncomingBlock on the wrong type!");
    return BB;
  }

  BasicBlock *getTargetBlock() const {
    assert(isTerminator() && "Call getTargetBlock on the wrong type!");
    return BB;
  }

  bool isLatching(Value *V) const {
    if (!isLatch()) return false;

    VASTSeqInst *SeqOp = dyn_cast_or_null<VASTSeqInst>(getSeqOp());

    if (SeqOp == 0) return false;

    return SeqOp->getValue() == V;
  }

  BasicBlock *getParent() const;

  // Iterators for dependencies.
  dep_iterator dep_begin() { return Deps.begin(); }
  dep_iterator dep_end() { return Deps.end(); }

  const_dep_iterator dep_begin() const { return Deps.begin(); }
  const_dep_iterator dep_end() const { return Deps.end(); }

  bool dep_empty() const { return Deps.empty(); }

  /// Iterators for the uses.
  typedef UseListTy::iterator use_iterator;
  use_iterator use_begin() { return UseList.begin(); }
  use_iterator use_end() { return UseList.end(); }

  typedef UseListTy::const_iterator const_use_iterator;
  const_use_iterator use_begin() const { return UseList.begin(); }
  const_use_iterator use_end() const { return UseList.end(); }

  bool use_empty() const { return UseList.empty(); }

  bool isDependsOn(const VASTSchedUnit *A) const {
    return Deps.count(const_cast<VASTSchedUnit*>(A));
  }

  VASTDep getEdgeFrom(const VASTSchedUnit *A, unsigned II = 0) const {
    assert(isDependsOn(A) && "Current atom not depend on A!");
    return getDepIt(A).getEdge(II);
  }

  unsigned getDFLatency(const VASTSchedUnit *A) const {
    assert(isDependsOn(A) && "Current atom not depend on A!");
    int L = getDepIt(A).getDFLatency();
    assert(L >= 0 && "Not a dataflow dependence!");
    return unsigned(L);
  }

  void removeDep(VASTSchedUnit *Src) {
    bool Erased = Deps.erase(Src);
    assert(Erased && "Src not a dependency of the current node!");
    Src->UseList.erase(this);
    (void) Erased;
  }

  // Maintaining dependencies.
  void addDep(VASTSchedUnit *Src, VASTDep NewE) {
    assert(Src != this && "Cannot add self-loop!");
    DepSet::iterator at = Deps.find(Src);

    if (at == Deps.end()) {
      Deps.insert(std::make_pair(Src, EdgeBundle(NewE)));
      Src->addToUseList(this);
    } else {
      int OldLatency = getEdgeFrom(Src).getLatency();
      at->second.addEdge(NewE);
      assert(OldLatency <= getEdgeFrom(Src).getLatency() && "Edge lost!");
      (void) OldLatency;
    }

    assert(getEdgeFrom(Src).getLatency() >= NewE.getLatency()
           && "Edge not isnerted?");
  }

  // SSA Form require that a def dominates all its uses. From the use's point
  // of use, it is dominated by all its operand. At the same time, because the
  // dominators have a tree structures, this means all operands of a use are
  // located in a path in the dominator tree that starts from the root and end
  // at the parent block of the use. We call this path the "operand dominators
  // path". The (data) flow dominator of a node is the parent block of an
  // operand of the node that is nearest to the parent block of the node in the
  // operand dominators path.
  BasicBlock *getFlowDominator(DominatorTree &DT) const;

  /// Return true if the node should be constrained by linear order.
  bool requireLinearOrder() const;

  /// Debug Helper functions.

  /// print - Print out the internal representation of this atom to the
  /// specified stream.  This should really only be used for debugging
  /// purposes.
  void print(raw_ostream &OS) const;

  /// dump - This method is used for debugging.
  ///
  void dump() const;

  /// resetSchedule - Reset the schedule of this scheduling unit.
  ///
  void resetSchedule();

  /// isScheduled - Return true if the scheduling unit is scheduled, false
  /// otherwise.
  ///
  bool isScheduled() const { return Schedule != 0; }

  /// scheduleTo - Schedule the VASTSchedUnit to a specific slot.
  ///
  bool scheduleTo(unsigned Step);

  /// getSchedule - Return the schedule of the current Scheduling Unit.
  ///
  uint32_t getSchedule() const { return Schedule; }

  /// getIdx - Return the index of the current scheduling unit.
  uint32_t getIdx() const { return InstIdx; };

  void viewNeighbourGraph();
};
} // end namespace vast

namespace llvm {
using namespace vast;
template<>
struct GraphTraits<VASTSchedUnit*> {
  typedef VASTSchedUnit NodeType;
  typedef VASTSchedUnit::use_iterator ChildIteratorType;

  static NodeType *getEntryNode(const VASTSchedUnit *N) {
    return const_cast<VASTSchedUnit*>(N);
  }

  static ChildIteratorType child_begin(NodeType *N) {
    return N->use_begin();
  }

  static ChildIteratorType child_end(NodeType *N) {
    return N->use_end();
  }
};
} // end namespace llvm

namespace vast {
using namespace llvm;
// The container of the VASTSUnits
class VASTSchedGraph {
  Function &F;

  typedef iplist<VASTSchedUnit> SUList;
  SUList SUnits;
  unsigned TotalSUs;

  void topsortCone(VASTSchedUnit *Root, std::set<VASTSchedUnit*> &Visited,
                   BasicBlock *BB);

  /// Helper class to arrange the scheduling units according to their parent BB,
  /// we will emit the schedule or build the linear order BB by BB.
  std::map<BasicBlock*, std::vector<VASTSchedUnit*> > BBMap;

  void verifyBBEntry(const VASTSchedUnit *SU) const;

  // Reassign parent BB of a SU according to it's schedule. Return true if the
  // parent BB is actually changed.
  bool reassignParentBB(VASTSchedUnit *SU, BasicBlock *BB, DominatorTree *DT);
public:
  VASTSchedGraph(Function &F);
  ~VASTSchedGraph();

  Function &getFunction() const { return F; }

  VASTSchedUnit *getEntry() { return &SUnits.front(); }
  const VASTSchedUnit *getEntry() const { return &SUnits.front(); }

  VASTSchedUnit *getExit() { return &SUnits.back(); }
  const VASTSchedUnit *getExit() const { return &SUnits.back(); }

  VASTSchedUnit *createSUnit(BasicBlock *BB, VASTSchedUnit::Type T);
  VASTSchedUnit *createSUnit(Instruction *Inst, VASTSchedUnit::Type T,
                             BasicBlock *BB, VASTSeqOp *SeqOp);

  // Iterate over all scheduling units in the scheduling graph.
  typedef SUList::iterator iterator;
  iterator begin() { return SUnits.begin(); }
  iterator end() { return SUnits.end(); }

  typedef SUList::const_iterator const_iterator;
  const_iterator begin() const { return SUnits.begin(); }
  const_iterator end() const { return SUnits.end(); }

  typedef SUList::reverse_iterator reverse_iterator;
  reverse_iterator rbegin() { return SUnits.rbegin(); }
  reverse_iterator rend() { return SUnits.rend(); }

  typedef SUList::const_reverse_iterator const_reverse_iterator;
  const_reverse_iterator rbegin() const { return SUnits.rbegin(); }
  const_reverse_iterator rend() const { return SUnits.rend(); }

  unsigned size() const { return TotalSUs; }

  typedef std::map<BasicBlock*, std::vector<VASTSchedUnit*> >::iterator bb_iterator;
  bb_iterator bb_begin() { return BBMap.begin(); }
  bb_iterator bb_end() { return BBMap.end(); }

  typedef std::map<BasicBlock*, std::vector<VASTSchedUnit*> >::const_iterator
          const_bb_iterator;

  bool isBBReachable(BasicBlock *BB) const {
    return BBMap.count(BB);
  }

  MutableArrayRef<VASTSchedUnit*> getSUInBB(BasicBlock *BB);
  ArrayRef<VASTSchedUnit*> getSUInBB(BasicBlock *BB) const;

  VASTSchedUnit *getEntrySU(BasicBlock *BB) const {
    VASTSchedUnit *Entry = getSUInBB(BB).front();
    assert(Entry->isBBEntry() && "Bad SU order, did you sort the SUs?");
    return Entry;
  }

  // This function do the following things to prepare the scheduling graph for
  // schedule emission:
  // 1. Remove virtual SU before emitting the scheduling.
  // 2. Reassign parent BB of the scheduling unit
  void finalizeScheduling(DominatorTree *DT);

  template<typename T>
  void sortSUs(T F) {
    typedef std::map<BasicBlock*, std::vector<VASTSchedUnit*> >::iterator
      iterator;

    for (iterator I = BBMap.begin(), E = BBMap.end(); I != E; ++I) {
      std::vector<VASTSchedUnit*> &SUs = I->second;

      array_pod_sort(SUs.begin(), SUs.end(), F);
    }
  }

  void sortSUsByIdx();

  /// Fix the scheduling graph after it is built.
  ///
  void prepareForScheduling();

  /// Sort the scheduling units in topological order.
  ///
  void topologicalSortSUs();

  /// Reset the schedule of all the scheduling units in the scheduling graph.
  ///
  void resetSchedule();

  void viewGraph();

  /// verify - Verify the scheduling graph.
  ///
  void verify() const;

  void print(raw_ostream &OS) const;

  void dump() const;
};
} // end namespace vast

namespace llvm {
using namespace vast;

template<>
struct GraphTraits<VASTSchedGraph*> : public GraphTraits<VASTSchedUnit*> {
  typedef VASTSchedUnit NodeType;
  typedef VASTSchedUnit::use_iterator ChildIteratorType;

  static NodeType *getEntryNode(const VASTSchedGraph *G) {
    return const_cast<VASTSchedUnit*>(G->getEntry());
  }

  typedef VASTSchedGraph::iterator nodes_iterator;

  static nodes_iterator nodes_begin(VASTSchedGraph *G) {
    return G->begin();
  }

  static nodes_iterator nodes_end(VASTSchedGraph *G) {
    return G->end();
  }
};
} // end namespace llvm

namespace vast {
using namespace llvm;

class PreSchedBinding;
class PSBCompNode;

class VASTScheduling : public VASTModulePass {
  // Mapping between LLVM IR and VAST IR.
  typedef std::map<Value*, SmallVector<VASTSchedUnit*, 4> > IR2SUMapTy;
  IR2SUMapTy IR2SUMap;
  typedef std::map<Argument*, VASTSeqValue*> ArgMapTy;
  ArgMapTy ArgMap;

  VASTSchedGraph *G;
  Dataflow *DF;
  VASTModule *VM;

  // Analyses for the scheduler
  DominatorTree *DT;
  LoopInfo *LI;

  VASTSchedUnit *getOrCreateBBEntry(BasicBlock *BB);

  void buildFlowDependencies(VASTSchedUnit *DstU, DataflowValue Src,
                             Dataflow::delay_type delay);
  void buildFlowDependencies(Instruction *Inst, VASTSchedUnit *U);
  void buildFlowDependencies(VASTSchedUnit *U);
  void buildFlowDependenciesConditionalInst(Instruction *Inst, BasicBlock *Target,
                                            VASTSchedUnit *U);
  VASTSchedUnit *getFlowDepSU(Value *V);
  VASTSchedUnit *getLaunchSU(Value *V);

  void buildControlFlowEdges();
  void preventInfinitUnrolling(Loop *L);
  // Make sure the incoming value assignment for PHIs are scheduled after all
  // user of PHIs, i.e. make sure the old value on the PHI nodes are read before
  // it is updated.
  // TODO: Relax these dependencies for software pipelining.
  void buildWARDepForPHIs(Loop *L);
  void tightReturns(BasicBlock *BB);
  void buildSyncEdgeForPHIs(BasicBlock *BB);
  void fixSchedulingGraph();

  void buildSchedulingUnits(VASTSlot *S);
  void buildSchedulingGraph();
  /// Build the CFG-wide memory dependencies.
  ///
  void buildMemoryDependencies();

  void scheduleGlobal();
  /// Fix (Check) the interval for cross BB chains.
  ///
  void verifyCrossBBDeps();

  /// Emit the schedule by reimplementing the state-transition graph according
  /// the new scheduling results.
  ///
  void emitSchedule();
public:
  static char ID;
  VASTScheduling();

  void getAnalysisUsage(AnalysisUsage &AU) const;

  bool runOnVASTModule(VASTModule &VM);

  void releaseMemory() {
    IR2SUMap.clear();
    ArgMap.clear();
  }
};
}

#endif