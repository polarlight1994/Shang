//===- VASSTSeqValNumbering.h - Reaching Definition on Verilog AST - C++ ----=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This define the VASSTSeqValNumbering pass, which construct SSA form on
// register transfer level.
//
//===----------------------------------------------------------------------===//


#include "vtm/VASTControlPathNodes.h"
#include "vtm/Utilities.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/Allocator.h"

#include <set>

#ifndef RTL_SSA_ANALYSIS_H
#define RTL_SSA_ANALYSIS_H

namespace llvm {
class VASTSeqValNumbering;
class SlotInfo;

// VASTSeqValue numbering information.
class SVNInfo {
  struct LiveInInfo {
    uint32_t Cycles;

    LiveInInfo(uint32_t Cycles = 0) : Cycles(Cycles) {}

    uint32_t getCycles() const { return Cycles; }

    void incCycles(int Inc = 1) { Cycles += Inc; }
  };

  VASTSeqValue *const V;
  VASTSlot *const Slot;
  const MachineInstr *const DefMI;

  // Vector for the dependent SVNInfos which is a Predecessor VAS.
  typedef DenseMap<SVNInfo*, LiveInInfo> VASCycMapTy;
  VASCycMapTy DepVAS;

  typedef SmallPtrSet<SVNInfo*, 8> VASSetTy;
  // Vector for the successor SVNInfo.
  VASSetTy UseVAS;

  void addDepVAS(SVNInfo *VAS, SVNInfo::LiveInInfo NewLI){
    assert(NewLI.getCycles() && "Expect non-zero distance!");

    LiveInInfo &Info = DepVAS[VAS];

    if (Info.getCycles() == 0) VAS->UseVAS.insert(this);

    if (Info.getCycles() == 0 || Info.getCycles() > NewLI.getCycles())
      // Try to take the shortest path.
      Info = NewLI;
  }

  SVNInfo(VASTSeqValue *V, VASTSlot *slot, const MachineInstr *MI)
    : V(V), Slot(slot), DefMI(MI) {}
  SVNInfo(const SVNInfo&); // Do not implement.

  LiveInInfo getDepInfo(SVNInfo *VAS) const {
    VASCycMapTy::const_iterator at = DepVAS.find(VAS);
    return at == DepVAS.end() ? LiveInInfo() : at->second;
  }

  friend class SlotInfo;
  friend class VASTSeqValNumbering;
public:
  VASTSeqValue *getValue() const { return V; }
  VASTSlot *getSlot() const { return Slot; }
  const MachineInstr *getDefMI() const { return DefMI; }

  std::string getName() const;

  void verify() const;

  void print(raw_ostream &OS, unsigned Ind = 0) const;

  void dump() const;

  typedef VASSetTy::iterator iterator;
  iterator use_begin() { return UseVAS.begin(); }
  iterator use_end() { return UseVAS.end(); }

  bool operator==(SVNInfo &RHS) const {
    return V == RHS.getValue() && Slot == RHS.getSlot();
  }
};

template<> struct GraphTraits<SVNInfo*> {
  typedef SVNInfo NodeType;
  typedef NodeType::iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->use_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->use_end();
  }
};

// SlotInfo, store the data-flow information of a slot.
class SlotInfo {
  // Define the VAS set for the reaching definition dense map.
  typedef std::set<SVNInfo*> VASSetTy;
  typedef std::map<SVNInfo*, SVNInfo::LiveInInfo> VASCycMapTy;
  const VASTSlot *S;
  // Define Set for the reaching definition.
  VASSetTy SlotGen;
  typedef std::set<VASTValue*> ValueSet;
  ValueSet OverWrittenValue;
  // In/Out set with cycles form define information.
  VASCycMapTy SlotIn;
  VASCycMapTy SlotOut;

  typedef VASSetTy::iterator gen_iterator;
  // get the iterator of the defining map of reaching definition.
  gen_iterator gen_begin() const { return SlotGen.begin(); }
  gen_iterator gen_end() const { return SlotGen.end(); }

  typedef
  std::pointer_to_unary_function<std::pair<SVNInfo*, unsigned>,
                                 SVNInfo*>
  vas_getter;

  static bool updateLiveIn(SVNInfo *VAS, SVNInfo::LiveInInfo NewLI,
                           VASCycMapTy &S) {
    assert(NewLI.getCycles() && "It takes at least a cycle to live in!");
    SVNInfo::LiveInInfo &Info = S[VAS];

    if (Info.Cycles == 0 || Info.Cycles > NewLI.Cycles) {
      // Try to take the shortest path.
      Info = NewLI;
      return true;
    }

    return false;
  }

  SVNInfo::LiveInInfo getLiveIn(SVNInfo *VAS) const {
    vascyc_iterator at = SlotIn.find(VAS);
    return at == SlotIn.end() ? SVNInfo::LiveInInfo() : at->second;
  }

  // Initialize the out set by simply copying the gen set, and initialize the
  // cycle counter to 0.
  void initOutSet();

  // Insert VAS into different set.
  void insertGen(SVNInfo *VAS) {
    SlotGen.insert(VAS);
    insertOvewritten(VAS->getValue());
  }

  void insertOvewritten(VASTValue *V) {
    OverWrittenValue.insert(V);
  }

  bool insertIn(SVNInfo *VAS, SVNInfo::LiveInInfo NewLI) {
    return updateLiveIn(VAS, NewLI, SlotIn);
  }

  bool insertOut(SVNInfo *VAS, SVNInfo::LiveInInfo NewLI) {
    return updateLiveIn(VAS, NewLI, SlotOut);
  }

  friend class VASTSeqValNumbering;
public:
  SlotInfo(const VASTSlot *s) : S(s) {}

  typedef VASCycMapTy::const_iterator vascyc_iterator;
  typedef mapped_iterator<VASCycMapTy::iterator, vas_getter> iterator;

  vascyc_iterator in_begin() const { return SlotIn.begin(); }
  vascyc_iterator in_end() const { return SlotIn.end(); }
  vascyc_iterator out_begin() const { return SlotOut.begin(); }
  vascyc_iterator out_end() const { return SlotOut.end(); }

  bool isVASKilled(const SVNInfo *VAS) const;

  // Get the distance (in cycles) from the define slot of the VAS to this slot.
  unsigned getCyclesFromDef(SVNInfo *VAS) const {
    return getLiveIn(VAS).getCycles();
  }

  // Get Slot pointer.
  const VASTSlot *getSlot() { return S; }

  void print(raw_ostream &OS) const;
  void dump() const;
};

// The RtlSSAAnalysis that construct the SSA form.
class VASTSeqValNumbering : public MachineFunctionPass {
public:
  // define VASVec for the SVNInfo.
  typedef SmallVector<SVNInfo*, 4> VASVec;
  typedef VASVec::iterator vasvec_it;

  // Define small vector for the slots.
  typedef SmallVector<VASTSlot*, 4> SlotVecTy;
  typedef SlotVecTy::iterator slot_vec_it;

  typedef std::map<const VASTSlot* ,SlotInfo*> SlotInfoTy;
  typedef SlotInfoTy::const_iterator slotinfo_it;

private:
  SlotInfoTy SlotInfos;
  SlotVecTy SlotVec;

  typedef DenseMap<std::pair<VASTSeqValue*, VASTSlot*>, SVNInfo*> VASMapTy;
  VASMapTy UniqueVASs;
  // Use mapped_iterator which is a simple iterator adapter that causes a
  // function to be dereferenced whenever operator* is invoked on the iterator.
  typedef
  std::pointer_to_unary_function<std::pair<std::pair<VASTValue*, VASTSlot*>,
                                                     SVNInfo*>,
                                 SVNInfo*>
  vas_getter;

  typedef mapped_iterator<VASMapTy::iterator, vas_getter> vas_iterator;
  typedef mapped_iterator<VASMapTy::const_iterator, vas_getter>
          const_vas_iterator;

  BumpPtrAllocator Allocator;

  // define VAS assign iterator.
  typedef VASTSeqValue::const_itertor vn_itertor;

  VASTModule *VM;
public:
  static char ID;

  vas_iterator vas_begin() {
    return vas_iterator(UniqueVASs.begin(),
                        vas_getter(pair_second<std::pair<VASTValue*, VASTSlot*>,
                                               SVNInfo*>));
  }

  vas_iterator vas_end() {
    return vas_iterator(UniqueVASs.end(),
                        vas_getter(pair_second<std::pair<VASTValue*, VASTSlot*>,
                        SVNInfo*>));
  }

  const_vas_iterator vas_begin() const {
    return const_vas_iterator(UniqueVASs.begin(),
                        vas_getter(pair_second<std::pair<VASTValue*, VASTSlot*>,
                                               SVNInfo*>));
  }

  const_vas_iterator vas_end() const {
    return const_vas_iterator(UniqueVASs.end(),
                        vas_getter(pair_second<std::pair<VASTValue*, VASTSlot*>,
                        SVNInfo*>));
  }


  slot_vec_it slot_begin() { return SlotVec.begin(); }
  slot_vec_it slot_end() { return SlotVec.end(); }

  // Get SlotInfo from the existing SlotInfos set.
  SlotInfo* getSlotInfo(const VASTSlot *S) const;

  SVNInfo *getValueASlot(VASTSeqValue *V, VASTSlot *S);

  // Traverse every register to define the SVNInfos.
  void buildAllVAS();

  // Traverse every register to define the SVNInfos.
  void buildVASGraph();

  // Add dependent SVNInfo.
  void addVASDep(SVNInfo *VAS, VASTSeqValue *DepVal);

  // Traverse the dependent VASTValue *to get the registers.
  void visitDepTree(VASTValue *DepTree, SVNInfo *VAS);

  bool addLiveIns(SlotInfo *From, SlotInfo *To, bool FromAliasSlot);
  bool addLiveInFromAliasSlots(VASTSlot *From, SlotInfo *To);

  // Using the reaching definition algorithm to sort out the ultimate
  // relationship of registers.
  // Dirty hack: maybe there are two same statements is a slot, and we can use
  // bit vector to implement the algorithm similar to the compiler principle.
  void ComputeReachingDefinition();

  // collect the Generated and Killed statements of the slot.
  void ComputeGenAndKill();

  void verifyRTLDependences() const;

  void viewGraph();

  void releaseMemory() {
    UniqueVASs.clear();
    Allocator.Reset();
    SlotVec.clear();
    SlotInfos.clear();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnMachineFunction(MachineFunction &MF);

  VASTSeqValNumbering();
};


template <> struct GraphTraits<VASTSeqValNumbering*>
: public GraphTraits<VASTSlot*> {

  typedef VASTSeqValNumbering::slot_vec_it nodes_iterator;
  static nodes_iterator nodes_begin(VASTSeqValNumbering *G) {
    return G->slot_begin();
  }
  static nodes_iterator nodes_end(VASTSeqValNumbering *G) {
    return G->slot_end();
  }
};
}
#endif
