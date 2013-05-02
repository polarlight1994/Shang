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

#include "shang/Passes.h"

#define DEBUG_TYPE "vast-overlapped-slot"
#include "llvm/Support/Debug.h"

using namespace llvm;

char OverlappedSlots::ID = 0;

INITIALIZE_PASS(OverlappedSlots, "vast-overlapped-slot",
                "Identify the timeframe overlapped slots",
                false, true)

OverlappedSlots::OverlappedSlots() : VASTModulePass(ID) {
  initializeOverlappedSlotsPass(*PassRegistry::getPassRegistry());
}

void OverlappedSlots::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequired<STGShortestPath>();
  AU.setPreservesAll();
}

bool OverlappedSlots::runOnVASTModule(VASTModule &VM) {
  return false;
}

void OverlappedSlots::releaseMemory() {

}

void OverlappedSlots::print(raw_ostream &OS) const {

}
