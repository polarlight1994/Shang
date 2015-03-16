//===----- SIRSchedulerBase.cpp - The Base Class of Schedulers --*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the base class of the schedulers.
//
//===----------------------------------------------------------------------===//

#include "sir/SIRSchedulerBase.h"

#define DEBUG_TYPE "shang-sir-scheduler-base"
#include "llvm/Support/Debug.h"

using namespace llvm;
static const unsigned MaxSlot = UINT16_MAX >> 2;

unsigned SIRScheduleBase::calculateASAP(const SIRSchedUnit *A) const {
  unsigned NewStep = 0;
  typedef SIRSchedUnit::const_dep_iterator iterator;
  for (iterator DI = A->dep_begin(), DE = A->dep_end(); DI != DE; ++DI) {
    const SIRSchedUnit *Dep = *DI;

    // Ignore the back-edges when we are not pipelining the BB.
    if (Dep->getIdx() > A->getIdx() && MII == 0) {
      assert((Dep->getIdx() < A->getIdx()
              || DI.getEdgeType(MII) == SIRDep::Generic
              || DI.getEdgeType(MII) == SIRDep::MemDep)
              && "Bad dependencies forms cycles");
      continue;
    }

    unsigned DepASAP = Dep->isScheduled() ? Dep->getSchedule() : getASAPStep(Dep);
    int Step = DepASAP + DI.getLatency(MII);

    unsigned UStep = std::max(0, Step);
    NewStep = std::max(UStep, NewStep);
  }

  return NewStep;
}

unsigned SIRScheduleBase::calculateALAP(const SIRSchedUnit *A) const  {
  unsigned NewStep = MaxSlot;
  typedef SIRSchedUnit::const_use_iterator iterator;
  for (iterator UI = A->use_begin(), UE = A->use_end(); UI != UE; ++UI) {
    SIRSchedUnit *Use = *UI;
    SIRDep UseEdge = Use->getEdgeFrom(A, MII);

    // Ignore the back-edges when we are not pipelining the BB.
    if (Use->getIdx() < A->getIdx() && MII == 0) {
      assert((Use->getIdx() > A->getIdx())
             || UseEdge.getEdgeType() == SIRDep::Generic
             || UseEdge.getEdgeType() == SIRDep::MemDep
             && "Bad dependencies!");
      continue;
    }

    unsigned UseALAP = Use->isScheduled() ?
                       Use->getSchedule() : getALAPStep(Use);
    if (UseALAP == 0) {
      assert(UseEdge.isLoopCarried() && "Broken time frame!");
      UseALAP = MaxSlot;
    }

    unsigned Step = UseALAP - UseEdge.getLatency(MII);
    NewStep = std::min(Step, NewStep);
  }

  return NewStep;
}

unsigned SIRScheduleBase::buildTimeFrameAndResetSchedule(bool reset) {
	if (reset) G.resetSchedule();

	buildTimeFrame();

	return CriticalPathEnd;
}

void SIRScheduleBase::resetTimeFrame() {
  // Reset the time frames
  for (iterator I = begin(), E = end(); I != E; ++I)
    SUnitToTF[I] = TimeFrame(0, MaxSlot);
}

void SIRScheduleBase::buildTimeFrame() {
  SIRSchedUnit *EntryRoot = G.getEntry();
  assert(EntryRoot->isScheduled() && "Entry must be scheduled first!");

  resetTimeFrame();

  // Build the time frames
  bool HasNegativeCycle = buildASAPStep();
  assert(!HasNegativeCycle && "Unexpected negative cycle!");
  buildALAPStep();
}

bool SIRScheduleBase::buildASAPStep() {
  bool NeedToReCalc = true;
  unsigned NumCalcTimes = 0;
  const unsigned GraphSize = G.size();

  // Build the time frame iteratively.
  while(NeedToReCalc) {
    NeedToReCalc = false;
    for (iterator I = begin(), E = end(); I != E; ++I) {
      const SIRSchedUnit *U = &*I;

      if (U->isScheduled()) {
        SUnitToTF[U].ASAP = U->getSchedule();
        continue;
      }

      unsigned NewStep = calculateASAP(U);

      unsigned &ASAPStep = SUnitToTF[U].ASAP;
      if (ASAPStep == NewStep) continue;
      ASAPStep = NewStep;

      if (NeedToReCalc) continue;

      // We need to re-calculate the ASAP steps if the sink
      // of the back-edges need to be update.
      typedef SIRSchedUnit::const_use_iterator use_iterator;
      for (use_iterator UI = U->use_begin(), UE = U->use_end(); UI != UE; ++UI) {
        const SIRSchedUnit *Use = *UI;
        NeedToReCalc |= (Use->getIdx() < U->getIdx())
                         && (calculateASAP(Use) != getASAPStep(Use));
      }
    }

    if (NeedToReCalc) {
      ++NumCalcTimes;
      // Only iterating |V|-1 times, otherwise there is negative cycle.
      if (NumCalcTimes >= GraphSize) return true;
    }
  }

  SIRSchedUnit *Exit = G.getExit();
  unsigned ExitASAP = getASAPStep(Exit);
  CriticalPathEnd = std::max(CriticalPathEnd, ExitASAP);
  return false;
}

bool SIRScheduleBase::buildALAPStep() {
  bool NeedToReCalc = true;
  int LastSlot = CriticalPathEnd;
  SUnitToTF[G.getExit()].ALAP = LastSlot;

  // Build the time frame iteratively.
  while(NeedToReCalc) {
    NeedToReCalc = false;
    for (reverse_iterator I = rbegin(), E = rend(); I != E; ++I) {
      const SIRSchedUnit *U = &*I;

      if (U == G.getExit()) continue;

      if (U->isScheduled()) {
        SUnitToTF[U].ALAP = U->getSchedule();
        continue;
      }

      unsigned NewStep = calculateALAP(U);

      unsigned &ALAPStep = SUnitToTF[U].ALAP;
      if (ALAPStep == NewStep) continue;
      assert(getASAPStep(U) <= NewStep && "Broken ALAP step!");
      ALAPStep = NewStep;

      // We need to re-calculate the ALAP steps if the source
      // of the edges need to be update.
      typedef SIRSchedUnit::const_dep_iterator dep_iterator;
      for (dep_iterator DI = U->dep_begin(), DE = U->dep_end(); DI != DE; ++DI) {
        const SIRSchedUnit *Dep = *DI;
        NeedToReCalc |= U->getIdx() < Dep->getIdx()
                        && calculateALAP(Dep) != getALAPStep(Dep);
      }
    }
  }

  return false;
}

unsigned SIRScheduleBase::computeRecMII(unsigned MinRecMII) {
  unsigned CriticalPathLength = getCriticalPathLength();
  unsigned MaxRecMII = CriticalPathLength;
  unsigned RecMII = 0;

  MinRecMII = std::max(1u, MinRecMII);
  G.resetSchedule();

  // Find the RecMII by binary search algorithm.
  while (MinRecMII <= MaxRecMII) {
    unsigned MidRecMII = MinRecMII + (MaxRecMII - MinRecMII) / 2;

    setMII(MidRecMII);

    resetTimeFrame();
    if (!buildASAPStep()) {
      RecMII = MidRecMII;
      // There is no negative cycles, try to further reduce the MII.
      MaxRecMII = MidRecMII - 1;
    } else
      // Else we need to relax the MII.
      MinRecMII = MidRecMII + 1;
  }

  assert (RecMII && "Negative cycle found even pipeline is disabled!");
  return RecMII;
}

unsigned SIRScheduleBase::computeStepKey(unsigned step) const {
  if (MII != 0) {
    int offset = int(step - EntrySlot) & int(MII);
    // Adjust the offset if necessary.
    if (offset < 0) offset += MII;

    step = EntrySlot + offset;
  }

  return step;
}

bool SIRScheduleBase::allNodesScheduled(const_iterator I, const_iterator E) const {
  while (I != E)
    if (!(I++)->isScheduled()) return false;

  return true;
}

