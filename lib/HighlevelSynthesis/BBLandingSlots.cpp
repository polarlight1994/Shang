//==- BBLandingSlots.cpp - Compute Landing Slots for Basic Blocks -*-C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the BBLandingSlot pass. The BBLandingSlot pass compute
// the landing slot, i.e. the first slot that is reachable by all predecessor of
// the BasicBlock. Sometimes there is more than 1 landing slots for a specified
// BasicBlock because of the CFG folding in Scheduling.
//
//===----------------------------------------------------------------------===//

#include "BBLandingSlots.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/Support/CFG.h"
#define DEBUG_TYPE "vast-bb-landing-slots"
#include "llvm/Support/Debug.h"

using namespace llvm;

char BBLandingSlots::ID = 0;

INITIALIZE_PASS(BBLandingSlots, "vast-bb-landing-slots",
                "Compute the Landing Slots for the BasicBlocks",
                false, true);

BBLandingSlots::BBLandingSlots() : VASTModulePass(ID), VM(0) {
  initializeBBLandingSlotsPass(*PassRegistry::getPassRegistry());
}

void BBLandingSlots::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.setPreservesAll();
}

void BBLandingSlots::releaseMemory() {
  LandingSlots.clear();
}

bool BBLandingSlots::runOnVASTModule(VASTModule &VM) {
  this->VM = &VM;

  // Remember the direct BB -> Slot mapping.
  typedef VASTModule::slot_iterator iterator;
  for (iterator I = VM.slot_begin(), E = VM.slot_end(); I != E; ++I) {
    VASTSlot *S = I;
    BasicBlock *BB = S->getParent();
    VASTSlot *&FirstSlot = BB2Slots[BB];
    if (FirstSlot) {
      assert(S->SlotNum > FirstSlot->SlotNum && "Broken slot number!");
      continue;
    }

    // It is the first time we visit the slot of this BB. It should be the entry
    // Slot of this BB.
    FirstSlot = S;
  }
  

  return false;
}

void BBLandingSlots::print(raw_ostream &OS) const { }


const BBLandingSlots::SlotSet &
BBLandingSlots::getLandingSlots(const BasicBlock *BB) {
  SlotSet &Slots = LandingSlots[BB];

  if (Slots.empty()) {
    // Build the landing slot set now. Perform depth first search until we
    // land to some slots.
    typedef df_iterator<const BasicBlock*> cfg_df_iterator;
    for (cfg_df_iterator I = df_begin(BB), E = df_end(BB); I != E; /*++I*/) {
      const BasicBlock *SuccBB = *I;
      if (VASTSlot *LandingSlot = BB2Slots.lookup(SuccBB)) {
        // Stop the depth first search if we just landed into some slots.
        Slots.insert(LandingSlot);
        I = I.skipChildren();
        continue;
      }

      ++I;
    }
  }

  return Slots;
}
