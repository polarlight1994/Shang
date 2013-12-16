//===-------- SchedulerBase.cpp - The Base Class of Schedulers --*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the base class of the schedulers.
//
//===----------------------------------------------------------------------===//

#include "SchedulerBase.h"
#include "ScheduleDOT.h"

#define DEBUG_TYPE "shang-scheduler-base"
#include "llvm/Support/Debug.h"

using namespace llvm;
static const unsigned MaxSlot = UINT16_MAX >> 2;

void SchedulerBase::viewGraph() {
  ViewGraph(this, "Scheduler");
}

//===----------------------------------------------------------------------===//
void SchedulerBase::resetTimeFrame() {
  // Reset the time frames
  for (iterator I = begin(), E = end();I != E;++I)
    SUnitToTF[I] = TimeFrame(0, MaxSlot);
}

void SchedulerBase::buildTimeFrame() {
  VASTSchedUnit *EntryRoot = G.getEntry();
  assert(EntryRoot->isScheduled() && "Entry must be scheduled first!");

  resetTimeFrame();
  // Build the time frames
  bool HasNegativeCycle = buildASAPStep();
  assert(!HasNegativeCycle && "Unexpected negative cycle!");
  buildALAPStep();

  DEBUG(dumpTimeFrame());
}

unsigned SchedulerBase::buildTimeFrameAndResetSchedule(bool reset) {
  if (reset) G.resetSchedule();

  buildTimeFrame();

  return CriticalPathEnd;
}

unsigned SchedulerBase::calculateASAP(const VASTSchedUnit *U) {
  unsigned NewStep = 0;
  typedef VASTSchedUnit::const_dep_iterator iterator;
  for (iterator DI = U->dep_begin(), DE = U->dep_end(); DI != DE; ++DI) {
    const VASTSchedUnit *Dep = *DI;
    // Ignore the back-edges when we are not pipelining the BB.
    if (Dep->getIdx() > U->getIdx() && MII == 0) {
      assert((Dep->getIdx() < U->getIdx()
              || DI.getEdgeType(MII) == VASTDep::Conditional
              || DI.getEdgeType(MII) == VASTDep::Synchronize
              || DI.getEdgeType(MII) == VASTDep::Generic
              || DI.getEdgeType(MII) == VASTDep::MemDep)
             && "Bad dependencies!");
      continue;
    }

    unsigned DepASAP = Dep->isScheduled() ? Dep->getSchedule() : getASAPStep(Dep);
    int Step = DepASAP + DI.getLatency(MII);
    DEBUG(dbgs() << "From ";
          if (DI.isLoopCarried()) dbgs() << "BackEdge ";
          Dep->print(dbgs());
          dbgs() << " Step " << Step << '\n');
    unsigned UStep = std::max(0, Step);
    NewStep = std::max(UStep, NewStep);
  }

  return NewStep;
}


bool SchedulerBase::buildASAPStep() {
  bool NeedToReCalc = true;
  unsigned NumCalcTimes = 0;
  const unsigned GraphSize = G.size();

  // Build the time frame iteratively.
  while(NeedToReCalc) {
    NeedToReCalc = false;
    for (iterator I = begin(), E = end(); I != E; ++I) {
      const VASTSchedUnit *U = I;
      if (U->isScheduled()) {
        SUnitToTF[U].ASAP = U->getSchedule();
        continue;
      }

      DEBUG(dbgs() << "\n\nCalculating ASAP step for \n";
            U->dump(););

      unsigned NewStep = calculateASAP(U);

      DEBUG(dbgs() << "Update ASAP step to: " << NewStep << " for \n";
            U->dump();
            dbgs() << "\n\n";);

      unsigned &ASAPStep = SUnitToTF[U].ASAP;
      if (ASAPStep == NewStep) continue;
      ASAPStep = NewStep;

      if (NeedToReCalc) continue;

      // We need to re-calculate the ASAP steps if the sink of the back-edges,
      // need to be update.
      typedef VASTSchedUnit::const_use_iterator dep_iterator;
      for (dep_iterator UI = U->use_begin(), UE = U->use_end(); UI != UE; ++UI) {
        const VASTSchedUnit *Use = *UI;
        NeedToReCalc |= Use->getIdx() < U->getIdx()
                        && calculateASAP(Use) != getASAPStep(Use);
      }
    }

    if (NeedToReCalc) {
      ++NumCalcTimes;
      // Only iterating |V|-1 times, otherwise there is negative cycle.
      if (NumCalcTimes >= GraphSize) return true;
    }
  }

  VASTSchedUnit *Exit = G.getExit();
  unsigned ExitASAP = getASAPStep(Exit);
  CriticalPathEnd = std::max(CriticalPathEnd, ExitASAP);
  return false;
}

unsigned SchedulerBase::computeRecMII(unsigned MinRecMII) {
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

  assert(RecMII && "Negative cycle found even pipeline is disabled!");

  return RecMII;
}


unsigned SchedulerBase::calculateALAP(const VASTSchedUnit *A) {
  unsigned NewStep = MaxSlot;
  typedef VASTSchedUnit::const_use_iterator iterator;
  for (iterator UI = A->use_begin(), UE = A->use_end(); UI != UE; ++UI) {
    VASTSchedUnit *Use = *UI;
    VASTDep UseEdge = Use->getEdgeFrom(A, MII);

    // Ignore the back-edges when we are not pipelining the BB.
    if (A->getIdx() > Use->getIdx() && MII == 0) {
      assert((A->getIdx() < Use->getIdx()
              || UseEdge.getEdgeType() == VASTDep::Conditional
              || UseEdge.getEdgeType() == VASTDep::Synchronize
              || UseEdge.getEdgeType() == VASTDep::Generic
              || UseEdge.getEdgeType() == VASTDep::MemDep)
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
    DEBUG(dbgs() << "From ";
          if (UseEdge.isLoopCarried()) dbgs() << "BackEdge ";
          Use->print(dbgs());
          dbgs() << " Step " << Step << '\n');
    NewStep = std::min(Step, NewStep);
  }

  return NewStep;
}


void SchedulerBase::buildALAPStep() {
  const VASTSchedUnit *Exit = G.getExit();
  int LastSlot = CriticalPathEnd;
  SUnitToTF[Exit].ALAP = LastSlot;

  bool NeedToReCalc = true;
  // Build the time frame iteratively.
  while (NeedToReCalc) {
    NeedToReCalc = false;
    typedef VASTSchedGraph::const_reverse_iterator iterator;
    for (iterator I = G.rbegin(), E = G.rend(); I != E; ++I) {
      const VASTSchedUnit *A = &*I;

      if (A == Exit) continue;

      if (A->isScheduled()) {
        SUnitToTF[A].ALAP = A->getSchedule();
        continue;
      }

      DEBUG(dbgs() << "\n\nCalculating ALAP step for \n";
            A->dump(););

      unsigned NewStep = calculateALAP(A);

      DEBUG(dbgs() << "Update ALAP step to: " << NewStep << " for \n";
            A->dump();
            dbgs() << "\n\n";);

      unsigned &ALAPStep = SUnitToTF[A].ALAP;
      if (ALAPStep == NewStep) continue;
      assert(getASAPStep(A) <= NewStep && "Broken ALAP step!");
      ALAPStep = NewStep;

      typedef VASTSchedUnit::const_dep_iterator dep_iterator;
      for (dep_iterator DI = A->dep_begin(), DE = A->dep_end(); DI != DE; ++DI) {
        const VASTSchedUnit *Dep = *DI;
        NeedToReCalc |= A->getIdx() < Dep->getIdx()
                        && calculateALAP(Dep) != getALAPStep(Dep);
      }
    }
  }
}


void SchedulerBase::printSUTimeFrame(raw_ostream &OS,
                                     const VASTSchedUnit *A) const {
  A->print(OS);
  OS << " : {" << getASAPStep(A) << "," << getALAPStep(A)
    << "} " <<  getTimeFrame(A);

  typedef VASTSchedUnit::const_dep_iterator dep_iterator;
  for (dep_iterator DI = A->dep_begin(), DE = A->dep_end(); DI != DE; ++DI)
    OS << " [" << DI->getIdx() << '@' << DI->getSchedule()
       << '~' << DI.getLatency() << ']';

  OS << " | ";

  typedef VASTSchedUnit::const_use_iterator use_iterator;
  for (use_iterator UI = A->use_begin(), UE = A->use_end(); UI != UE; ++UI)
    OS << " [" << (*UI)->getIdx() << '@' << (*UI)->getSchedule()
       << '~' << (*UI)->getEdgeFrom(A).getLatency() <<  ']';

  OS << '\n';
}


void SchedulerBase::dumpTimeFrame() const {
  printTimeFrame(dbgs());
}

unsigned SchedulerBase::computeStepKey(unsigned step) const {
  if (MII != 0) {
    int offset = int(step - EntrySlot) % int(MII);
    // Wrap the offset if necessary.
    if (offset < 0) offset = MII + offset;

    step = EntrySlot + offset;
  }

  return step;
}

bool SchedulerBase::allNodesSchedued(const_iterator I, const_iterator E) const {
  while (I != E)
    if (!(I++)->isScheduled()) return false;

  return true;
}

bool SchedulerBase::scheduleCriticalPath(iterator I, iterator E) {
  llvm_unreachable("Not implement yet!");
  while (I != E) {
    VASTSchedUnit *A = I++;

    if (A->isScheduled() || getTimeFrame(A) != 1)
      continue;

    unsigned step = getASAPStep(A);
    // if (!tryTakeResAtStep(A, step)) return false;
    DEBUG(A->print(dbgs()));
    DEBUG(dbgs() << " asap step: " << step << " in critical path.\n");
    A->scheduleTo(step);
  }

  return true;
}
