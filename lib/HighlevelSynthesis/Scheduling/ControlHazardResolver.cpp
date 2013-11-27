//==- ControlHazardResolver.cpp - Resolve Control Chaining Hazard -*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Control (Chaining) Hazard Resolver. Control chaning
// hazard occur when chaining is only required in the shortest path.
//
//===----------------------------------------------------------------------===//

#include "SDCScheduler.h"

#include "llvm/IR/Function.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/ADT/PostOrderIterator.h"
#define DEBUG_TYPE "vast-control-hazard"
#include "llvm/Support/Debug.h"

#include "lpsolve/lp_lib.h"

using namespace llvm;

namespace {
struct ControlChainingHazardResolver {
  SDCScheduler &Scheduler;
  VASTSchedGraph &G;
  DominatorTree &DT;

  typedef SchedulerBase::TimeFrame TimeFrame;

  static TimeFrame &reduce(TimeFrame &LHS, const TimeFrame &RHS) {
    LHS.ASAP = std::min<unsigned>(LHS.ASAP, RHS.ASAP);
    LHS.ALAP = std::max<unsigned>(LHS.ALAP, RHS.ALAP);
    return LHS;
  }

  std::map<const BasicBlock*, TimeFrame> BBTimeFrames;

  ControlChainingHazardResolver(SDCScheduler &Scheduler, DominatorTree &DT)
    : Scheduler(Scheduler), G(*Scheduler), DT(DT) {}

  TimeFrame getTimeFrame(const BasicBlock *BB) const {
    std::map<const BasicBlock*, TimeFrame>::const_iterator I
      = BBTimeFrames.find(BB);

    return I != BBTimeFrames.end() ? I->second : TimeFrame();
  }

  void initDistances();
  TimeFrame calculateTimeFrame(BasicBlock *BB);
  bool updateTimeFrame(BasicBlock *BB, TimeFrame NewTF);

  bool resolveHazard();
  bool resolveHazard(VASTSchedUnit *SU);
};
}


void ControlChainingHazardResolver::initDistances() {
  Function &F = G.getFunction();
  BBTimeFrames[&F.getEntryBlock()] = TimeFrame(0, 0);

  G.sortSUsByIdx();

  // Build the scheduling units according to the original scheduling.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

  for (unsigned i = 0; i < 2; ++i) {
    for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
      BasicBlock *BB = *I;

      TimeFrame Distance = calculateTimeFrame(BB);
      updateTimeFrame(BB, Distance);
    }
  }
}

ControlChainingHazardResolver::TimeFrame
ControlChainingHazardResolver::calculateTimeFrame(BasicBlock *BB) {
  VASTSchedUnit *Entry = G.getEntrySU(BB);
  TimeFrame CurTF;

  typedef VASTSchedUnit::dep_iterator dep_iterator;
  for (dep_iterator I = Entry->dep_begin(), E = Entry->dep_end(); I != E; ++I) {
    VASTSchedUnit *Src = *I;

    if (!Src->isTerminator()) continue;

    if (I.getEdgeType() != VASTDep::Conditional) continue;

    BasicBlock *SrcBB = Src->getParent();
    TimeFrame SrcEntryTF = getTimeFrame(SrcBB);

    // For the nonstructural CFG, we may get a back edge. For this kind of edge
    // the flow-dependencies from the source BB should be break by a PHI node.
    // And it is ok to ignore such incoming block.
    if (!SrcEntryTF) {
      assert(DT.dominates(BB, SrcBB) && "Expect backedge here!");
      continue;
    }

    VASTSchedUnit *SrcEntry = G.getEntrySU(SrcBB);
    unsigned SrcBBLength = Src->getSchedule() - SrcEntry->getSchedule();
    reduce(CurTF, SrcEntryTF + SrcBBLength);
  }

  return CurTF;
}

bool ControlChainingHazardResolver::updateTimeFrame(BasicBlock *BB,
  TimeFrame NewTF) {
  TimeFrame &TF = BBTimeFrames[BB];
  reduce(TF, NewTF);
  return true;
}

bool ControlChainingHazardResolver::resolveHazard() {
  bool HazardDetected = false;

  typedef VASTSchedGraph::iterator iterator;
  for (iterator I = G.begin(), E = G.end(); I != E; ++I)
    HazardDetected |= resolveHazard(I);

  return HazardDetected;
}

bool ControlChainingHazardResolver::resolveHazard(VASTSchedUnit *Dst) {
  if (LLVM_UNLIKELY(Dst->isVirtual()))
    return false;

  BasicBlock *BB = Dst->getParent();
  unsigned DstOffset = Dst->getSchedule() - G.getEntrySU(BB)->getSchedule();
  bool AnlyHazard = false;

  typedef VASTSchedUnit::dep_iterator dep_iterator;
  for (dep_iterator I = Dst->dep_begin(), E = Dst->dep_end(); I != E; ++I) {
    if (I.getEdgeType() == VASTDep::LinearOrder
        || I.getEdgeType() == VASTDep::Conditional
        || I.getEdgeType() == VASTDep::CtrlDep)
      continue;

    // Hazard only occur when the data dependency edge have 0 delay.
    if (I.getLatency() > 0)
      continue;

    VASTSchedUnit *Src = *I;

    // When the U depends on the function argument, there is a dependency
    // edge from the entry of the whole scheduling graph. Do not fail in this
    // case.
    if (Src->isEntry()) continue;

    // There is not hazard if Src and Dst are not scheduled to the same step.
    // Because hazard only occur when src and dst are scheduled to the same step,
    // which require chaining.
    if (Src->getSchedule() != Dst->getSchedule())
      continue;

    BasicBlock *SrcBB = Src->getParent();
    if (SrcBB == BB)
      continue;

    VASTSchedUnit *SrcEntry = G.getEntrySU(SrcBB);
    unsigned SrcOffset = Src->getSchedule() - SrcEntry->getSchedule();
    assert(getTimeFrame(BB).ASAP - getTimeFrame(SrcBB).ASAP
           + DstOffset - SrcOffset == 0 && "InterBB dependency not preserved!");
    unsigned NonShortestPathDistance = getTimeFrame(BB).ALAP -
      getTimeFrame(SrcBB).ALAP;
    if (NonShortestPathDistance + DstOffset - SrcOffset > 0) {
      Scheduler.addDifferentialConstraint(Dst, Src, GE, 1);
      // Dst->addDep(Src, VASTDep::CreateCtrlDep(1));
      AnlyHazard |= true;
    }
  }

  return AnlyHazard;
}

bool SDCScheduler::resolveControlChainingHazard() {
  ControlChainingHazardResolver CCHR(*this, DT);
  CCHR.initDistances();
  return CCHR.resolveHazard();
}
