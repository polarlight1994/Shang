//===-------   SIRScheduling.h - Scheduling Graph on SIR  ------*- C++ -*-===//
//
//                      The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the SIRSchedUnit and SIRSchedGraph. With these class we
// perform scheduling in High-level Synthesis. Please note that the scheduling
// is based on LLVM IR. After scheduling we will annotate the schedule of the
// LLVM Instructions in form of metadata. And we will rebuild the SIR according
// to the schedule.
//
//===----------------------------------------------------------------------===//

#ifndef SIR_SCHED_GRAPH
#define SIR_SCHED_GRAPH

#include "sir/SIR.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/Dominators.h"
#include <map>
#include <set>

namespace llvm {
class SIRDep {
public:
  enum Types {
    // Data dependency
    ValDep      = 0,
    // Memory dependency
    MemDep      = 1,
    // Control dependency
    CtrlDep     = 2,
    // Sync dependency
    SyncDep     = 3,
    // Delay dependency
    DelayDep    = 4
  };

private:
  unsigned EdgeType;
  // Iterate distance.
  int Distance;
  // The latency of this edge.
  float Latency;

public:
  SIRDep(enum Types T, float Latency, int Distance)
    : EdgeType(T), Latency(Latency), Distance(Distance) {}

  static SIRDep CreateValDep(float Latency, int Distance = 0) {
    return SIRDep(ValDep, Latency, Distance);
  }

  static SIRDep CreateMemDep(float Latency, int Distance = 0) {
    return SIRDep(MemDep, Latency, Distance);
  }

  static SIRDep CreateCtrlDep(float Latency, int Distance = 0) {
    return SIRDep(CtrlDep, Latency, 0);
  }
  static SIRDep CreateDelayDep(float Latency, int Distance = 0) {
    return SIRDep(DelayDep, Latency, 0);
  }

  static SIRDep CreateSyncDep(float Latency) {
    return SIRDep(SyncDep, 0 - Latency, 0);
  }

  Types getEdgeType() const { return Types(EdgeType); }
  int getDistance() const { return Distance; }
  bool isLoopCarried() const { return getDistance() != 0; }
  inline float getLatency(unsigned II = 0) const {
    // Compute the latency considering the distance in loop.
    return Latency - int(II) * getDistance();
  }
  inline bool operator==(const SIRDep &RHS) const {
    return RHS.EdgeType == EdgeType
      && RHS.Latency == Latency
      && RHS.Distance == Distance;
  }
};

class SIRSchedUnit : public ilist_node<SIRSchedUnit> {
public:
  enum Type {
    // Entry and Exit of the whole scheduling graph
    Entry, Exit,
    // The supper source node of the basic block
    BlockEntry,
    // PHI node
    PHI,
    // Slot transition
    SlotTransition,
    // Normal node for SeqOp
    SeqSU,
    // Normal node for CombOp
    CombSU,
    // PHI pack
    PHIPack,
    // ExitSlot pack
    ExitSlotPack,
    // Memory pack
    MemoryPack,
    // Invalid node for the ilist sentinel
    Invalid
  };

private:
  const Type T;
  // Initial Interval of the functional unit.
  uint32_t II;
  uint16_t Idx;

  float Schedule;
  // The latency of this unit self.
  float Latency;

  // Denote of whether this unit has been scheduled.
  bool IsScheduled;

  // EdgeBundle allow us add/remove edges between SIRSchedUnit more easily.
  struct EdgeBundle {
    SmallVector<SIRDep, 1> Edges;
    explicit EdgeBundle(SIRDep E) : Edges(1, E) {}

    void addEdge(SIRDep NewEdge);
    // Since in all edges in Bundle, only the one with
    // biggest latency matters, so here we return the
    // biggest latency SIRDep.
    SIRDep getEdge(unsigned II = 0) const;

    int getDFLatency() const;
  };
  typedef DenseMap<SIRSchedUnit *, EdgeBundle> DepSet;

public:
  template<class IteratorType, bool IsConst>
  class SIRSchedUnitDepIterator : public IteratorType {
    typedef SIRSchedUnitDepIterator<IteratorType, IsConst> Self;
    typedef typename conditional<IsConst, const SIRSchedUnit, SIRSchedUnit>::type
      NodeType;
    typedef typename conditional<IsConst, const SIRDep, SIRDep>::type
      EdgeType;
    typedef typename conditional<IsConst, const EdgeBundle, EdgeBundle>::type
      EdgeBundleType;

    EdgeBundleType &getEdgeBundle() const {
      return IteratorType::operator->()->second;
    }
  public:
    SIRSchedUnitDepIterator(IteratorType i) : IteratorType(i) {}

    NodeType *operator*() const {
      return IteratorType::operator->()->first;
    }

    NodeType *operator->() const { return operator*(); }
    EdgeType getEdge(unsigned II = 0) const {
      return getEdgeBundle().getEdge(II);
    }

    Self& operator++() {                // Pre-increment
      IteratorType::operator++();
      return *this;
    }

    Self operator++(int) { // Post-increment
      return IteratorType::operator++(0);
    }

    // Forwarding the function from the Edge.
    SIRDep::Types getEdgeType(unsigned II = 0) const {
      return getEdge(II).getEdgeType();
    }
    inline float getLatency(unsigned II = 0) const {
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

  typedef SIRSchedUnitDepIterator<DepSet::const_iterator, true> const_dep_iterator;
  typedef SIRSchedUnitDepIterator<DepSet::iterator, false> dep_iterator;

private:
  // Remember the dependencies of the scheduling unit.
  DepSet Deps;
  // The SeqOps that contained in the SUnit are SeqOps
  // that should be scheduled together. For example,
  // the SeqOps created for MemInst.
  SmallVector<SIRSeqOp *, 4> SeqOps;
  // The CombOp contained in the SUnit.
  Instruction *CombOp;

  BasicBlock *BB;

  friend struct ilist_sentinel_traits<SIRSchedUnit>;
  friend class SIRSchedGraph;

  // The scheduling units that using this scheduling unit,
  // which means these units below depends on this unit.
  typedef std::set<SIRSchedUnit *> UseListTy;
  UseListTy UseList;

  dep_iterator getDepIt(const SIRSchedUnit *A) {
    return Deps.find(const_cast<SIRSchedUnit *>(A));
  }
  const_dep_iterator getDepIt(const SIRSchedUnit *A) const {
    return Deps.find(const_cast<SIRSchedUnit *>(A));
  }

public:
  // The virtual constructor to construct the ilist.
  SIRSchedUnit();
  // The constructor for Virtual SUnit.
  SIRSchedUnit(unsigned Idx, Type T, BasicBlock *BB);
  // The constructor for SeqOps SUnit.
  SIRSchedUnit(unsigned Idx, Type T, BasicBlock *BB,
               SmallVector<SIRSeqOp *, 4>  SeqOps);
  // The constructor for SeqOp SUnit.
  SIRSchedUnit(unsigned Idx, Type T, BasicBlock *BB, SIRSeqOp *SeqOp);
  // The constructor for CombOp SUnit.
  SIRSchedUnit(unsigned Idx, Type T, BasicBlock *BB, Instruction *CombOp);

  unsigned getII() const { return II; }
  void setII(unsigned newII) { this->II = std::max(this->II, II); }
  unsigned getIdx() const { return Idx; }
  Type getType() const { return T; }
  float getSchedule() const { return Schedule; }
  bool scheduleTo(float NewSchedule) {
    assert(NewSchedule >= 0 && "Unexpected NULL schedule!");

    // Set the IsScheduled.
    IsScheduled = true;

    bool Changed = NewSchedule != Schedule;
    Schedule = NewSchedule;

    return Changed;
  }
  void resetSchedule() {
    IsScheduled = false;
    Schedule = 0.0;
  }

  Value *getValue();

  bool isEntry() const { return T == Entry; }
  bool isExit() const { return T == Exit; }
  bool isBBEntry() const { return T == BlockEntry; }
  bool isPHI() const { return T == PHI; }
  bool isSlotTransition() const { return T == SlotTransition; }
  bool isSeqSU() const { return T == SeqSU; }
  bool isCombSU() const { return T == CombSU; }
  bool isPHIPack() const { return T == PHIPack; }
  bool isExitSlotPack() const { return T == ExitSlotPack; }
  bool isMemoryPack() const { return T == MemoryPack; }

  SIRSeqOp *getSeqOp() const {
    assert(SeqOps.size() == 1 && "Use the wrong function!");

    return SeqOps.front();
  }
  ArrayRef<SIRSeqOp *> getSeqOps() const { return SeqOps; }

  Instruction *getCombOp() const {
    return CombOp;
  }

  // If the SUnit is PHI, then the BB we hold in SUnit is its IncomingBB.
  // If the SUnit is terminator, then the BB we hold in SUnit is its TargetBB.
  // Otherwise the BB we hold in SUnit is its ParentBB.
  BasicBlock *getParentBB() const;

  float getLatency() const { return Latency; }

  void addToUseList(SIRSchedUnit *User) {
    UseList.insert(User);
  }
  SmallVector<SIRSchedUnit *, 4> getUseList() const {
    SmallVector<SIRSchedUnit *, 4> Users;

    typedef UseListTy::iterator iterator;
    for (iterator I = UseList.begin(), E = UseList.end(); I != E; I++) {
      Users.push_back(*I);
    }
    return Users;
  }

  // Iterators for dependencies.
  dep_iterator dep_begin() { return Deps.begin(); }
  dep_iterator dep_end() { return Deps.end(); }

  const_dep_iterator dep_begin() const { return Deps.begin(); }
  const_dep_iterator dep_end() const { return Deps.end(); }

  bool dep_empty() const { return Deps.empty(); }
  unsigned dep_size() const { return Deps.size(); }

  /// Iterators for the uses.
  typedef UseListTy::iterator use_iterator;
  use_iterator use_begin() { return UseList.begin(); }
  use_iterator use_end() { return UseList.end(); }

  typedef UseListTy::const_iterator const_use_iterator;
  const_use_iterator use_begin() const { return UseList.begin(); }
  const_use_iterator use_end() const { return UseList.end(); }

  bool use_empty() const { return UseList.empty(); }
  unsigned use_size() const { return UseList.size(); }

  bool isDependsOn(const SIRSchedUnit *A) const {
    return Deps.count(const_cast<SIRSchedUnit *>(A));
  }

  SIRDep getEdgeFrom(const SIRSchedUnit *A, unsigned II = 0) const {
    assert(isDependsOn(A) && "Current atom not depend on A!");
    return getDepIt(A).getEdge(II);
  }

  void removeDep(SIRSchedUnit *Src) {
    bool Erased = Deps.erase(Src);
    assert(Erased && "Src not a dependency of the current node!");
    Src->UseList.erase(this);
    (void) Erased;
  }

  void addDep(SIRSchedUnit *Src, SIRDep NewEdge) {
    // Only PHI node will have self-loop data dependency.
    if (Src == this && NewEdge.getEdgeType() == SIRDep::ValDep)
      assert(Src->isPHI() || Src->isPHIPack() || Src->isExitSlotPack() && "Cannot add self-loop!");

    DepSet::iterator at = Deps.find(Src);

    // If there is no old Dep before, then just add a new one.
    if (at == Deps.end()) {
      Deps.insert(std::make_pair(Src, EdgeBundle(NewEdge)));
      Src->addToUseList(this);
    } else {
      float OldLatency = getEdgeFrom(Src).getLatency();
      at->second.addEdge(NewEdge);
      assert(OldLatency <= getEdgeFrom(Src).getLatency() && "Edge lost!");
      (void) OldLatency;
    }

    assert(getEdgeFrom(Src).getLatency() >= NewEdge.getLatency()
      && "Edge not inserted?");
  }

  void replaceAllDepWith(SIRSchedUnit *NewSUnit);

  // Only the SUnit in Slot0r can have schedule of 0. So all others
  // scheduled SUnit should have a positive schedule number.
  bool isScheduled() const { return IsScheduled; }

  /// Functions for debug
  void print(raw_ostream &OS) const;
  void dump() const;
};

template<> struct GraphTraits<SIRSchedUnit *> {
  typedef SIRSchedUnit NodeType;
  typedef SIRSchedUnit::use_iterator ChildIteratorType;

  static NodeType *getEntryNode(const SIRSchedUnit *N) {
    return const_cast<SIRSchedUnit *>(N);
  }

  static ChildIteratorType child_begin(NodeType *N) {
    return N->use_begin();
  }

  static ChildIteratorType child_end(NodeType *N) {
    return N->use_end();
  }
};

class SIRSchedGraph {
private:
  Function &F;

  unsigned TotalSUs;
  typedef iplist<SIRSchedUnit> SUList;
  SUList SUnits;

  // Mapping between SIRSeqOp and SIRSchedUnit.
  typedef std::map<SIRSeqOp *, SIRSchedUnit *> SeqOp2SUMapTy;
  SeqOp2SUMapTy SeqOp2SUMap;

  // Mapping between SIRSlot and SIRSchedUnits.
  typedef std::map<SIRSlot *, SmallVector<SIRSchedUnit *, 4> >Slot2SUMapTy;
  Slot2SUMapTy Slot2SUMap;

  // Mapping between LLVM IR and SIRSchedUnits.
  typedef std::map<Value *, SmallVector<SIRSchedUnit *, 4> > IR2SUMapTy;
  IR2SUMapTy IR2SUMap;

  // Mapping between Loop BB and Loop SUnit.
  typedef std::map<BasicBlock *, SIRSchedUnit *> LoopBB2LoopSUMapTy;
  LoopBB2LoopSUMapTy LoopBB2LoopSUMap;

  // Mapping between Pipelined BB and MII.
  typedef std::map<BasicBlock *, unsigned> PipelinedBB2MIIMapTy;
  PipelinedBB2MIIMapTy PipelineBB2MIIMap;

  // Mapping between SIRSchedUnit and SIRMemoryBank it accessed.
  typedef std::map<SIRSchedUnit *, SIRMemoryBank *> SU2SMBMapTy;
  SU2SMBMapTy SU2SMBMap;

  // Helper class to arrange the scheduling units according to their parent BB,
  // we will emit the schedule or build the linear order BB by BB.
  std::map<BasicBlock *, SmallVector<SIRSchedUnit *, 4> > BBMap;

public:
  SIRSchedGraph(Function &F);
  ~SIRSchedGraph();

  Function &getFunction() const { return F; }

  SIRSchedUnit *getEntry() { return &SUnits.front(); }
  const SIRSchedUnit *getEntry() const { return &SUnits.front(); }

  SIRSchedUnit *getExit() { return &SUnits.back(); }
  const SIRSchedUnit *getExit() const { return &SUnits.back(); }

  bool isBBReachable(BasicBlock *BB) const {
    return BBMap.count(BB);
  }

  bool hasSU(SIRSeqOp *SeqOp) const { return SeqOp2SUMap.count(SeqOp); }
  SIRSchedUnit *lookupSU(SIRSeqOp *SeqOp) const;
  bool indexSU2SeqOp(SIRSchedUnit *SU, SIRSeqOp *SeqOp);

  bool hasSU(SIRSlot *S) const { return Slot2SUMap.count(S); }
  ArrayRef<SIRSchedUnit *> lookupSUs(SIRSlot *S) const;
  bool indexSU2Slot(SIRSchedUnit *SU, SIRSlot *S);

  bool hasSU(Value *V) const { return IR2SUMap.count(V); }
  ArrayRef<SIRSchedUnit *> lookupSUs(Value *V) const;
  bool indexSU2IR(SIRSchedUnit *SU, Value *V);

  bool hasLoopSU(BasicBlock *BB) const { return LoopBB2LoopSUMap.count(BB); }
  SIRSchedUnit *getLoopSU(BasicBlock *BB);
  bool indexLoopSU2LoopBB(SIRSchedUnit *SU, BasicBlock *BB);

  bool hasMII(BasicBlock *BB) const { return PipelineBB2MIIMap.count(BB); }
  unsigned getMII(BasicBlock *BB);
  bool indexPipelinedBB2MII(BasicBlock *PipelinedBB, unsigned MII);

  bool hasMemoryBank(SIRSchedUnit *SU) const { return SU2SMBMap.count(SU); }
  SIRMemoryBank *getMemoryBank(SIRSchedUnit *SU);
  bool indexMemoryBank2SUnit(SIRMemoryBank *Bank, SIRSchedUnit *SU);

  ArrayRef<SIRSchedUnit *> getSUsInBB(BasicBlock *BB) const;

  SIRSchedUnit *getEntrySU(BasicBlock *BB) const {
    SIRSchedUnit *Entry = getSUsInBB(BB).front();
    assert(Entry->isBBEntry() && "Bad SU order, did you sort the SUs?");
    return Entry;
  }

  SIRSchedUnit *createSUnit(BasicBlock *ParentBB, SIRSchedUnit::Type T);
  SIRSchedUnit *createSUnit(BasicBlock *ParentBB, SIRSchedUnit::Type T, SIRSeqOp *SeqOp);
  SIRSchedUnit *createSUnit(BasicBlock *ParentBB, SIRSchedUnit::Type T,
                            SmallVector<SIRSeqOp *, 4> SeqOps);
  SIRSchedUnit *createSUnit(BasicBlock *ParentBB, SIRSchedUnit::Type T, Instruction *CombOp);

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

  typedef std::map<BasicBlock *, SmallVector<SIRSchedUnit *, 4> >::iterator bb_iterator;
  bb_iterator bb_begin() { return BBMap.begin(); }
  bb_iterator bb_end() { return BBMap.end(); }

  typedef std::map<BasicBlock *, SmallVector<SIRSchedUnit *, 4> >::const_iterator
    const_bb_iterator;
  const_bb_iterator bb_begin() const { return BBMap.begin(); }
  const_bb_iterator bb_end() const { return BBMap.end(); }

  typedef LoopBB2LoopSUMapTy::const_iterator const_loopbb_iterator;
  const_loopbb_iterator loopbb_begin() const { return LoopBB2LoopSUMap.begin(); }
  const_loopbb_iterator loopbb_end() const { return LoopBB2LoopSUMap.end(); }

  unsigned size() const { return TotalSUs; }

  // Sort the scheduling units in topological order.
  void toposortCone(SIRSchedUnit *Root, std::set<SIRSchedUnit *> &Visited,
    BasicBlock *BB);
  void topologicalSortSUs();

  void replaceAllUseWith(SIRSchedUnit *OldSU, SIRSchedUnit *NewSU);

  void deleteUselessSUnit(SIRSchedUnit *U);
  void gc();

  // Reset the schedule of all the scheduling units in the graph.
  void resetSchedule();
};

template<> struct GraphTraits<SIRSchedGraph *> : public GraphTraits<SIRSchedUnit *> {
  typedef SIRSchedUnit NodeType;
  typedef SIRSchedUnit::use_iterator ChildIteratorType;

  static NodeType *getEntryNode(const SIRSchedGraph *G) {
    return const_cast<SIRSchedUnit *>(G->getEntry());
  }

  typedef SIRSchedGraph::iterator nodes_iterator;

  static nodes_iterator nodes_begin(SIRSchedGraph *G) {
    return G->begin();
  }

  static nodes_iterator nodes_end(SIRSchedGraph *G) {
    return G->end();
  }
};
}

#endif