//===---- OverlappedSlots.cpp - Identify the overlapped slots ----*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the OverlapSlots analysis. The analysis identifies the
// non-mutually exclusive slots with overlapped timeframes. This can happened
// after we relax the control dependencies from/to the boundaries of the basic
// blocks.
//
//===----------------------------------------------------------------------===//

#include "OverlappedSlots.h"
#include "STGShortestPath.h"

#include "shang/VASTSlot.h"
#include "shang/VASTModule.h"
#include "shang/Passes.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "vast-overlapped-slot"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(NumOverlappeds, "Number of overlapped slots");

char OverlappedSlots::ID = 0;
char &llvm::OverlappedSlotsID = OverlappedSlots::ID;

INITIALIZE_PASS_BEGIN(OverlappedSlots, "vast-overlapped-slot",
                      "Identify the timeframe overlapped slots",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(STGShortestPath)
INITIALIZE_PASS_END(OverlappedSlots, "vast-overlapped-slot",
                    "Identify the timeframe overlapped slots",
                    false, true)

OverlappedSlots::OverlappedSlots() : VASTModulePass(ID), STP(0) {
  initializeOverlappedSlotsPass(*PassRegistry::getPassRegistry());
}

void OverlappedSlots::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequired<STGShortestPath>();
  AU.setPreservesAll();
}

void OverlappedSlots::buildOverlappedMap(VASTSlot *S,
                                         ArrayRef<VASTSlot*> StraightFlow) {
  typedef df_iterator<VASTSlot*> slot_df_iterator;
  for (slot_df_iterator DI = df_begin(S), DE = df_end(S); DI != DE; /*++DI*/) {
    VASTSlot *Child = *DI;

    unsigned Distance = Child == S ? 0
                        : STP->getShortestPath(S->SlotNum, Child->SlotNum);

    // Ignore the slots that not overlap with any of the slot in StraightFlow.
    if (Distance > StraightFlow.size()) {
      DI.skipChildren();
      continue;
    }

    if (Distance) {
      unsigned Offset = Distance - 1;
      VASTSlot *OverlappedSlot = StraightFlow[Offset];
      Overlappeds[OverlappedSlot->SlotNum].set(Child->SlotNum);
      ++NumOverlappeds;
    }

    ++DI;
  }
}

void OverlappedSlots::buildOverlappedMap(VASTSlot *S) {
  // Build the straight line control flow.
  SmallVector<VASTSlot*, 8> StraightFlow;
  SmallVector<VASTSlot*, 8> ConditionalBroundaries;

  typedef df_iterator<VASTSlot*> slot_df_iterator;
  for (slot_df_iterator DI = df_begin(S), DE = df_end(S); DI != DE; /*++DI*/) {
    VASTSlot *Child = *DI;

    // Skip the current slot.
    if (Child == S) {
      ++DI;
      continue;
    }

    // The edges to the successors of a subgroup are all conditional.
    if (Child->IsVirtual) {
      // Collect the (conditional) successor of S.
      if (DI.getPathLength() == 2)
        ConditionalBroundaries.push_back(Child);

      DI.skipChildren();
      continue;
    }

    // Now child is a part of the straight line control flow.
    StraightFlow.push_back(Child);
    ++DI;
  }

  // Build the overlapped map from the target of side branch.
  while (!ConditionalBroundaries.empty())
    buildOverlappedMap(ConditionalBroundaries.pop_back_val(), StraightFlow);
}

static bool HasSideBranch(VASTSlot *S) {
  bool HasNonVirtualSucc = false;
  bool HasVirtualSucc = false;
  typedef VASTSlot::succ_iterator iterator;
  for (iterator I = S->succ_begin(), E = S->succ_end(); I != E; ++I) {
    VASTSlot *Succ = *I;
    HasVirtualSucc |= Succ->IsVirtual;
    HasNonVirtualSucc |= !Succ->IsVirtual;
  }

  return HasNonVirtualSucc && HasVirtualSucc;
}

bool OverlappedSlots::runOnVASTModule(VASTModule &VM) {
  STP = &getAnalysis<STGShortestPath>();

  // 1. Collect find all side-branching slot like this:
  //  S1
  //  | \
  //  S2 S3
  // Where edge (S1, S2) is unconditional while edge (S1, S2) is conditional.

  typedef VASTModule::slot_iterator iterator;
  for (iterator I = VM.slot_begin(), E = VM.slot_end(); I != E; ++I) {
    VASTSlot *S = I;

    if (S->IsVirtual) continue;

    if (HasSideBranch(S)) buildOverlappedMap(S);
  }

  dump();

  return false;
}

void OverlappedSlots::releaseMemory() {
  Overlappeds.clear();
}

void OverlappedSlots::print(raw_ostream &OS) const {
  typedef DenseMap<unsigned, SparseBitVector<> >::const_iterator iterator;
  for (iterator I = Overlappeds.begin(), E = Overlappeds.end(); I != E; ++I) {
    OS << "S#" << I->first << " -> ";
    ::dump(I->second, OS);
    OS << '\n';
  }
}
