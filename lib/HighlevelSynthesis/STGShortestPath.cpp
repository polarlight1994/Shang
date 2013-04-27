//==- STGShortestPath.cpp - Shortest Path Distance between States -*-C++ -*-==//
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

#include "STGShortestPath.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CFG.h"
#define DEBUG_TYPE "vast-stg-shortest-path"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(NumIterations, "Number of iterations in the STP algorithm");

char STGShortestPath::ID = 0;
const unsigned STGShortestPath::Inf = UINT16_MAX;

INITIALIZE_PASS(STGShortestPath, "vast-stg-shortest-path",
                "Compute the Landing Slots for the BasicBlocks",
                false, true)

STGShortestPath::STGShortestPath() : VASTModulePass(ID), VM(0) {
  initializeSTGShortestPathPass(*PassRegistry::getPassRegistry());
}

void STGShortestPath::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.setPreservesAll();
}

void STGShortestPath::releaseMemory() {
  STPMatrix.clear();
  VM = 0;
}

bool STGShortestPath::runOnVASTModule(VASTModule &VM) {
  this->VM = &VM;
  // Initialize the neighbor weight.
  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator I = VM.slot_begin(), E = VM.slot_end(); I != E; ++I) {
    VASTSlot *Src = I;

    typedef VASTSlot::succ_iterator succ_iterator;
    for (succ_iterator SI = Src->succ_begin(), SE = Src->succ_end(); SI != SE; ++SI) {
      VASTSlot *Dst = *SI;
      assert(Src != Dst && "Unexpected loop!");
      STPMatrix[Dst->SlotNum][Src->SlotNum] = Dst->IsVirtual ? 0 : 1;
    }
  }

  // Visit the slots in topological order.
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >
    RPO(VM.getStartSlot());

  typedef
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >::rpo_iterator
  slot_top_iterator;

  bool changed = true;

  while (changed) {
    changed = false;
    ++NumIterations;

    // Use the Floyd Warshal algorithm to compute the shortest path.
    for (slot_top_iterator I =RPO.begin(), E = RPO.end(); I != E; ++I) {
      VASTSlot *To = *I;

      typedef VASTSlot::pred_iterator pred_iterator;
      for (pred_iterator PI = To->pred_begin(), PE = To->pred_end(); PI != PE; ++PI) {
        VASTSlot *Thu = *PI;

        DenseMap<unsigned, unsigned> &Srcs = STPMatrix[Thu->SlotNum];
        typedef DenseMap<unsigned, unsigned>::iterator from_iterator;
        for (from_iterator FI = Srcs.begin(), FE = Srcs.end(); FI != FE; ++FI) {
          //D[i][j] = min( D[i][j], D[i][k] + D[k][j]
          unsigned DistanceToThuFI = FI->second + 1;
          unsigned DistanceToFI = getShortestPath(FI->first, To->SlotNum);
          if (DistanceToThuFI < DistanceToFI) {
            STPMatrix[To->SlotNum][FI->first] = DistanceToThuFI;
            changed = true;
          }
        }
      }
    }
  }

  return false;
}

void STGShortestPath::print(raw_ostream &OS) const {
  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator I = VM->slot_begin(), IE = VM->slot_end(); I != IE; ++I) {
    for (slot_iterator J = VM->slot_begin(), JE = VM->slot_end(); J != JE; ++J) {
      OS << '[' << I->SlotNum << ',' << J->SlotNum << "] = ";
      unsigned Distance = getShortestPath(I->SlotNum, J->SlotNum);
      if (Distance == Inf) OS << "Inf";
      else                 OS << Distance;
      OS << ",\t";
    }
    OS << '\n';
  }
}

unsigned STGShortestPath::getShortestPath(unsigned From, unsigned To) const {
  DenseMap<unsigned, DenseMap<unsigned, unsigned> >::const_iterator
    to_at = STPMatrix.find(To);

  if (to_at == STPMatrix.end()) return Inf;

  DenseMap<unsigned, unsigned>::const_iterator from_at = to_at->second.find(From);

  if (from_at == to_at->second.end()) return Inf;

  return from_at->second;
}
