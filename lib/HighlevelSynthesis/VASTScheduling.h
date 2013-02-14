//===------- VASTScheduling.h - Scheduling Graph on VAST  -------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
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

#include "shang/VASTSeqOp.h"

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
class VASTModule;

class VASTDep {
public:
  enum Types {
    ValDep      = 0,
    MemDep      = 1,
    CtrlDep     = 2,
    FixedTiming = 3,
    LinearOrder = 4,
    Conditional = 5,
    Predicate   = 6
  };
private:
  uint8_t EdgeType : 3;
  // Iterate distance.
  int16_t Distance : 13;
  // The latancy of this edge.
  int16_t Latancy;
  int32_t Data;

public:
  VASTDep(enum Types T, int latancy, int Dst)
    : EdgeType(T), Distance(Dst), Latancy(latancy) {}

  Types getEdgeType() const { return Types(EdgeType); }

  // Compute the latency considering the distance between iterations in a loop.
  inline int getLatency(unsigned II = 0) const {
    return Latancy - int(II) * getDistance();
  }

  void setLatency(unsigned latency) { Latancy = latency; }
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
    return VASTDep(MemDep, Latency, Distance);
  }

  static VASTDep CreateFlowDep(int Latency) {
    return VASTDep(ValDep, Latency, 0);
  }

  static VASTDep CreateCtrlDep(int Latency) {
    return VASTDep(CtrlDep, Latency, 0);
  }

  static VASTDep CreateFixTimingConstraint(int Latency) {
    return VASTDep(FixedTiming, Latency, 0);
  }

  template<Types Type>
  static VASTDep CreateDep(int Latency) {
    return VASTDep(Type, Latency, 0);
  }

  static VASTDep CreateCndDep() {
    return VASTDep(Conditional, 0, 0);
  }
};

//
class VASTSchedUnit : public ilist_node<VASTSchedUnit> {
  // TODO: typedef SlotType
  uint16_t Schedule;
  uint16_t InstIdx;

  // EdgeBundle allow us add/remove edges between VASTSUnit more easily.
  struct EdgeBundle {
    SmallVector<VASTDep, 1> Edges;

    explicit EdgeBundle(VASTDep E) : Edges(1, E)  {}

    void addEdge(VASTDep NewEdge);
    VASTDep &getEdge(unsigned II = 0);
    const VASTDep &getEdge(unsigned II = 0) const {
      return const_cast<EdgeBundle*>(this)->getEdge(II);
    }
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
    EdgeType &getEdge(unsigned II = 0) const {
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

  const PointerUnion<BasicBlock*, Instruction*> Ptr;
  const PointerIntPair<BasicBlock*, 1, bool> BB;
  SmallVector<VASTSeqOp*, 4> SeqOps;

  VASTSchedUnit();

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
  VASTSchedUnit(unsigned InstIdx, Instruction *Inst, bool IsLatch, BasicBlock *BB);
  VASTSchedUnit(unsigned InstIdx, BasicBlock *BB);

  bool isEntry() const { return Ptr.is<BasicBlock*>() && Ptr.isNull(); }
  bool isExit() const { return Ptr.is<Instruction*>() && Ptr.isNull(); }
  bool isBBEntry() const { return Ptr.is<BasicBlock*>() && !Ptr.isNull(); }

  Instruction *getInst() const { return Ptr.get<Instruction*>(); }

  bool isLatch() const { return BB.getInt(); }
  bool isLaunch() const { return !BB.getInt(); }

  BasicBlock *getIncomingBlock() const {
    assert(isa<PHINode>(getInst()) && isLaunch()
           && "Call getIncomingBlock on the wrong type!");
    return BB.getPointer();
  }

  BasicBlock *getTargetBlock() const {
    assert(isa<TerminatorInst>(getInst())
           && "Call getTargetBlock on the wrong type!");
    return BB.getPointer();
  }

  bool isLatching(Value *V) const {
    return isLatch() && Ptr.dyn_cast<Instruction*>() == V;
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

  VASTDep &getEdgeFrom(const VASTSchedUnit *A, unsigned II = 0) {
    assert(isDependsOn(A) && "Current atom not depend on A!");
    return getDepIt(A).getEdge(II);
  }

  const VASTDep &getEdgeFrom(const VASTSchedUnit *A, unsigned II = 0) const {
    assert(isDependsOn(A) && "Current atom not depend on A!");
    return getDepIt(A).getEdge(II);
  }

  /// Maintaining dependencies.
  void addDep(VASTSchedUnit *Src, VASTDep NewE) {
    assert(Src != this && "Cannot add self-loop!");
    DepSet::iterator at = Deps.find(Src);

    if (at == Deps.end()) {
      Deps.insert(std::make_pair(Src, EdgeBundle(NewE)));
      Src->addToUseList(this);
      return;
    }

    at->second.addEdge(NewE);
  }

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
  void scheduleTo(unsigned Step);

  /// getSchedule - Return the schedule of the current Scheduling Unit.
  ///
  uint16_t getSchedule() const { return Schedule; }

  uint16_t getIdx() const { return InstIdx; };
};


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

// The container of the VASTSUnits
class VASTSchedGraph {
  typedef iplist<VASTSchedUnit> SUList;
  SUList SUnits;

  void scheduleSDC();

public:
  VASTSchedGraph();
  ~VASTSchedGraph();

  VASTSchedUnit *getEntry() { return &SUnits.front(); }
  const VASTSchedUnit *getEntry() const { return &SUnits.front(); }

  VASTSchedUnit *getExit() { return &SUnits.back(); }
  const VASTSchedUnit *getExit() const { return &SUnits.back(); }

  VASTSchedUnit *createSUnit(BasicBlock *BB) {
    VASTSchedUnit *U = new VASTSchedUnit(SUnits.size(), BB);
    // Insert the newly create SU before the exit.
    SUnits.insert(SUnits.back(), U);
    return U;
  }

  VASTSchedUnit *createSUnit(Instruction *Inst, bool IsLatch, BasicBlock *BB) {
    VASTSchedUnit *U = new VASTSchedUnit(SUnits.size(), Inst, IsLatch, BB);
    // Insert the newly create SU before the exit.
    SUnits.insert(SUnits.back(), U);
    return U;
  }

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

  unsigned size() const { return SUnits.size(); }

  /// Fix the scheduling graph after it is built.
  ///
  void prepareForScheduling();

  /// Schedule the scheduling graph.
  ///
  void schedule();

  /// Reset the schedule of all the scheduling units in the scheduling graph.
  ///
  void resetSchedule();

  /// Emit the schedule by reimplementing the state-transition graph according
  /// the new scheduling results.
  ///
  void emitSchedule(Function &F);

  /// Emit the scheduling units in the same BB.
  ///
  unsigned emitScheduleInBB(MutableArrayRef<VASTSchedUnit*> SUs,
                            unsigned LastSlotNum);

  /// Emit the scheduling units to a specific slot.
  ///
  void emitScheduleAtSlot(MutableArrayRef<VASTSchedUnit*> SUs,
                          unsigned SlotNum, bool IsFirstSlot);

  void viewGraph();

  /// verify - Verify the scheduling graph.
  ///
  void verify() const;
};


/// Helper class to arrange the scheduling units according to their parent BB, 
/// we will emit the schedule or build the linear order BB by BB.
struct SUBBMap {
  std::map<BasicBlock*, std::vector<VASTSchedUnit*> > Map;

  void buildMap(VASTSchedGraph &G);

  MutableArrayRef<VASTSchedUnit*> getSUInBB(BasicBlock *BB);

  template<typename T>
  void sortSUs(T F) {
    typedef std::map<BasicBlock*, std::vector<VASTSchedUnit*> >::iterator
    iterator;

    for (iterator I = Map.begin(), E = Map.end(); I != E; ++I) {
      std::vector<VASTSchedUnit*> &SUs = I->second;

      array_pod_sort(SUs.begin(), SUs.end(), F);
    }
  }
};


template<>
struct GraphTraits<VASTSchedGraph*> : public GraphTraits<VASTSchedUnit*> {
  typedef VASTSchedUnit NodeType;
  typedef VASTSchedUnit::use_iterator ChildIteratorType;

  static NodeType *getEntryNode(const VASTSchedGraph *G) {
    return const_cast<VASTSchedUnit*>(G->getEntry());
  }

  static ChildIteratorType chile_begin(NodeType *N) {
    N->use_begin();
  }

  static ChildIteratorType chile_end(NodeType *N) {
    N->use_end();
  }

  typedef VASTSchedGraph::iterator nodes_iterator;

  static nodes_iterator nodes_begin(VASTSchedGraph *G) {
    return G->begin();
  }

  static nodes_iterator nodes_end(VASTSchedGraph *G) {
    return G->end();
  }
};
}

#endif