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
#include "llvm/Support/CFG.h"
#define DEBUG_TYPE "vast-bb-landing-slots"
#include "llvm/Support/Debug.h"

using namespace llvm;

char STGShortestPath::ID = 0;
const unsigned STGShortestPath::Inf = UINT16_MAX;

INITIALIZE_PASS(STGShortestPath, "vast-stg-shortest-path",
                "Compute the Landing Slots for the BasicBlocks",
                false, true)

STGShortestPath::STGShortestPath() : VASTModulePass(ID) {
  initializeSTGShortestPathPass(*PassRegistry::getPassRegistry());
}

void STGShortestPath::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.setPreservesAll();
}

void STGShortestPath::releaseMemory() {
  STPMatrix.clear();
}

bool STGShortestPath::runOnVASTModule(VASTModule &VM) {
  // Initialize the neighbor weight.
  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator I = VM.slot_begin(), E = VM.slot_end(); I != E; ++I) {
    VASTSlot *Src = I;

    STPMatrix[Idx(Src->SlotNum, Src->SlotNum)] = 0;

    typedef VASTSlot::succ_iterator succ_iterator;
    for (succ_iterator SI = Src->succ_begin(), SE = Src->succ_end(); SI != SE; ++SI) {
      VASTSlot *Dst = *SI;

      STPMatrix[Idx(Src->SlotNum, Dst->SlotNum)] = 1;
    }
  }

  // Visit the slots in topological order.
  //ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >
  //  RPO(VM.getStartSlot());

  //typedef
  //ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >::rpo_iterator
  //slot_top_iterator;

  // Use the Floyd Warshal algorithm to compute the shortest path.
  for (slot_iterator K = VM.slot_begin(), KE = VM.slot_end(); K != KE; ++K)
    for (slot_iterator I = VM.slot_begin(), IE = VM.slot_end(); I != IE; ++I)
      for (slot_iterator J = VM.slot_begin(), JE = VM.slot_end(); J != JE; ++J) {
        //D[i][j] = min( D[i][j], D[i][k] + D[k][j]
        unsigned DistanceThu = getShortestPath(I->SlotNum, K->SlotNum)
                             + getShortestPath(K->SlotNum, J->SlotNum);

        if (DistanceThu >= Inf) continue;

        STPMatrix[Idx(I->SlotNum, J->SlotNum)]
          = std::min(getShortestPath(I->SlotNum, J->SlotNum), DistanceThu);
      }

  return false;
}

void STGShortestPath::print(raw_ostream &OS) const { }

unsigned STGShortestPath::getShortestPath(unsigned From, unsigned To) const {
  std::map<std::pair<unsigned, unsigned>, unsigned>::const_iterator
    at = STPMatrix.find(Idx(From, To));

  if (at == STPMatrix.end()) return Inf;

  return at->second;
}
