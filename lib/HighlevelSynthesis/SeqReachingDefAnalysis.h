//===--- SeqReachingDefAnalysis.h - Reaching Definition on VAST - C++ ----=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This define the SeqReachingDefAnalysis pass, which compute the reaching
// definition analysis on the Verilog AST.
//
//===----------------------------------------------------------------------===//

#include "shang/VASTModulePass.h"
#include "shang/VASTSeqValue.h"
#include "shang/Utilities.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Allocator.h"

#include <set>

#ifndef RTL_SSA_ANALYSIS_H
#define RTL_SSA_ANALYSIS_H

namespace llvm {
class SeqReachingDefAnalysis;
class SlotInfo;

// SlotInfo, store the data-flow information of a slot.
class SlotInfo {
public:
  struct VNInfo {
    VASTSeqValue *Dst;
    VASTSeqOp *Op;

    /*implicit*/ VNInfo(VASTSeqDef D) : Dst(D), Op(D.Op) {}

    /*implicit*/ VNInfo(VASTSeqUse U) : Dst(U.getDst()), Op(U.Op) {}

    bool operator<(const VNInfo &RHS) const {
      if (Dst < RHS.Dst) return true;
      else if (Dst > RHS.Dst) return false;

      if (Op->getPred() < RHS.Op->getPred()) return true;
      else if (Op->getPred() > RHS.Op->getPred()) return false;

      return Op->getSlot() < RHS.Op->getSlot();
    }
  };

private:
  struct LiveInInfo {
    uint32_t Cycles;

    LiveInInfo(uint32_t Cycles = 0) : Cycles(Cycles) {}

    uint32_t getCycles() const { return Cycles; }

    void incCycles(int Inc = 1) { Cycles += Inc; }
  };

  // Define the VAS set for the reaching definition dense map.
  typedef std::set<VNInfo> VASSetTy;
  typedef std::map<VNInfo, SlotInfo::LiveInInfo> VASCycMapTy;
  const VASTSlot *S;
  // Define Set for the reaching definition.
  VASSetTy SlotGen;
  typedef std::set<VASTSeqValue*> ValueSet;
  ValueSet OverWrittenValue;
  // In/Out set with cycles form define information.
  VASCycMapTy SlotIn;
  VASCycMapTy SlotOut;

  typedef VASSetTy::iterator gen_iterator;
  // get the iterator of the defining map of reaching definition.
  gen_iterator gen_begin() const { return SlotGen.begin(); }
  gen_iterator gen_end() const { return SlotGen.end(); }

  typedef
  std::pointer_to_unary_function<std::pair<VASTSeqDef, unsigned>, VASTSeqDef>
  vas_getter;

  static bool updateLiveIn(VNInfo D, SlotInfo::LiveInInfo NewLI, VASCycMapTy &S) {
    assert(NewLI.getCycles() && "It takes at least a cycle to live in!");
    SlotInfo::LiveInInfo &Info = S[D];

    if (Info.Cycles == 0 || Info.Cycles > NewLI.Cycles) {
      // Try to take the shortest path.
      Info = NewLI;
      return true;
    }

    return false;
  }

  SlotInfo::LiveInInfo getLiveIn(VNInfo D) const {
    vascyc_iterator at = SlotIn.find(D);
    return at == SlotIn.end() ? SlotInfo::LiveInInfo() : at->second;
  }

  // Initialize the out set by simply copying the gen set, and initialize the
  // cycle counter to 0.
  void initOutSet();

  // Insert VAS into different set.
  void insertGen(VNInfo D) {
    SlotGen.insert(D);
    insertOvewritten(D.Dst);
  }

  void insertOvewritten(VASTSeqValue *V);

  bool insertIn(VNInfo D, SlotInfo::LiveInInfo NewLI) {
    return updateLiveIn(D, NewLI, SlotIn);
  }

  bool insertOut(VNInfo D, SlotInfo::LiveInInfo NewLI) {
    return updateLiveIn(D, NewLI, SlotOut);
  }

  friend class SeqReachingDefAnalysis;
public:
  SlotInfo(const VASTSlot *s) : S(s) {}

  typedef VASCycMapTy::const_iterator vascyc_iterator;
  typedef mapped_iterator<VASCycMapTy::iterator, vas_getter> iterator;

  vascyc_iterator in_begin() const { return SlotIn.begin(); }
  vascyc_iterator in_end() const { return SlotIn.end(); }
  vascyc_iterator out_begin() const { return SlotOut.begin(); }
  vascyc_iterator out_end() const { return SlotOut.end(); }

  bool isVASKilled(VNInfo VN) const;

  // Get the distance (in cycles) from the define slot of the VAS to this slot.
  unsigned getCyclesFromDef(VASTSeqUse D) const {
    return getLiveIn(D).getCycles();
  }

  // Get Slot pointer.
  const VASTSlot *getSlot() { return S; }

  void print(raw_ostream &OS) const;
  void dump() const;
};

// The RtlSSAAnalysis that construct the SSA form.
class SeqReachingDefAnalysis : public VASTModulePass {
public:
  // define VASVec for the SVNInfo.
  typedef std::vector<VASTSeqDef> VASVec;
  typedef VASVec::iterator vasvec_it;

  // Define small vector for the slots.
  typedef SmallVector<VASTSlot*, 4> SlotVecTy;
  typedef SlotVecTy::iterator slot_vec_it;

  typedef std::map<const VASTSlot* ,SlotInfo*> SlotInfoTy;
  typedef SlotInfoTy::const_iterator slotinfo_it;

private:
  SlotInfoTy SlotInfos;
  SlotVecTy SlotVec;

  // define VAS assign iterator.
  typedef VASTSeqValue::const_itertor vn_itertor;

  VASTModule *VM;
  BumpPtrAllocator Allocator;

  bool addLiveIns(SlotInfo *From, SlotInfo *To, bool FromAliasSlot);

  // Using the reaching definition algorithm to sort out the ultimate
  // relationship of registers.
  // Dirty hack: maybe there are two same statements is a slot, and we can use
  // bit vector to implement the algorithm similar to the compiler principle.
  void ComputeReachingDefinition();

  // collect the Generated and Killed statements of the slot.
  void ComputeGenAndKill();
  void ComputeGenAndKill(const VASTSeqDef &D);
public:
  static char ID;

  slot_vec_it slot_begin() { return SlotVec.begin(); }
  slot_vec_it slot_end() { return SlotVec.end(); }

  // Get SlotInfo from the existing SlotInfos set.
  SlotInfo* getSlotInfo(const VASTSlot *S) const;

  void releaseMemory() {
    Allocator.Reset();
    SlotVec.clear();
    SlotInfos.clear();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);

  SeqReachingDefAnalysis();
};
}
#endif
