//===- SeqLiveVariables.h - LiveVariables analysis on the STG ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the LiveVariable Analysis on the state-transition graph.
//
//===----------------------------------------------------------------------===//

#ifndef SEQ_LIVEVARIABLES_H
#define SEQ_LIVEVARIABLES_H

#include "shang/VASTModulePass.h"

#include "llvm/IR/Value.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/PointerIntPair.h"

#include <map>
#include <set>

namespace llvm {
class VASTValue;
struct VASTLatch;
class VASTSeqOp;
class VASTSeqValue;
class VASTSlot;
class VASTModule;
class STGShortestPath;
template<class PtrType, unsigned SmallSize> class SmallPtrSet;
class BasicBlock;
class Value;

class SeqLiveVariables : public VASTModulePass {
  VASTModule *VM;

public:
  static char ID;

  SeqLiveVariables();

  struct VarInfo : public ilist_node<VarInfo> {
    // TODO: For the VASTSeqVal definition that does not corresponding to
    // an instruction, identify them by a extend PseudoSourceValue.
    Value *V;

    explicit VarInfo(Value *V = 0) : V(V) {}

    // Test the Read slot is reachable from the definition.
    // Please note that even the ReadSlotNum is equal to a define slot, it is
    // still not reachable if the slot is not in the alive slots.
    bool isSlotReachable(unsigned SlotNum) const {
      SparseBitVector<> TestBit;
      TestBit.set(SlotNum);
      return Alives.contains(TestBit) || Kills.contains(TestBit)
             || DefKills.contains(TestBit);
    }

    /// AliveSlots - Set of Slots at which this value is defined.  This is a bit
    /// set which uses the Slot number as an index.
    ///
    SparseBitVector<> Defs;

    /// AliveSlots - Set of Slots in which this value is alive completely
    /// through.  This is a bit set which uses the Slot number as an index.
    ///
    SparseBitVector<> Alives;

    /// Kills - Set of Slots which are the last use of this VASTSeqDef.
    ///
    SparseBitVector<> Kills;

    /// DefKill - The slot that the define is read, and the new define is
    /// available at the same time.
    ///
    SparseBitVector<> DefKills;

    /// Auxiliary Masks
    /// Landings - The slots immediately reachable by the define slot.
    SparseBitVector<> Landings;

    /// Overlappeds - The slots overlaps with the reachable slot of the current
    /// live variable.
    SparseBitVector<> Overlappeds;

    void initializeDefSlot(unsigned SlotNum) {
      // Initialize the define slot.
      Defs.set(SlotNum);
      // If vr is not alive in any block, then defaults to dead.
      Kills.set(SlotNum);
    }

    void verify() const;
    void verifyKillAndAlive() const;
    void print(raw_ostream &OS) const;
    void dump() const;
  };

  // The overrided function of MachineFunctionPass.
  bool runOnVASTModule(VASTModule &VM);
  void getAnalysisUsage(AnalysisUsage &AU) const;
  void releaseMemory();
  void verifyAnalysis() const;

  unsigned getIntervalFromDef(const VASTSeqValue *V, VASTSlot *ReadSlot,
                              STGShortestPath *SSP) const;

  void print(raw_ostream &OS) const;
private:
  typedef ArrayRef<VASTSlot*> PathVector;
  void handleSlot(VASTSlot *S, PathVector PathFromEntry);
  void handleUse(VASTSeqValue *Use, VASTSlot *UseSlot, PathVector PathFromEntry);

  void initializeLandingSlots();
  void initializeOverlappedSlots();

  // The value information for each definitions. Please note that the
  // VASTSeqValue is supposed to be in SSA form.
  std::map<const VASTSeqValue*, VarInfo*> VarInfos;
  iplist<VarInfo> VarList;
  typedef iplist<VarInfo>::iterator var_iterator;
  typedef iplist<VarInfo>::const_iterator const_var_iterator;

  // The Slots the writing a specific SeqValue.
  std::map<VASTSeqValue*, SparseBitVector<> > WrittenSlots;

  bool isWrittenAt(VASTSeqValue *V, VASTSlot *S);

  // Create the VarInfo for PHINodes.
  void createInstVarInfo(VASTModule *VM);

  // Debug Helper functions.
  static void dumpVarInfoSet(SmallPtrSet<VarInfo*, 8> VIs);

public:
  // Get the corresponding VarInfo, create the VarInfo if necessary.
  VarInfo *getVarInfo(const VASTSeqValue *Val) const {
    std::map<const VASTSeqValue*, VarInfo*>::const_iterator
      at = VarInfos.find(Val);
    assert(at != VarInfos.end() && "Value use before define!");
    return at->second;
  }
};
}

#endif
