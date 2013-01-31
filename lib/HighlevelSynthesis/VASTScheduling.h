//===------- VASTScheduling.h - Scheduling Graph on VAST  -------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the VASTSUnit class, which represents the elemental
// scheduling unit in VAST.
//
//===----------------------------------------------------------------------===//
//

#ifndef VAST_SCHEDULING_H
#define VAST_SCHEDULING_H

#include "shang/VASTControlPathNodes.h"

#include "llvm/Support/DataTypes.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/PointerUnion.h"

namespace llvm {
class raw_ostream;
class VASTModule;

class VASTDep {
public:
  enum Types {
    ValDep = 0,
    MemDep = 1,
    CtrlDep = 2,
    FixedTiming = 3,
    ChainSupporting = 4,
    LinearOrder = 5,
    Conditional = 6
  };
private:
  uint8_t EdgeType : 3;
  // Iterate distance.
  int16_t Distance : 13;
  // The latancy of this edge.
  int16_t Latancy;
  int32_t Data;

  friend class VSUnit;
protected:
  VASTDep(enum Types T, int latancy, int Dst)
    : EdgeType(T), Distance(Dst), Latancy(latancy) {}
public:
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
    return VDEdge(MemDep, Latency, DISTANCE);
  }

  static VASTDep CreateMemDep(int Latency, int Distance) {
    return VASTDep(MemDep, Latency, Distance);
  }

  static VASTDep CreateValDep(int Latency) {
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
};

class VASTSUnit {
  // TODO: typedef SlotType
  unsigned SchedSlot : 31;
  bool     HasFixedTiming: 1;
  uint16_t InstIdx;
  uint16_t FUNum;

  // EdgeBundle allow us add/remove edges between VASTSUnit more easily.
  struct EdgeBundle {
    SmallVector<VASTDep, 1> Edges;
    bool IsCrossBB;

    explicit EdgeBundle(VASTDep E, bool IsCrossBB)
      : Edges(1, E), IsCrossBB(IsCrossBB) {}

    void addEdge(VASTDep NewEdge);
    VASTDep &getEdge(unsigned II = 0);
    const VASTDep &getEdge(unsigned II = 0) const {
      return const_cast<EdgeBundle*>(this)->getEdge(II);
    }
  };

public:
  template<class IteratorType, bool IsConst>
  class VSUnitDepIterator : public IteratorType {
    typedef VSUnitDepIterator<IteratorType, IsConst> Self;
    typedef typename conditional<IsConst, const VASTSUnit, VASTSUnit>::type
            NodeType;
    typedef typename conditional<IsConst, const VASTDep, VASTDep>::type
            EdgeType;
    typedef typename conditional<IsConst, const EdgeBundle, EdgeBundle>::type
            EdgeBundleType;

    EdgeBundleType &getEdgeBundle() const {
      return IteratorType::operator->()->second;
    }
  public:
    VSUnitDepIterator(IteratorType i) : IteratorType(i) {}

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

private:
  // Remember the dependencies of the scheduling unit.
  typedef DenseMap<VASTSUnit*, EdgeBundle> DepSet;
  DepSet Deps;

  // The scheduling units that using this scheduling unit.
  typedef std::set<VASTSUnit*> UseListTy;
  UseListTy UseList;

  void addToUseList(VASTSUnit *User) { UseList.insert(User); }

  // The scheduling unit may represent a VASTNode of just an alias of another
  // Scheduling unit.
  PointerUnion<VASTNode*, VASTSUnit*> Ptr;
public:
  VASTSUnit(VASTNode *N, unsigned InstIdx);

  BasicBlock *getParentBB() const;
  
  void addDep(VASTSUnit *Src, VASTDep NewE) {
    assert(Src != this && "Cannot add self-loop!");
    DepSet::iterator at = Deps.find(Src);

    if (at == Deps.end()) {
      bool IsCrossBB = Src->getParentBB() != getParentBB();
      Deps.insert(std::make_pair(Src, EdgeBundle(NewE, IsCrossBB)));
      Src->addToUseList(this);
      return;
    }

    at->second.addEdge(NewE);
  }
};

// The container of the VASTSUnits
class VASTSchedGraph {
public:
  typedef std::vector<VASTSUnit*> SUnitVecTy;
  typedef SUnitVecTy::iterator iterator;
  typedef SUnitVecTy::const_iterator const_iterator;
  enum { NullSUIdx = 0u, FirstSUIdx = 1u };

private:
  unsigned InstIdx;
  VASTSUnit *Entry, *Exit;
  std::map<VASTNode*, VASTSUnit*> N2SUMap;
  SUnitVecTy SUnits;

  void buildSchedGraph(VASTModule *VM);
  void buildSchedGraph(VASTSlot *S);
  
  void buildDepEdges(VASTSlot *S);
  void buildDepEdges(VASTSeqOp *SeqOp);
  void buildDatapathEdge(VASTSeqOp *SeqOp);

  VASTSUnit *buildSlotEntry(VASTSlot *S);
  VASTSUnit *buildSeqOpSU(VASTSeqOp *SeqOp);
  VASTSUnit *buildSUnit(VASTNode *N);

  void schedule();

  VASTSUnit *getSUnit(VASTNode *N) const {
    std::map<VASTNode*, VASTSUnit*>::const_iterator at = N2SUMap.find(N);
    assert(at != N2SUMap.end() && "SUnit not found!");
    return at->second;
  }
public:
  VASTSchedGraph();
  ~VASTSchedGraph();

  VASTSUnit *createSUnit(BasicBlock *ParentBB, uint16_t FUNum);

  void schedule(VASTModule *VM);
};

}

#endif