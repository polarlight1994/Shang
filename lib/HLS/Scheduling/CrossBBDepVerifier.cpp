//===- CrossBBChainFixer.cpp - Fix interval for cross BB chains -*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the CrossBBChainFixer, which fix the interval for the
// corss BB chains.
//
//===----------------------------------------------------------------------===//

#include "VASTScheduling.h"
#include "vast/VASTModule.h"
#include "llvm/IR/Function.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "cross-bb-dep-verifier"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct Verifier {
  Function &F;
  VASTSchedGraph &G;
  DominatorTree *DT;

  std::map<BasicBlock*, unsigned> SPDFromEntry;

  Verifier(VASTSchedGraph &G, DominatorTree *DT)
    : F(G.getFunction()), G(G), DT(DT) {}

  void initialize();
  void verify();

  unsigned computeExpectedSPDFromEntry(ArrayRef<VASTSchedUnit*> SUs);
  unsigned computeActualSPDFromEntry(VASTSchedUnit *Entry, unsigned ExpectedSPD);

  void ensureNoFlowDepFrom(BasicBlock *FromBB, BasicBlock *ToBB);
};
}

unsigned Verifier::computeActualSPDFromEntry(VASTSchedUnit *Entry,
                                             unsigned ExpectedSPD) {
  typedef VASTSchedUnit::dep_iterator dep_iterator;
  unsigned ActualSPD = UINT32_MAX;
  // A flag to indicate if the current Entry is connected to any its predecessors
  // exit, i.e. the current entry and the exit are scheduled to the same slot.
  bool IsConnected = false;

  for (dep_iterator I = Entry->dep_begin(), E = Entry->dep_end(); I != E; ++I) {
    VASTSchedUnit *Src = *I;

    if (!Src->isTerminator()) continue;

    if (I.getEdgeType() != VASTDep::Conditional) continue;
    
    BasicBlock *SrcBB = Src->getParent();
    unsigned SrcSPDFromEntry = SPDFromEntry[SrcBB];

    // For the nonstructural CFG, we may get a back edge. For this kind of edge
    // the flow-dependencies from the source BB should be break by a PHI node.
    // And it is ok to ignore such incoming block.
    if (SrcSPDFromEntry == 0) {
      assert(!DT->properlyDominates(SrcBB, Entry->getParent())
             && "Dominator edge not handled correctly!");
      continue;
    }

    VASTSchedUnit *SrcEntry = G.getEntrySU(SrcBB);
    unsigned TotalSlots = Src->getSchedule() - SrcEntry->getSchedule();
    IsConnected |= Src->getSchedule() == Entry->getSchedule();

    // Get the distance from the entry of IDom to the exit of predecessor BB.
    // It is also the distance from the entry of IDom through predecessor BB
    // to the entry of current BB.
    unsigned Entry2ExitDistanceFromEntry = SrcSPDFromEntry + TotalSlots;

    int ExtraLatency = int(ExpectedSPD) - int(Entry2ExitDistanceFromEntry);
    if (LLVM_UNLIKELY(ExtraLatency > 0)) {
      dbgs() << "Allocate extra " << ExtraLatency << " wait state: "
             << SrcBB->getName() << " -> "
             << Entry->getParent()->getName() <<'\n';
      llvm_unreachable("As we support conditional dependencies in ILP model,"
                       "we should not get a cross bb dependency violation!");
    }

    ActualSPD = std::min(ActualSPD, Entry2ExitDistanceFromEntry);
  }

  assert(IsConnected && "No shortest path to current block?");

  // After delay operations are inserted, the actual distance from IDom is no
  // smaller than the expected distance.
  return std::max(ActualSPD, ExpectedSPD);
}

unsigned
Verifier::computeExpectedSPDFromEntry(ArrayRef<VASTSchedUnit*> SUs) {
  assert(SUs[0]->isBBEntry() && "BBEntry not placed at the beginning!");
  unsigned EntrySlot = SUs[0]->getSchedule();
  BasicBlock *BB = SUs[0]->getParent();
  unsigned CurSPDFromEntry = 0;

  for (unsigned i = 1, e = SUs.size(); i != e; ++i) {
    VASTSchedUnit *U = SUs[i];

    unsigned USlot = U->getSchedule();
    unsigned UOffset = USlot - EntrySlot;

    typedef VASTSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator I = U->dep_begin(), E = U->dep_end(); I != E; ++I) {
      if (I.getEdgeType() == VASTDep::LinearOrder
          || I.getEdgeType() == VASTDep::Conditional
          || I.getEdgeType() == VASTDep::Synchronize
          || I.getEdgeType() == VASTDep::CtrlDep)
       continue;

      VASTSchedUnit *Src = *I;

      // When the U depends on the function argument, there is a dependency
      // edge from the entry of the whole scheduling graph. Do not fail in this
      // case.
      if (Src->isEntry()) continue;

      BasicBlock *SrcBB = Src->getParent();
      if (SrcBB == BB) continue;

      VASTSchedUnit *SrcEntry = G.getEntrySU(SrcBB);

      // Get the required entry-to-entry distance from source BB to current BB.
      int E2EDistance = I.getLatency() - UOffset
          + (Src->getSchedule() - SrcEntry->getSchedule());
      // Get the actual distance from SrcBB to IDom.
      int Src2EntryDistance = SPDFromEntry[SrcBB];
      assert(Src2EntryDistance && "SrcBB not visited?");
      // Calculate the required distance from Entry to Current BB.
      int Entry2BBDistance = E2EDistance + Src2EntryDistance;

      CurSPDFromEntry = std::max<int>(CurSPDFromEntry, Entry2BBDistance);
    }
  }

  return CurSPDFromEntry;
}

void Verifier::initialize() {
  G.sortSUsByIdx();

  BasicBlock *Entry = &F.getEntryBlock();
  VASTSchedUnit *EntrySU = G.getEntrySU(Entry);
  SPDFromEntry[Entry] = EntrySU->getSchedule();
}

void Verifier::verify() {
  // Visit the basic block in topological order.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    BasicBlock *BB = *I;

    DEBUG(dbgs() << "Visiting: " << BB->getName() << '\n');

    if (BB == &F.getEntryBlock()) continue;

    ArrayRef<VASTSchedUnit*> SUs(G.getSUInBB(BB));
    unsigned ExpectedSPD = computeExpectedSPDFromEntry(SUs);
    unsigned ActualSPD = computeActualSPDFromEntry(SUs.front(), ExpectedSPD);

    SPDFromEntry[BB] = ActualSPD;
  }
}

//===----------------------------------------------------------------------===//
void VASTScheduling::verifyCrossBBDeps() {
  Verifier V(*G, DT);

  V.initialize();
  V.verify();
}
