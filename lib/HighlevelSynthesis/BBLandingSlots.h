//===- BBLandingSlots.h - Compute Landing Slots for Basic Blocks -*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the BBLandingSlot pass. The BBLandingSlot pass compute the
// landing slot, i.e. the first slot that is reachable by all predecessor of
// the BasicBlock. Sometimes there is more than 1 landing slots for a specified
// BasicBlock because of the CFG folding in Scheduling.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_BB_LANDING_SLOT_H
#define VAST_BB_LANDING_SLOT_H

#include "shang/VASTModulePass.h"

#include "llvm/ADT/DenseSet.h"

namespace llvm {
class BasicBlock;
class VASTSlot;
class VASTModule;

class BBLandingSlots : public VASTModulePass {
  DenseMap<const BasicBlock*, VASTSlot*> BB2Slots;
  DenseMap<const BasicBlock*, DenseSet<VASTSlot*> > LandingSlots;
  VASTModule *VM;

public:
  static char ID;

  BBLandingSlots();

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
  void print(raw_ostream &OS) const;

  typedef DenseSet<VASTSlot*> SlotSet;
  const SlotSet &getLandingSlots(const BasicBlock *BB);
};
}

#endif
