//===- CrossBBChainFixer.cpp - Fix interval for cross BB chains -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
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
#include "shang/VASTModule.h"
#include "llvm/IR/Function.h"

#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "cross-bb-interval-fixer"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(AllocatedWaitStates,
          "Number of wait states inserted to fix the inter-bb latency");

namespace {
struct IntervalFixer {
  VASTModule &VM;
  Function &F;
  VASTSchedGraph &G;

  std::map<BasicBlock*, unsigned> SPDFromEntry;

  IntervalFixer(VASTModule &VM, VASTSchedGraph &G) : VM(VM), F(VM), G(G) {}

  void initialize();
  void fixInterval();

  unsigned computeExpectedSPDFromEntry(ArrayRef<VASTSchedUnit*> SUs);
  unsigned allocateWaitStates(VASTSchedUnit *Entry, unsigned ExpectedSPD);

  void ensureNoFlowDepFrom(BasicBlock *FromBB, BasicBlock *ToBB);
};
}

void IntervalFixer::ensureNoFlowDepFrom(BasicBlock *FromBB, BasicBlock *ToBB) {
#ifndef NDEBUG
  ArrayRef<VASTSchedUnit*> SUs(G.getSUInBB(ToBB));

  for (unsigned i = 0, e = SUs.size(); i != e; ++i) {
    VASTSchedUnit *U = SUs[i];

    typedef VASTSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator I = U->dep_begin(), E = U->dep_end(); I != E; ++I) {
      VASTSchedUnit *Src = *I;

      if (Src->isEntry()) continue;

      if (I.getEdgeType() == VASTDep::LinearOrder
          || I.getEdgeType() == VASTDep::Conditional)
       continue;

      if (Src->getParent() == FromBB)
        llvm_unreachable("Source of flow dependencies not dominates all its use!");
    }
  }
#endif
}

unsigned IntervalFixer::allocateWaitStates(VASTSchedUnit *Entry,
                                           unsigned ExpectedSPD) {
  typedef VASTSchedUnit::dep_iterator dep_iterator;
  unsigned ActualSPD = UINT32_MAX;

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
      ensureNoFlowDepFrom(SrcBB, Entry->getParent());
      continue;
    }

    VASTSchedUnit *SrcEntry = G.getSUInBB(SrcBB).front();
    assert(SrcEntry->isBBEntry() && "Bad SU order!");
    unsigned TotalSlots = Src->getSchedule() - SrcEntry->getSchedule();
    
    // Get the distance from the entry of IDom to the exit of predecessor BB.
    // It is also the distance from the entry of IDom through predecessor BB
    // to the entry of current BB.
    unsigned Entry2ExitDistanceFromEntry = SrcSPDFromEntry + TotalSlots;

    int ExtraLatency = int(ExpectedSPD) - int(Entry2ExitDistanceFromEntry);
    if (ExtraLatency > 0) {
      dbgs() << "Allocate extra " << ExtraLatency << " wait state: "
             << SrcBB->getName() << " -> "
             << Entry->getParent()->getName() <<'\n';
      llvm_unreachable("As we support conditional dependencies in ILP model,"
                        "we should not get a cross bb interval violation!");
    }

    ActualSPD = std::min(ActualSPD, Entry2ExitDistanceFromEntry);
  }

  // After delay operations are inserted, the actual distance from IDom is no
  // smaller than the expected distance.
  return std::max(ActualSPD, ExpectedSPD);
}

unsigned
IntervalFixer::computeExpectedSPDFromEntry(ArrayRef<VASTSchedUnit*> SUs) {
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
      VASTSchedUnit *Src = *I;

      // When the U depends on the function argument, there is a dependency
      // edge from the entry of the whole scheduling graph. Do not fail in this
      // case.
      if (Src->isEntry()) continue;

      BasicBlock *SrcBB = Src->getParent();
      if (SrcBB == BB) continue;

      if (I.getEdgeType() == VASTDep::LinearOrder
          || I.getEdgeType() == VASTDep::Conditional)
       continue;

      VASTSchedUnit *SrcEntry = G.getSUInBB(SrcBB).front();
      assert(SrcEntry->isBBEntry() && "BBMap broken!");

      // Get the required entry-to-entry distance from source BB to current BB.
      int E2EDistance = I.getLatency() - UOffset
        + (Src-> getSchedule() - SrcEntry->getSchedule());
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

static int top_sort_idx(const VASTSchedUnit *LHS, const VASTSchedUnit *RHS) {
  if (LHS->getSchedule() != RHS->getSchedule())
    return LHS->getSchedule() < RHS->getSchedule() ? -1 : 1;

  if (LHS->getIdx() < RHS->getIdx()) return -1;
  if (LHS->getIdx() > RHS->getIdx()) return 1;

  return 0;
}

static int top_sort_idx_wrapper(const void *LHS, const void *RHS) {
  return top_sort_idx(*reinterpret_cast<const VASTSchedUnit* const *>(LHS),
                      *reinterpret_cast<const VASTSchedUnit* const *>(RHS));
}

void IntervalFixer::initialize() {
  G.sortSUs(top_sort_idx_wrapper);

  BasicBlock *Entry = &F.getEntryBlock();
  VASTSchedUnit *EntrySU = G.getSUInBB(Entry).front();
  assert(EntrySU->isBBEntry() && "BBEntry not placed at the beginning!");
  SPDFromEntry[Entry] = EntrySU->getSchedule();
}

void IntervalFixer::fixInterval() {
  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;

    DEBUG(dbgs() << "Visiting: " << BB->getName() << '\n');

    if (BB == &F.getEntryBlock()) continue;

    ArrayRef<VASTSchedUnit*> SUs(G.getSUInBB(BB));
    unsigned ExpectedSPD = computeExpectedSPDFromEntry(SUs);
    unsigned ActualSPD = allocateWaitStates(SUs.front(), ExpectedSPD);

    SPDFromEntry[BB] = ActualSPD;
  }
}

//===----------------------------------------------------------------------===//
void VASTSchedGraph::fixIntervalForCrossBBChains(VASTModule &VM) {
  IntervalFixer Fixer(VM, *this);

  Fixer.initialize();
  Fixer.fixInterval();
}
