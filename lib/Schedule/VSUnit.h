//===------------- VSUnit.h - Translate LLVM IR to VSUnit  -------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the VSUnit class, which represent the basic atom operation
// in hardware.
//
//===----------------------------------------------------------------------===//
//

#ifndef VBE_HARDWARE_ATOM_H
#define VBE_HARDWARE_ATOM_H

#include "vtm/VInstrInfo.h"
#include "vtm/FUInfo.h"
#include "vtm/Utilities.h"
#include "vtm/BitLevelDelayInfo.h"

#include "llvm/Assembly/Writer.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_os_ostream.h"

#include <list>

namespace llvm {
class BitLevelInfo;
class SDCSchedulingBase; 
template<bool> class SDCScheduler;
class FuncUnitId;
class VSUnit;
class VSchedGraph;

class MachineBasicBlock;
class MachineInstr;
class MachineOperand;

class MachineDominatorTree;

/// @brief Inline operation
class VDEdge {
public:
  enum Types {
    ValDep = 0,
    MemDep = 1,
    CtrlDep = 2,
    FixedTiming = 3,
    ChainSupporting = 4,
    LinearOrder = 5
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
  VDEdge(enum Types T, int latancy, int Dst)
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

  inline bool operator==(const VDEdge &RHS) const {
    return RHS.EdgeType == EdgeType
           && RHS.Latancy == Latancy
           && RHS.Distance == Distance;
  }

  void print(raw_ostream &OS) const;

  template<int DISTANCE>
  static VDEdge CreateMemDep(int Latency) {
    return VDEdge(MemDep, Latency, DISTANCE);
  }

  static VDEdge CreateMemDep(int Latency, int Distance) {
    return VDEdge(MemDep, Latency, Distance);
  }

  static VDEdge CreateValDep(int Latency) {
    return VDEdge(ValDep, Latency, 0);
  }

  static VDEdge CreateCtrlDep(int Latency) {
    return VDEdge(CtrlDep, Latency, 0);
  }

  static VDEdge CreateFixTimingConstraint(int Latency) {
    return VDEdge(FixedTiming, Latency, 0);
  }

  template<Types Type>
  static VDEdge CreateDep(int Latency) {
    return VDEdge(Type, Latency, 0);
  }
};

template<VDEdge::Types Type>
struct IsVDEdgeTypeValDepEval {

};

template<>
struct IsVDEdgeTypeValDepEval<VDEdge::ValDep> {
  enum  { Val = true };
};

template<>
struct IsVDEdgeTypeValDepEval<VDEdge::CtrlDep> {
  enum  { Val = false };
};

template<>
struct IsVDEdgeTypeValDepEval<VDEdge::MemDep> {
  enum  { Val = false };
};

/// @brief Base Class of all hardware atom.
class VSUnit {
  // TODO: typedef SlotType
  unsigned SchedSlot : 30;
  bool     IsDangling : 1;
  bool     HasFixedTiming: 1;
  uint16_t InstIdx;
  uint16_t FUNum;

  struct EdgeBundle {
    SmallVector<VDEdge, 1> Edges;
    bool IsCrossBB;

    explicit EdgeBundle(VDEdge E, bool IsCrossBB)
      : Edges(1, E), IsCrossBB(IsCrossBB) {}

    void addEdge(VDEdge NewEdge);
    VDEdge &getEdge(unsigned II = 0);
    const VDEdge &getEdge(unsigned II = 0) const {
      return const_cast<EdgeBundle*>(this)->getEdge(II);
    }
  };

public:
  template<class IteratorType, bool IsConst>
  class VSUnitDepIterator : public IteratorType {
    typedef VSUnitDepIterator<IteratorType, IsConst> Self;
    typedef typename conditional<IsConst, const VSUnit, VSUnit>::type NodeType;
    typedef typename conditional<IsConst, const VDEdge, VDEdge>::type EdgeType;
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
    VDEdge::Types getEdgeType(unsigned II = 0) const {
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
  typedef DenseMap<VSUnit*, EdgeBundle> DepSet;
  DepSet CPDeps, DPDeps;

  // The atoms that using this atom.
  typedef std::set<VSUnit*> UseListTy;
  UseListTy CPUseList, DPUseList;

  template<bool IsCtrlPath>
  void addToUseList(VSUnit *User) {
    if (IsCtrlPath) CPUseList.insert(User);
    else            DPUseList.insert(User);
  }

  VSUnit(const VSUnit&);          // DO NOT IMPLEMENT
  void operator=(const VSUnit&);  // DO NOT IMPLEMENT

  /// The corresponding Instructions - We may store several instruction inside
  /// the same schedule unit, so we can clamp them in a same slot.
  SmallVector<InstPtrTy, 8> Instrs;
  // Latency from representative instruction, the latency of the SUnit is store
  // in latencies[0].
  SmallVector<int8_t, 8> latencies;

  friend class VSchedGraph;

  // Create the entry node.
  VSUnit(MachineBasicBlock *MBB, uint16_t Idx);

  VSUnit(unsigned short Idx, uint16_t FUNum);

  void addPtr(InstPtrTy Ptr, int8_t Latency) {
    assert((Instrs.empty() || getParentBB() == Ptr.getParent())
           && "Mixing instructions from different BB!");
    Instrs.push_back(Ptr);
    latencies.push_back(Latency);
  }

  // Update internal status.
  VSUnit *updateIdx(unsigned short Idx);
  void setIsDangling(bool isDangling = true) { IsDangling = isDangling; }

  void cleanCPDepAndUse() {
    CPDeps.clear();
    DPDeps.clear();
    CPUseList.clear();
    DPUseList.clear();
  }
public:
  static const unsigned short MaxSlot = ~0 >> 1;

  unsigned short getIdx() const { return InstIdx; }
  bool isDangling() const { return IsDangling; }

  /// @name Operands
  //{
  // Add a new depencence edge to the atom.
  template<bool IsCtrlPath>
  void addDep(VSUnit *Src, VDEdge NewE) {
    assert(Src != this && "Cannot add self-loop!");
    DepSet &Set = IsCtrlPath ? CPDeps : DPDeps;
    DepSet::iterator at = Set.find(Src);

    if (at == Set.end()) {
      bool IsCrossBB = Src->getParentBB() != getParentBB();
      Set.insert(std::make_pair(Src, EdgeBundle(NewE, IsCrossBB)));
      Src->addToUseList<IsCtrlPath>(this);
      return;
    }

    at->second.addEdge(NewE);
  }

  // Iterators for data-path dependencies and control-path dependencies.
  typedef VSUnitDepIterator<DepSet::iterator, false> dep_iterator;
  template<bool IsCtrlPath>
  dep_iterator dep_begin() {
    return IsCtrlPath ? CPDeps.begin() : DPDeps.begin();
  }
  template<bool IsCtrlPath>
  dep_iterator dep_end() {
    return IsCtrlPath ? CPDeps.end() : DPDeps.end();
  }

  typedef VSUnitDepIterator<DepSet::const_iterator, true> const_dep_iterator;
  template<bool IsCtrlPath>
  const_dep_iterator dep_begin() const {
    return IsCtrlPath ? CPDeps.begin() : DPDeps.begin();
  }
  template<bool IsCtrlPath>
  const_dep_iterator dep_end() const {
    return IsCtrlPath ? CPDeps.end() : DPDeps.end();
  }

  template<bool IsCtrlPath>
  size_t num_deps() const { return IsCtrlPath ? CPDeps.size() : DPDeps.size(); }

  template<bool IsCtrlPath>
  bool dep_empty() const { return IsCtrlPath ? CPDeps.empty() : DPDeps.empty(); }

  // If the current atom depend on A?
  template<bool IsCtrlPath>
  bool isDepOn(const VSUnit *A) const {
    return getDepIt<IsCtrlPath>(A) != dep_end<IsCtrlPath>();
  }

  // If this Depend on A? return the position if found, return dep_end otherwise.
  template<bool IsCtrlPath>
  dep_iterator getDepIt(const VSUnit *A) {
    return IsCtrlPath ? CPDeps.find(const_cast<VSUnit*>(A))
                      : DPDeps.find(const_cast<VSUnit*>(A));
  }

  template<bool IsCtrlPath>
  VDEdge &getEdgeFrom(const VSUnit *A, unsigned II = 0) {
    assert(isDepOn<IsCtrlPath>(A) && "Current atom not depend on A!");
    return getDepIt<IsCtrlPath>(A).getEdge();
  }

  template<bool IsCtrlPath>
  const_dep_iterator getDepIt(const VSUnit *A) const {
    return IsCtrlPath ? CPDeps.find(const_cast<VSUnit*>(A))
                      : DPDeps.find(const_cast<VSUnit*>(A));
  }

  template<bool IsCtrlPath>
  const VDEdge &getEdgeFrom(const VSUnit *A, unsigned II = 0) const {
    assert(isDepOn<IsCtrlPath>(A) && "Current atom not depend on A!");
    return getDepIt<IsCtrlPath>(A).getEdge(II);
  }

  //}

  /// @name Use
  //{
  typedef UseListTy::iterator use_iterator;
  typedef UseListTy::const_iterator const_use_iterator;
  template<bool IsCtrlPath>
  use_iterator use_begin() {
    return IsCtrlPath ? CPUseList.begin() : DPUseList.begin();
  }
  template<bool IsCtrlPath>
  const_use_iterator use_begin() const {
    return IsCtrlPath ? CPUseList.begin() : DPUseList.begin();
  }
  template<bool IsCtrlPath>
  use_iterator use_end() {
    return IsCtrlPath ? CPUseList.end() : DPUseList.end();
  }
  template<bool IsCtrlPath>
  const_use_iterator use_end() const {
    return IsCtrlPath ? CPUseList.end() : DPUseList.end();
  }

  template<bool IsCtrlPath>
  bool use_empty() const {
    return IsCtrlPath ? CPUseList.empty() : DPUseList.empty();
  }
  template<bool IsCtrlPath>
  size_t num_uses() const {
    return IsCtrlPath ? CPUseList.size() : DPUseList.size();
  }
  //}

  unsigned countValDeps() const;
  unsigned countValUses() const;

  // Dirty Hack: Only return the first instruction.
  InstPtrTy getRepresentativePtr() const {
    return Instrs.front();
  }

  InstPtrTy getPtrAt(unsigned Idx) const { return Instrs[Idx]; }

  bool isRepresentativeInst(MachineInstr *MI) const {
    return MI == getRepresentativePtr();
  }

  bool isBBEntry() const { return getRepresentativePtr().isMBB(); }
  bool isTerminator() const {
    return !isBBEntry() && getRepresentativePtr()->isTerminator();
  }

  MachineBasicBlock *getParentBB() const {
    return getRepresentativePtr().getParent();
  }

  // Get the latency from RepresentativeInst to MI.
  int8_t getLatencyFor(MachineInstr *MI) const;
  int8_t getLatencyAt(unsigned Idx) const {
    if (Idx == 0) return 0;
    return latencies[Idx];
  }

  // Get the total latency from the RepresentativeInst through SrcMI to DstMI.
  template<VDEdge::Types Type>
  inline int getLatencyTo(MachineInstr *SrcMI, MachineInstr *DstMI,
                          VSchedGraph &G) const;

  int getValLatencyTo(MachineInstr *SrcMI, MachineInstr *DstMI,
                      VSchedGraph &G) const {
    return getLatencyTo<VDEdge::ValDep>(SrcMI, DstMI, G);
  }
  int getLatencyFrom(MachineInstr *SrcMI, int SrcLatency) const;

  // Get the maximum latency from RepresentativeInst to DstMI.
  template<VDEdge::Types Type>
  inline int getMaxLatencyTo(MachineInstr *DstMI, VSchedGraph &G) const;

  typedef SmallVectorImpl<InstPtrTy>::iterator instr_iterator;

  size_t num_instrs() const { return Instrs.size(); }
  instr_iterator instr_begin() { return Instrs.begin(); }
  instr_iterator instr_end()   { return Instrs.end(); }

  typedef SmallVectorImpl<InstPtrTy>::const_iterator const_instr_iterator;
  const_instr_iterator instr_begin() const { return Instrs.begin(); }
  const_instr_iterator instr_end()   const { return Instrs.end(); }

  MachineInstr *instr_back() const { return Instrs.back(); }

  bool isPHI() const {
    if (MachineInstr *MI = getRepresentativePtr())
      return MI->isPHI();

    return false;
  }

  unsigned getLatency() const {
    return latencies.front();
  }

  void setLatency(unsigned L) {
    latencies[0] = L;
    assert(getLatency() == L && "Latency overflow!");
  }

  unsigned getSlot() const { return SchedSlot; }
  unsigned getFinSlot() const { return getSlot() + getLatency(); }

  bool isScheduled() const { return SchedSlot != 0; }
  void scheduledTo(unsigned slot);
  void resetSchedule() { SchedSlot = 0; }
  
  bool hasFixedTiming() const { return HasFixedTiming; }
  void setHasFixedTiming(bool has = true) { HasFixedTiming = has; }

  unsigned getOpcode() const;
  VFUs::FUTypes getFUType() const;
  bool isDatapath() const;
  bool isControl() const { return !isDatapath(); }

  unsigned getFUNum() const { return FUNum; }

  FuncUnitId getFUId() const {
    return FuncUnitId(getFUType(), getFUNum());
  }

  /// print - Print out the internal representation of this atom to the
  /// specified stream.  This should really only be used for debugging
  /// purposes.
  void print(raw_ostream &OS) const;

  /// dump - This method is used for debugging.
  ///
  void dump() const;

  // Index functor for VSUnit.
  template<typename T>
  struct IdxFunctor : public std::unary_function<const T*, unsigned> {
    unsigned operator()(const T *U) const {
      return U->getIdx();
    }
  };
};

static inline VSUnit::dep_iterator cp_begin(VSUnit *U) {
  return U->dep_begin<true>();
}

static inline VSUnit::dep_iterator cp_end(VSUnit *U) {
  return U->dep_end<true>();
}

static inline VSUnit::const_dep_iterator cp_begin(const VSUnit *U) {
  return U->dep_begin<true>();
}

static inline VSUnit::const_dep_iterator cp_end(const VSUnit *U) {
  return U->dep_end<true>();
}

static inline size_t cp_empty(const VSUnit *G) {
  return G->dep_empty<true>();
}

static inline VSUnit::dep_iterator dp_begin(VSUnit *U) {
  return U->dep_begin<false>();
}

static inline VSUnit::dep_iterator dp_end(VSUnit *U) {
  return U->dep_end<false>();
}

static inline VSUnit::const_dep_iterator dp_begin(const VSUnit *U) {
  return U->dep_begin<false>();
}

static inline VSUnit::const_dep_iterator dp_end(const VSUnit *U) {
  return U->dep_end<false>();
}

static inline size_t dp_empty(const VSUnit *G) {
  return G->dep_empty<false>();
}

static inline VSUnit::use_iterator cuse_begin(VSUnit *U) {
  return U->use_begin<true>();
}

static inline VSUnit::use_iterator cuse_end(VSUnit *U) {
  return U->use_end<true>();
}

static inline VSUnit::const_use_iterator cuse_begin(const VSUnit *U) {
  return U->use_begin<true>();
}

static inline VSUnit::const_use_iterator cuse_end(const VSUnit *U) {
  return U->use_end<true>();
}

static inline size_t cuse_empty(const VSUnit *G) {
  return G->use_empty<true>();
}

static inline VSUnit::use_iterator duse_begin(VSUnit *U) {
  return U->use_begin<false>();
}

static inline VSUnit::use_iterator duse_end(VSUnit *U) {
  return U->use_end<false>();
}

static inline VSUnit::const_use_iterator duse_begin(const VSUnit *U) {
  return U->use_begin<false>();
}

static inline VSUnit::const_use_iterator duse_end(const VSUnit *U) {
  return U->use_end<false>();
}

static inline size_t duse_empty(const VSUnit *G) {
  return G->use_empty<false>();
}

template<bool IsCtrlPath> struct VSUnitDepGraphTraits {
  typedef VSUnit NodeType;
  typedef VSUnit::dep_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->dep_begin<IsCtrlPath>();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->dep_end<IsCtrlPath>();
  }
};
template<bool IsCtrlPath> struct ConstVSUnitDepGraphTraits {
  typedef const VSUnit NodeType;
  typedef VSUnit::const_dep_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->dep_begin<IsCtrlPath>();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->dep_end<IsCtrlPath>();
  }
};

template<bool IsCtrlPath> struct VSUnitUseGraphTraits {
  typedef VSUnit NodeType;
  typedef VSUnit::use_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->use_begin<IsCtrlPath>();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->use_end<IsCtrlPath>();
  }
};
template<bool IsCtrlPath> struct ConstVSUnitUseGraphTraits {
  typedef const VSUnit NodeType;
  typedef VSUnit::const_use_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->use_begin<IsCtrlPath>();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->use_end<IsCtrlPath>();
  }
};

class VSchedGraph {
public:
  typedef std::vector<VSUnit*> SUnitVecTy;
  typedef SUnitVecTy::iterator iterator;
  typedef SUnitVecTy::const_iterator const_iterator;
  enum { NullSUIdx = 0u, FirstSUIdx = 1u };
  BitLevelDelayInfo &DLInfo;

  // The flag to indicate whether dangling nodes (or cross BB chains) are
  // allowed.
  const bool AllowDangling;
private:
  // Scheduling units in data-path and control-path.
  SUnitVecTy DPSUs, CPSUs;
  VSUnit *Exit;
  // The number of schedule unit.
  unsigned NextSUIdx;

  // The schedule unit that jump back to current fsm state.
  PointerIntPair<MachineInstr*, 1, bool> LoopOp;

  typedef std::map<InstPtrTy, VSUnit*> SUnitMapType;
  SUnitMapType InstToSUnits;
  struct BBInfo {
    // The entry and exit node of the current BB.
    VSUnit *Entry, *Exit;

    // The initial interval of the pipelined BB.
    unsigned II;

    // The BBInfo of current BB's immediate dominator.
    // NOTE: Don't use pointer to the BBInfo, because BBInfo is store in a
    // std::vector, which may reallocate on the fly.
    unsigned IDomIdx;

    // The shortest path distance from the BB's IDom.
    // NOTE: The distance is from the entry of the source BB to the entry of
    // sink BB.
    unsigned SPDFromIDom;

    unsigned getTotalSlot() const {
      return Exit->getSlot() - Entry->getSlot();
    }

    MachineBasicBlock *operator ->() const {
      return Entry->getParentBB();
    }

    inline bool operator ==(const BBInfo &RHS) const {
      // Two BBInfo is equal if and only if their entry is the same.
      // TODO: Assert the reset fields are the same.
      return Entry == RHS.Entry;
    }

    inline bool operator !=(const BBInfo &RHS) const {
      return !operator==(RHS);
    }
  };

  typedef std::vector<BBInfo> BBInfoMapTy;
  BBInfoMapTy BBInfoMap;

  inline const BBInfo &getIDomInfo(const MachineBasicBlock *MBB) const {
    const BBInfo &Info = getBBInfo(MBB);
    assert(Info.IDomIdx < BBInfoMap.size() && "Bad IDomIdx!");
    return BBInfoMap[Info.IDomIdx];
  }

  unsigned getSPD(const BBInfo &Src, const BBInfo &Snk,
                  unsigned ExtraDistance = 0) const;

  inline const BBInfo &getBBInfo(const MachineBasicBlock *MBB) const {
    if (enablePipeLine()) {
      assert(MBB == BBInfoMap.back().Exit->getParentBB()
             && "Bad MBB in local scheduling graph!");
      return BBInfoMap.back();
    }

    return BBInfoMap[MBB->getNumber()];
  }

  inline BBInfo &getBBInfo(const MachineBasicBlock *MBB) {
    if (enablePipeLine()) {
      assert(MBB == BBInfoMap.back().Exit->getParentBB()
        && "Bad MBB in local scheduling graph!");
      return BBInfoMap.back();
    }

    return BBInfoMap[MBB->getNumber()];
  }

  // If the MI is a branch instruction that branching back to the entry of its
  // parent BB, remember it.
  bool rememberLoopOp(MachineInstr *MI);

  // Emit the schedule of the instructions in MBB.
  unsigned emitSchedule(iterator su_begin, iterator su_end, unsigned StartSlot,
                        MachineBasicBlock *MBB);

  // Clear the dangling flag of the VSUnits which is reachable from Root via
  // dependencies chain.
  static void clearDanglingFlagForTree(VSUnit *Root);

  void addSoftConstraintsToBreakChains(SDCSchedulingBase &S);

  void insertDelayBlocks();

  unsigned calculateExpectedSPDFromIDom(const VSUnit *BBEntry);

  void insertDelayBlock(MachineBasicBlock *From, MachineBasicBlock *To,
                        unsigned Latency);
  // Insert the necessary delay operation from current BB's predecessors to
  // current BB to make sure the shortest path distance is satisfied.
  // Return the actually SPD from current BB's IDom after the delay operations
  // are inserted.
  unsigned insertDelayBlock(const VSUnit *BBEntry, unsigned ExpectedSPD);

  // Insert the copy operations which copy the result of the operations to
  // registers, so that we can break the chain.
  void insertFUCtrlAndCopy();
  // This function should visit the instructions in topological order, i.e. all
  // the dependencies of MI should had been visited before MI.
  // If we need to copy the result of MI to register return true, otherwise,
  // return false.
  bool fixRegClassForDatapath(MachineInstr *MI);

  template<bool IsValDep>
  inline unsigned getCtrlStepBetweenImpl(const MachineInstr *SrcInstr,
                                         const MachineInstr *DstInstr) const {
    if (!SrcInstr) return getStepsFromEntry(DstInstr);

    if (IsValDep) return DLInfo.getChainedCPs(SrcInstr, DstInstr->getOpcode());

    return getStepsToFinish(SrcInstr);
  }
public:
  const unsigned EntrySlot;

  VSchedGraph(BitLevelDelayInfo &DLInfo, bool AllowDangling,
              bool EnablePipeline, unsigned EntrySlot)
    : DLInfo(DLInfo), AllowDangling(AllowDangling), Exit(0),
      NextSUIdx(FirstSUIdx), LoopOp(0, EnablePipeline), EntrySlot(EntrySlot) {}

  ~VSchedGraph() {
    std::for_each(DPSUs.begin(), DPSUs.end(), deleter<VSUnit>);
    std::for_each(CPSUs.begin(), CPSUs.end(), deleter<VSUnit>);
  }

  // Forwarding function from BitLevelDelayInfo.
  unsigned getStepsToFinish(const MachineInstr *MI) const {
    return DLInfo.getStepsToFinish(MI);
  }

  void addDummyLatencyEntry(const MachineInstr *MI) {
    DLInfo.addDummyLatencyEntry(MI);
  }

  typedef BitLevelDelayInfo::DepLatInfoTy DepLatInfoTy;
  const DepLatInfoTy *getDepLatInfo(const MachineInstr *DstMI) const {
    return DLInfo.getDepLatInfo(DstMI);
  }

  void buildExitMIInfo(const MachineInstr *SSnk, DepLatInfoTy &Info,
                       const std::set<const MachineInstr*> &ExitMIs) {
    DLInfo.buildExitMIInfo(SSnk, Info, ExitMIs);
  }

  template<VDEdge::Types Type>
  inline unsigned getCtrlStepBetween(const MachineInstr *SrcInstr,
                                     const MachineInstr *DstInstr) const {
    return getCtrlStepBetweenImpl<IsVDEdgeTypeValDepEval<Type>::Val  >(SrcInstr,
                                                                       DstInstr);
  }

  unsigned getStepsFromEntry(const MachineInstr *DstInstr) const;

  // Verify the schedule graph, should be call after the graph is built.
  void verify() const;
  void verifySUDataPath(const VSUnit *SU) const;
  void verifySUControlPath(const VSUnit *SU) const;

  // Add the the iterator point to the first newly added SU.
  iterator mergeSUsInSubGraph(VSchedGraph &SubGraph);

  // VSUnit Creating/Mapping/Merging
  bool mapMI2SU(InstPtrTy Ptr, VSUnit *SU, int8_t latency, bool Mixed = false) {
    if (!Mixed && SU->num_instrs() && Ptr.isMI()
        && SU->isDatapath() != VInstrInfo::isDatapath(Ptr->getOpcode()))
      return false;

    SU->addPtr(Ptr, latency);
    SUnitMapType::iterator where;
    bool inserted;
    tie(where, inserted) = InstToSUnits.insert(std::make_pair(Ptr, SU));
    assert(inserted && "Mapping from I already exist!");
    return true;
  }

  // Extend the to schedule SU list to all SU in current schedule graph.
  void prepareForDatapathSched();

  void topologicalSortCPSUs();

  VSUnit *createTerminator(const MachineBasicBlock *MBB,
                           const MachineDominatorTree *MDT = 0);

  VSUnit *lookUpTerminator(const MachineBasicBlock *MBB) const {
    unsigned Index = MBB->getNumber();
    if (Index >= BBInfoMap.size()) {
      // The scheduling graph may be a local graph which only contains 1 MBB.
      if (enablePipeLine()) {
        VSUnit *Terminator = BBInfoMap.back().Exit;
        assert(MBB == Terminator->getParentBB()
               && "Bad MBB in local scheduling graph!");
        return Terminator;
      }

      return 0;
    }

    return BBInfoMap[Index].Exit;
  }

  unsigned num_bbs() const { return BBInfoMap.size(); }

  VSUnit *createVSUnit(InstPtrTy Ptr, unsigned fuid = 0);
  VSUnit *createExitRoot(const MachineBasicBlock *MBB) {
    assert (Exit == 0 && "Exit already created!");
    Exit = new VSUnit(NextSUIdx++, 0);
    Exit->addPtr(MBB, 0);

    CPSUs.push_back(Exit);

    typedef BBInfoMapTy::iterator it;
    for (it I = BBInfoMap.begin(), E = BBInfoMap.end(); I != E; ++I)
      if (cuse_empty(I->Exit))
        Exit->addDep<true>(I->Exit, VDEdge::CreateCtrlDep(0));

    return Exit;
  }

  bool isLoopOp(MachineInstr *MI) {
    assert(MI->getDesc().isTerminator() && "Expected terminator!");

    // Set the current instruction as loop operation if it is jumping back
    // to the current state and we want to pipeline the state.
    if (rememberLoopOp(MI) && enablePipeLine())
      return true;

    return false;
  }

  bool isLoopPHIMove(MachineInstr *MI);

  /// Mapping machine instruction to schedule unit, this will help us build the
  /// the dependences between schedule unit based on dependences between machine
  /// instructions.
  VSUnit *lookupSUnit(InstPtrTy Ptr) const {
    SUnitMapType::const_iterator at = InstToSUnits.find(Ptr);
    return at == InstToSUnits.end() ? 0 : at->second;
  }

  /// @name Roots
  //{
  VSUnit *getEntryRoot() const { return CPSUs.front(); }
  MachineBasicBlock *getEntryBB() const {
    return getEntryRoot()->getParentBB();
  }
  VSUnit *getExitRoot() const { return Exit; }
  MachineBasicBlock *getExitBB() const { return getExitRoot()->getParentBB(); }
  unsigned getMaxSlot() const { return getExitRoot()->getSlot() + 1; }
  //}

  /// iterator/begin/end - Iterate over all schedule unit in the graph.
  // For Control-path operations and Data-path operations
  size_t num_sus() const { return DPSUs.size() + CPSUs.size(); }

  // Generalized iterators.
  template<bool IsCtrlPath>
  inline iterator begin() {
    return IsCtrlPath ? CPSUs.begin() : DPSUs.begin();
  }

  template<bool IsCtrlPath>
  inline const_iterator begin() const {
    return IsCtrlPath ? CPSUs.begin() : DPSUs.begin();
  }

  template<bool IsCtrlPath>
  inline iterator end() {
    return IsCtrlPath ? CPSUs.end() : DPSUs.end();
  }

  template<bool IsCtrlPath>
  inline const_iterator end() const {
    return IsCtrlPath ? CPSUs.end() : DPSUs.end();
  }

  template<bool IsCtrlPath>
  inline size_t size() const { return IsCtrlPath ? CPSUs.size() : DPSUs.size(); }

  unsigned getNextSUIdx() const { return NextSUIdx; }
  void resetCPSchedule();
  void resetDPSchedule();

  template<bool IsCtrlPath>
  void resetSchedule() {
    if (IsCtrlPath) resetCPSchedule();
    else            resetDPSchedule();
  }

  inline unsigned getStartSlot(const MachineBasicBlock *MBB) const {
    return getBBInfo(MBB).Entry->getSlot();
  }

  inline unsigned getEndSlot(const MachineBasicBlock *MBB) const {
    return getBBInfo(MBB).Exit->getSlot();
  }

  inline unsigned getTotalSlot(const MachineBasicBlock *MBB) const {
    return getBBInfo(MBB).getTotalSlot();
  }

  // II for Modulo schedule
  inline bool isPipelined(const MachineBasicBlock *MBB) const {
    return getII(MBB) < getTotalSlot(MBB);
  }

  inline unsigned getLoopOpSlot(const MachineBasicBlock *MBB) const {
    return getStartSlot(MBB) + getII(MBB);
  }

  inline unsigned getII(const MachineBasicBlock *MBB) const {
    const BBInfo &Info = getBBInfo(MBB);
    return Info.II ? Info.II : Info.getTotalSlot();
  }

  bool enablePipeLine() const {
    return LoopOp.getInt();
  }

  bool hasLoopOp() const { return LoopOp.getPointer() != 0; }
  VSUnit *getLoopOp() const {
    if (MachineInstr *MI = LoopOp.getPointer())
      return lookupSUnit(MI);

    return 0;
  }

  void print(raw_ostream &OS) const;
  void dump() const;
  void viewCPGraph();
  void viewDPGraph();

  /// @name Scheduling
  //{
  bool scheduleLoop();
  // Schedule datapath operations as late as possible after control operations
  // scheduled, this can reduce register usage.
  void scheduleControlPath();
  void scheduleDatapath();
  unsigned emitSchedule();
  //}
};

static inline VSchedGraph::iterator cp_begin(VSchedGraph *G) {
  return G->begin<true>();
}

static inline VSchedGraph::iterator cp_end(VSchedGraph *G) {
  return G->end<true>();
}

static inline VSchedGraph::const_iterator cp_begin(const VSchedGraph *G) {
  return G->begin<true>();
}

static inline VSchedGraph::const_iterator cp_end(const VSchedGraph *G) {
  return G->end<true>();
}

static inline size_t num_cps(const VSchedGraph *G) {
  return G->size<true>();
}

static inline VSchedGraph::iterator dp_begin(VSchedGraph *G) {
  return G->begin<false>();
}

static inline VSchedGraph::iterator dp_end(VSchedGraph *G) {
  return G->end<false>();
}

static inline VSchedGraph::const_iterator dp_begin(const VSchedGraph *G) {
  return G->begin<false>();
}

static inline VSchedGraph::const_iterator dp_end(const VSchedGraph *G) {
  return G->end<false>();
}

template<VDEdge::Types Type>
int VSUnit::getLatencyTo(MachineInstr *SrcMI, MachineInstr *DstMI,
                         VSchedGraph &G) const {
  int Latency = G.getCtrlStepBetween<Type>(SrcMI, DstMI);
  if (SrcMI != getRepresentativePtr()) {
    Latency += getLatencyFor(SrcMI);
  }

  return Latency;
}

template<VDEdge::Types Type>
int VSUnit::getMaxLatencyTo(MachineInstr *DstMI, VSchedGraph &G) const {
  int latency = 0;
  for (const_instr_iterator I = instr_begin(), E = instr_end(); I != E; ++I)
    // Also compute the latency to DstMI even *I (SrcMI) is 0, which means the
    // source is the entry root of the state.
    latency = std::max(getLatencyTo<Type>(*I, DstMI, G), latency);

  return latency;
}

// The wrapper for control-path dependencies graph and data-path dependencies
// graph.
template<bool IsCtrlPath>
struct VSchedGraphWrapper {
  std::vector<const VSUnit*> SUs;

  /*implicit*/ inline VSchedGraphWrapper(const VSchedGraph *G);

  template<typename Iterator>
  VSchedGraphWrapper(Iterator I, Iterator E) : SUs(I, E) {}

  typedef std::vector<const VSUnit*>::const_iterator const_iterator;
  const_iterator begin() const { return SUs.begin(); }
  const_iterator end() const { return SUs.end(); }

  unsigned size() const { return SUs.size(); }
};

template<>
inline VSchedGraphWrapper<true>::VSchedGraphWrapper(const VSchedGraph *G)
  : SUs(cp_begin(G), cp_end(G)) {}

template<>
inline VSchedGraphWrapper<false>::VSchedGraphWrapper(const VSchedGraph *G)
  : SUs(dp_begin(G), dp_end(G))
{
  // The control-path scheduling units are also in the data-path
  // dependencies graph.
  SUs.insert(SUs.end(), cp_begin(G), cp_end(G));
}

template <bool IsCtrlPath>
struct GraphTraits<VSchedGraphWrapper<IsCtrlPath> >{
  typedef VSchedGraphWrapper<IsCtrlPath> GraphType;
  typedef const VSUnit NodeType;
  typedef VSUnit::const_use_iterator ChildIteratorType;

  //static NodeType *getEntryNode(const GraphType &G) {
  //  return G->getEntryRoot();
  //}

  static ChildIteratorType child_begin(NodeType *N) {
    return N->use_begin<IsCtrlPath>();
  }

  static ChildIteratorType child_end(NodeType *N) {
    return N->use_end<IsCtrlPath>();
  }

  typedef typename GraphType::const_iterator nodes_iterator;
  static nodes_iterator nodes_begin(const GraphType &G) {
    return G.begin();
  }

  static nodes_iterator nodes_end(const GraphType &G) {
    return G.end();
  }

  static unsigned size(const GraphType &G) {
    return G.size();
  }
};
} // end namespace

#endif
