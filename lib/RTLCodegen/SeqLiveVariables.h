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

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Support/Allocator.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/PointerIntPair.h"

#include <map>
#include <set>

namespace llvm {
class VASTValue;
struct VASTSeqUse;
struct VASTSeqDef;
class VASTSeqOp;
class VASTSeqValue;
class VASTSlot;
class VASTModule;
template<class PtrType, unsigned SmallSize> class SmallPtrSet;
class MachineBasicBlock;
class MachineInstr;

class SeqLiveVariables : public MachineFunctionPass {
public:
  static char ID;

  SeqLiveVariables();

  struct VarInfo {
    // DefinedByPHI - Set to true if this variable defined by PHI node.
    PointerIntPair<MachineInstr*, 1, bool> DefMI;

    explicit VarInfo(MachineInstr *MI = 0, bool IsPHI = false)
      : DefMI(MI, IsPHI) {}

    bool isPHI() const { return DefMI.getInt(); }

    /// AliveSlots - Set of Slots at which this value is defined.  This is a bit
    /// set which uses the Slot number as an index.
    ///
    SparseBitVector<> DefSlots;

    /// AliveSlots - Set of Slots in which this value is alive completely
    /// through.  This is a bit set which uses the Slot number as an index.
    ///
    SparseBitVector<> AliveSlots;

    /// Kills - List of VASTSeqOp's which are the last use of this VASTSeqDef.
    ///
    SparseBitVector<> Kills;

    void verify() const;

    void dump() const;
  };

  // The overrided function of MachineFunctionPass.
  bool runOnMachineFunction(MachineFunction &MF);
  void getAnalysisUsage(AnalysisUsage &AU) const;
  void releaseMemory();
  void verifyAnalysis() const;
private:
  typedef const std::vector<VASTSlot*> PathVector;
  void handleSlot(VASTSlot *S, PathVector &PathFromEntry);
  void handleUse(VASTSeqValue *Use, VASTSlot *UseSlot, PathVector &PathFromEntry);
  void handleDef(VASTSeqDef D);

  struct VarName {
    VASTSeqValue *Dst;
    VASTSlot *S;

    /*implicit*/ VarName(VASTSeqDef D);

    /*implicit*/ VarName(VASTSeqUse U);

    VarName(VASTSeqValue *Dst, VASTSlot *S) : Dst(Dst), S(S) {}

    bool operator<(const VarName &RHS) const {
      if (Dst < RHS.Dst) return true;
      else if (Dst > RHS.Dst) return false;

      return S < RHS.S;
    }
  };

  BumpPtrAllocator Allocator;
  // The value information for each definitions.
  std::map<VarName, VarInfo*> VarInfos;

  // Get the corresponding VarInfo, create the VarInfo if necessary.
  VarInfo *getVarInfo(VarName VN) const {
    std::map<VarName, VarInfo*>::const_iterator at = VarInfos.find(VN);
    assert(at != VarInfos.end() && "Value use before define!");
    return at->second;
  }

  // The Slots the writing a specific SeqValue.
  std::map<VASTSeqValue*, SparseBitVector<> > WrittenSlots;

  bool isWrittenAt(VASTSeqValue *V, VASTSlot *S);

  // Create the VarInfo for PHINodes.
  void createInstVarInfo(VASTModule *VM);

  // Debug Helper functions.
  static void dumpVarInfoSet(SmallPtrSet<VarInfo*, 8> VIs);
};
}

#endif
