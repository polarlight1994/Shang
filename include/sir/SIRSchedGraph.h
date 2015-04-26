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
    ValDep      = 0,
    MemDep      = 1,
    CtrlDep     = 2,
    // Generic dependency that may form cycles.
    Generic     = 3
  };

private:
  uint8_t EdgeType : 4;
  // Iterate distance.
  int16_t Distance : 14;
  // The latency of this edge.
  uint16_t Latency : 14;

public:
  SIRDep(enum Types T, unsigned Latency, int Distance)
    : EdgeType(T), Latency(Latency), Distance(Distance) {}

	static SIRDep CreateValDep(int Latency) {
		return SIRDep(ValDep, Latency, 0);
	}

	static SIRDep CreateMemDep(int Latency, int Distance) {
		return SIRDep(MemDep, Latency, Distance);
	}

	static SIRDep CreateCtrlDep(int Latency) {
		return SIRDep(CtrlDep, Latency, 0);
	}

  Types getEdgeType() const { return Types(EdgeType); }
  int getDistance() const { return Distance; }
  bool isLoopCarried() const { return getDistance() != 0; }
  inline int getLatency(unsigned II = 0) const {
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
		// Slot Transition
		SlotTransition,
		// Normal node for Scheduling
		Normal,
    // Invalid node for the ilist sentinel
    Invalid
  };

private:
  const Type T : 4;
  // Initial Interval of the functional unit.
  uint32_t II : 8;
  uint32_t Schedule : 20;
  uint16_t InstIdx;
	// The latency of this unit self.
	uint16_t Latency;

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

  typedef SIRSchedUnitDepIterator<DepSet::const_iterator, true> const_dep_iterator;
  typedef SIRSchedUnitDepIterator<DepSet::iterator, false> dep_iterator;

private:
  // Remember the dependencies of the scheduling unit.
  DepSet Deps;
  
	Instruction *Inst;
  SIRSeqOp *SeqOp;
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
  // Create the virtual SUs.
  SIRSchedUnit(unsigned InstIdx, Instruction *Inst, 
		           Type T, BasicBlock *BB, SIRSeqOp *SeqOp);

  void setII(unsigned newII) { this->II = std::max(this->II, II); }
  unsigned getII() const { return II; }

	unsigned getInstIdx() const { return InstIdx; }

  unsigned getSchedule() const { return Schedule; }
	bool scheduleTo(unsigned Step) { 
		bool Changed = Step != Schedule;
		// StartSlot must be a IdleSlot without any SeqOps!
		assert(Step && "Bad schedule!");
		Schedule = Step;
		return Changed; 
	}

	void resetSchedule() { Schedule = 0; }

  bool isEntry() const { return T == Entry; }
  bool isExit() const { return T == Exit; }
  bool isBBEntry() const { return T == BlockEntry; }
  bool isPHI() const { return T == PHI; }

	bool isTerminator() const { return isa<TerminatorInst>(getInst()); }

	Instruction *getInst() const { return Inst; }
	SIRSeqOp *getSeqOp() const { return SeqOp; }

	// If the SUnit is PHI, then the BB we hold in SUnit is its IncomingBB.
	// If the SUnit is terminator, then the BB we hold in SUnit is its TargetBB.
	// Otherwise the BB we hold in SUnit is its ParentBB.
  BasicBlock *getParentBB() const;
	BasicBlock *getIncomingBB() const;
	BasicBlock *getTargetBB() const;

	unsigned getLatency() const { return Latency; }

  void addToUseList(SIRSchedUnit *User) {
    UseList.insert(User);
  }

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

  bool isDependsOn(const SIRSchedUnit *A) const {
    return Deps.count(const_cast<SIRSchedUnit *>(A));
  }

  SIRDep getEdgeFrom(const SIRSchedUnit *A, unsigned II = 0) const {
    assert(isDependsOn(A) && "Current atom not depend on A!");
    return getDepIt(A).getEdge(II);
  }

  unsigned getDFLatency(const SIRSchedUnit *A) const {
    assert(isDependsOn(A) && "Current atom not depend on A!");
    int L = getDepIt(A).getDFLatency();
    assert(L >= 0 && "Not a data-flow dependence!");
    return unsigned(L);
  }

  void removeDep(SIRSchedUnit *Src) {
    bool Erased = Deps.erase(Src);
    assert(Erased && "Src not a dependency of the current node!");
    Src->UseList.erase(this);
    (void) Erased;
  }

  void addDep(SIRSchedUnit *Src, SIRDep NewEdge) {
    assert(Src != this && "Cannot add self-loop!");
    DepSet::iterator at = Deps.find(Src);

    // If there is no old Dep before, then just add a new one.
    if (at == Deps.end()) {
      Deps.insert(std::make_pair(Src, EdgeBundle(NewEdge)));
      Src->addToUseList(this);
    } else {
      int OldLatency = getEdgeFrom(Src).getLatency();
      at->second.addEdge(NewEdge);
      assert(OldLatency <= getEdgeFrom(Src).getLatency() && "Edge lost!");
      (void) OldLatency;
    }

    assert(getEdgeFrom(Src).getLatency() >= NewEdge.getLatency()
           && "Edge not inserted?");
  }

	// Only the Entry SUnit can have schedule of 0.
	// So all others scheduled SUnit should have a
	// positive schedule number.
  bool isScheduled() const { return Schedule != 0; }

  // Return the index of the current scheduling unit.
  uint32_t getIdx() const { return InstIdx; }

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
  
 	// Mapping between LLVM IR and SIR Scheduling Units.
 	typedef std::map<Value *, SmallVector<SIRSchedUnit *, 4> > IR2SUMapTy;
 	IR2SUMapTy IR2SUMap;

  // Helper class to arrange the scheduling units according to their parent BB,
  // we will emit the schedule or build the linear order BB by BB.
  std::map<BasicBlock*, std::vector<SIRSchedUnit *> > BBMap;

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

	bool hasSU(Value *V) const { return IR2SUMap.count(V); }
	ArrayRef<SIRSchedUnit *> lookupSUs(Value *V) const;
	bool indexSU2IR(SIRSchedUnit *SU, Value *V);

  MutableArrayRef<SIRSchedUnit *> getSUsInBB(BasicBlock *BB);
  ArrayRef<SIRSchedUnit *> getSUsInBB(BasicBlock *BB) const;

  SIRSchedUnit *getEntrySU(BasicBlock *BB) const {
    SIRSchedUnit *Entry = getSUsInBB(BB).front();
    assert(Entry->isBBEntry() && "Bad SU order, did you sort the SUs?");
    return Entry;
  }

  SIRSchedUnit *createSUnit(BasicBlock *ParentBB, SIRSchedUnit::Type T);
  SIRSchedUnit *createSUnit(Instruction *Inst, BasicBlock *ParentBB,
		                        SIRSchedUnit::Type T, SIRSeqOp *SeqOp);

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

  typedef std::map<BasicBlock*, std::vector<SIRSchedUnit *> >::iterator bb_iterator;
  bb_iterator bb_begin() { return BBMap.begin(); }
  bb_iterator bb_end() { return BBMap.end(); }

  typedef std::map<BasicBlock*, std::vector<SIRSchedUnit *> >::const_iterator
    const_bb_iterator;
  const_bb_iterator bb_begin() const { return BBMap.begin(); }
  const_bb_iterator bb_end() const { return BBMap.end(); }

  unsigned size() const { return TotalSUs; }

	// Sort the scheduling units in topological order.
	void toposortCone(SIRSchedUnit *Root, std::set<SIRSchedUnit *> &Visited,
		                BasicBlock *BB);
	void topologicalSortSUs();

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