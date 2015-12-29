// ===------- SIRIMSScheduler.cpp ------- IMSScheduler ----------*- C++ -*-===//
// 
//                          The SIR HLS framework                            //
// 
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
// 
// ===----------------------------------------------------------------------===//
// 
// This file implement the scheduler based on the Iterative Modulo Schedule
// algorithm which is targeting on the pipelining the loop.
// 
// ===----------------------------------------------------------------------===//

#include "sir/SIRIMSScheduler.h"

#include "vast/LuaI.h"

using namespace llvm;
using namespace vast;

static const unsigned MaxSlot = UINT16_MAX >> 2;

float SIRIMSScheduler::calculateASAP(const SIRSchedUnit *A) const {
  float NewStep = 0;

  // All BBEntry should be schedule to 0.
  if (A->isBBEntry())
    return 0;

  typedef SIRSchedUnit::const_dep_iterator iterator;
  for (iterator DI = A->dep_begin(), DE = A->dep_end(); DI != DE; ++DI) {
    const SIRSchedUnit *Dep = *DI;

    // Ignore the back-edge.
    if (Dep->getIdx() >= A->getIdx())
      continue;

    // Ignore the dependency across the BB.
    if (Dep->getParentBB() != LoopBB)
      continue;

    float DepASAP = Dep->isScheduled() ? Dep->getSchedule() : getASAPStep(Dep);
    float DepLatency = Dep->getLatency();

    // Get the correct latency in MII.
    float Step = DepASAP + DepLatency + DI.getLatency(MII);
    assert(Step >= 0.0 && "Unexpected Negative Schedule!");

    NewStep = std::max(Step, NewStep);
  }

  return NewStep;
}

float SIRIMSScheduler::calculateALAP(const SIRSchedUnit *A) const {
  float NewStep = CriticalPathEnd;

  // All loop SUnit should be schedule to MII.
  if (A == G.getLoopSU(LoopBB))
    return CriticalPathEnd;

  typedef SIRSchedUnit::const_use_iterator iterator;
  for (iterator UI = A->use_begin(), UE = A->use_end(); UI != UE; ++UI) {
    SIRSchedUnit *Use = *UI;

    // Ignore the back-edge.
    if (Use->getIdx() <= A->getIdx())
      continue;

    // Ignore the dependency across the BB.
    if (Use->getParentBB() != LoopBB)
      continue;

    SIRDep UseEdge = Use->getEdgeFrom(A);

    float UseALAP = Use->isScheduled() ? Use->getSchedule() : getALAPStep(Use);
    float ALatency = A->getLatency();

    // Get the correct latency in MII.
    float Step = UseALAP - ALatency - UseEdge.getLatency(MII);
    NewStep = std::min(Step, NewStep);
  }

  return NewStep;
}

bool SIRIMSScheduler::buildASAPStep() {
  bool NeedToReCalc = true;
  unsigned NumCalcTimes = 0;
  // The size of graph should be counted as the
  // size of SUnitList of the loop BB.
  const unsigned GraphSize = SUnits.size();

  // Build the time frame iteratively.
  while(NeedToReCalc) {
    NeedToReCalc = false;
    for (iterator I = begin(), E = end(); I != E; ++I) {
      SIRSchedUnit *U = *I;

      // If it is already scheduled, get the result as time frame.
      if (U->isScheduled()) {
        SUnitToTF[U].ASAP = U->getSchedule();
        continue;
      }

      // Calculate the ASAP step.
      float NewSchedule = calculateASAP(U);

      // Get the max ASAP as the CriticalPathEnd.
      CriticalPathEnd = std::max(CriticalPathEnd, NewSchedule);

      // Update the ASAP step.
      float &ASAPSchedule = SUnitToTF[U].ASAP;
      if (ASAPSchedule == NewSchedule) continue;
      ASAPSchedule = NewSchedule;

      // If NeedToReCalc is true, then we need to re-calculate
      // whole graph later. "continue" here is to avoid determining
      // NeedToReCalc anymore to save time since once it is true,
      // it will always be true in this loop.
      if (NeedToReCalc) continue;

      // We need to re-calculate the ASAP steps if the sink
      // of the back-edges need to be update.
      typedef SIRSchedUnit::use_iterator use_iterator;
      for (use_iterator UI = U->use_begin(), UE = U->use_end(); UI != UE; ++UI) {
        SIRSchedUnit *Use = *UI;

        if (Use->getParentBB() != LoopBB)
          continue;

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

  return false;
}

bool SIRIMSScheduler::buildALAPStep() {
  bool NeedToReCalc = true;

  // Build the time frame iteratively.
  while(NeedToReCalc) {
    NeedToReCalc = false;
    for (reverse_iterator I = rbegin(), E = rend(); I != E; ++I) {
      SIRSchedUnit *U = *I;

      // If it is already scheduled, get the result as time frame.
      if (U->isScheduled()) {
        SUnitToTF[U].ALAP = U->getSchedule();
        continue;
      }

      // Calculate the ALAP step.
      float NewSchedule = calculateALAP(U);

      // Update the ALAP step.
      float &ALAPSchedule = SUnitToTF[U].ALAP;
      if (ALAPSchedule == NewSchedule) continue;

      // Here we should make sure the ASAP is smaller than the ALAP.
      assert(int(getASAPStep(U)) <= int(NewSchedule) && "Broken ALAP schedule!");

      ALAPSchedule = NewSchedule;

      // We need to re-calculate the ALAP steps if the source
      // of the edges need to be update.
      typedef SIRSchedUnit::dep_iterator dep_iterator;
      for (dep_iterator DI = U->dep_begin(), DE = U->dep_end(); DI != DE; ++DI) {
        SIRSchedUnit *Dep = *DI;

        if (Dep->getParentBB() != LoopBB)
          continue;

        NeedToReCalc |= (U->getIdx() < Dep->getIdx())
          && (calculateALAP(Dep) != getALAPStep(Dep));
      }
    }
  }

  return false;
}

void SIRIMSScheduler::resetTimeFrame() {
  // Reset the time frames to [0, MaxSlot];
  for (iterator I = begin(), E = end(); I != E; ++I)
    SUnitToTF[*I] = TimeFrame(0, MaxSlot);
}

void SIRIMSScheduler::buildTimeFrame() {
  resetTimeFrame();

  // Build the time frames, to be noted that we are only use
  // the ASAP and ALAP to get time frame for each SUnit not
  // scheduling them really.
  bool HasNegativeCycle = buildASAPStep();
  assert(!HasNegativeCycle && "Unexpected negative cycle!");
  buildALAPStep();
}

unsigned SIRIMSScheduler::computeRecMII() {
  int RecMII = 0;

  /// Calculate the RecMII according to the data dependency edge loop
  /// using formula: Latency/Distance. To be noted that, all data
  /// dependency edge loop will occurred will contained the PHI node.

  // First we collect all the PHI nodes in loop BB.
  for (iterator I = begin(), E = end(); I != E; ++I) {
    SIRSchedUnit *SU = *I;

    if (!SU->isPHI()) continue;

    PHINodes.push_back(SU);
  }

  for (iterator I = PHINodes.begin(), E = PHINodes.end(); I != E; ++I) {
    SIRSchedUnit *PHINode = *I;

    typedef SIRSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator DI = PHINode->dep_begin(), DE = PHINode->dep_end(); DI != DE; ++DI) {
      SIRSchedUnit *DepSU = *DI;

      if (DepSU->isPHI() && DepSU->getParentBB() == LoopBB && DI.getDistance(MII) == 1)
        RecMII = std::max(RecMII, int(ceil(DI.getLatency(MII))));
    }
  }

  return RecMII;
}

unsigned SIRIMSScheduler::computeResMII() {
  return 1;
}

bool SIRIMSScheduler::scheduleSUTo(SIRSchedUnit *U, unsigned schedule) {
  assert(MII != 0 && "Unexpected Zero MII!");

  // Calculate the corresponding step and stage.
  int Step = int(schedule - EntrySchedule) % MII;
  int Stage = (schedule - EntrySchedule) / MII;

  assert(Step >= 0 && Stage >= 0 && "Unexpected Negative Result!");
  assert(!ScheduleResult.count(U) && "Unexpected existed step!");

  // Get the StageNum. To be noted that, the Stage starts from 0,
  // so we need to add it by one.
  StageNum = std::max(int(StageNum), Stage + 1);

  bool Result = ScheduleResult.insert(std::make_pair(U, std::make_pair(Step, Stage))).second;

  if (Result)   
    // To be noted that, this schedule result is temporary and will be
    // reseted after IMS result.
    U->scheduleTo(schedule);

  return Result;
}

void SIRIMSScheduler::unscheduleSU(SIRSchedUnit *U) {
  assert(ScheduleResult.count(U) && "Unexpected not-existed SUnit!");

  // Find the SUnit and erase it.
  std::map<SIRSchedUnit *, std::pair<unsigned, unsigned> >::iterator
    at = ScheduleResult.find(U);
  ScheduleResult.erase(at);
}

bool SIRIMSScheduler::verifySchedule() {
  for (iterator I = begin(), E = end(); I != E; ++I) {
    SIRSchedUnit *SU = *I;

    unsigned Stage = getStage(SU);
    unsigned Step = getStep(SU);

    typedef SIRSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator DI = SU->dep_begin(), DE = SU->dep_end(); DI != DE; ++DI) {
      SIRSchedUnit *DepSU = *DI;

      // Only handle the back-edge.
      if (DepSU->getParentBB() != LoopBB || DepSU->getIdx() < SU->getIdx()
          || DI.getEdgeType(MII) != SIRDep::ValDep)
        continue;

      unsigned DepStage = getStage(DepSU);
      unsigned DepStep = getStep(DepSU);

      unsigned CurrentDistance = DepStage - Stage;
      unsigned TargetDistance = DI.getDistance();

      unsigned DistanceDiff = TargetDistance - CurrentDistance;
      
      int CurrentLatency = DistanceDiff * MII + Step - DepStep;
      int TargetLatency = DI.getLatency(MII);

      if (CurrentLatency < TargetLatency)
        return false;
    }
  }

  return true;
}

namespace {
bool SUnitLess(SIRSchedUnit *LHS, SIRSchedUnit *RHS) {
  return LHS->getIdx() < RHS->getIdx();
}
}

void SIRIMSScheduler::collectBasicInfo() {
  // First collect the EntrySlot and ExitSlot of LoopBB.
  this->OriginEntrySlot = SM->getLandingSlot(LoopBB);
  this->OriginExitSlot = SM->getLatestSlot(LoopBB);

  // Then collect all SUnit in LoopBB in order.
  ArrayRef<SIRSchedUnit *> SUs = G.getSUsInBB(LoopBB);
  for (int i = 0; i < SUs.size(); ++i)
    SUnits.push_back(SUs[i]);

  std::sort(SUnits.begin(), SUnits.end(), SUnitLess);
}

bool SIRIMSScheduler::couldBePipelined() {
  // If the MII is equal to CriticalPathEnd, then it means the
  // loop pipeline is not able to be implemented.
  if (MII > CriticalPathEnd)
    return false;

  // If the loop condition cannot be evaluated in less than II
  // slots, then we cannot decide whether we can initial a new
  // iteration.
  SIRSchedUnit *LoopSU = G.getLoopSU(LoopBB);
  if (getASAPStep(LoopSU) > MII)
    return false;

  return true;
}

SIRIMSScheduler::Result SIRIMSScheduler::schedule() {
  // Collect basic info of this loop BB.
  collectBasicInfo();

  // Calculate the MII according to the Rec&Res.
  computeMII();

  // Build TimeFrame for the SUnits in this local BB.
  buildTimeFrame();

  if (!couldBePipelined())
    return Fail;

  for (iterator I = begin(), E = end(); I != E; ++I) {
    SIRSchedUnit *SU = *I;

    // Other SUnits will be pushed into ReadyQueue to prepare to be
    // scheduled.
    ReadyQueue.push(SU);
  }

  while (!ReadyQueue.empty()) {
    SIRSchedUnit *SU = ReadyQueue.top();
    ReadyQueue.pop();

    unsigned EarliestResult = 0;
    for (unsigned i = unsigned(floor(getASAPStep(SU))),
                  e = unsigned(floor(getALAPStep(SU))); i <= e; i += 1) {
      scheduleSUTo(SU, i);

      break;
    }

    // Otherwise we cannot schedule SU because of other constraints.
    if (!SU->isScheduled()) {
      return Fail;
    }

    // Rebuild the TimeFrame and the ReadyQueue.
    buildTimeFrame();
    ReadyQueue.reheapify();
  }

//   if (!verifySchedule())
//     return Fail;

  emitSchedule();

  /// Debug code
  std::string IMSResult = LuaI::GetString("IMSResult");
  std::string Error;
  raw_fd_ostream Output(IMSResult.c_str(), Error);

  Output << "MII is set to " << MII << "\n";

  for (iterator I = begin(), E = end(); I != E; ++I) {
    SIRSchedUnit *U = *I;

    Output << "SU#" << U->getIdx() << " is scheduled to "
      << "#" << U->getSchedule() << " and step " << getStep(U) << "\n";
  }

  return Success;
}

void SIRIMSScheduler::emitSchedule() {
  // To be noted that, we are not really set the Schedule of the
  // SIRSchedUnit but add a new constraint to this SUnit so that
  // the SDC scheduler will give the right result we want.
  ArrayRef<SIRSchedUnit *> SUs = G.getSUsInBB(LoopBB);

  // Get the Entry SUnit of this BB.
  SIRSchedUnit *EntrySU = G.getSUsInBB(LoopBB).front();
  assert(EntrySU->isBBEntry() && "Unexpected SUnit Type!");

  typedef ArrayRef<SIRSchedUnit *>::iterator iterator;
  for (iterator I = SUs.begin(), E = SUs.end(); I != E; ++I) {
    SIRSchedUnit *U = *I;

    if (U->isBBEntry()) continue;

    typedef SIRSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator DI = U->dep_begin(), DE = U->dep_end(); DI != DE;) {
      SIRSchedUnit *DepU = *DI;
      float OriginLatency = DI->getLatency();

      // Move forward here since we will remove dependency later
      // which may affect the iterator.
      DI++;

      // Remove all dependencies inside of the BB, then we can
      // add a new constraint.
      if (DepU->getParentBB() == U->getParentBB())
        continue;

      // If the dependency is coming from outside of the BB, we can
      // transform it into a dependency between Src SUnit to the BBEntry
      // so that origin dependency will not affect the step result.
      float NewLatency = OriginLatency - getASAPStep(U);
      NewLatency = NewLatency > 0 ? NewLatency : 0;
      U->removeDep(DepU);
      EntrySU->addDep(DepU, SIRDep::CreateCtrlDep(NewLatency));
    }
  }

  // Also constraint the LoopSUnit to locate before the Exit.
  SIRSchedUnit *LoopSU = G.getLoopSU(LoopBB);
  SIRSchedUnit *Exit = G.getExit();

  Exit->addDep(LoopSU, SIRDep::CreateCtrlDep(0));
}