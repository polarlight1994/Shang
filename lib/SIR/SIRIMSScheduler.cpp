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

unsigned SIRIMSScheduler::calculateASAP(const SIRSchedUnit *A) const {
  unsigned NewStep = 0;

  // All BBEntry should be schedule to 0.
  if (A->isBBEntry())
    return 0;

  typedef SIRSchedUnit::const_dep_iterator iterator;
  for (iterator DI = A->dep_begin(), DE = A->dep_end(); DI != DE; ++DI) {
    const SIRSchedUnit *Dep = *DI;

    // Ignore the back-edge.
    if (Dep->getIdx() >= A->getIdx())
      continue;

    // If the dependency is coming from previous BB, we should transform it
    // into dependency to the Entry of LoopBB.
    if (Dep->getParentBB() != LoopBB) {
      assert(Dep->isPHI() && "All dependencies cross BB should be PHI-Data-Dependency!");

      unsigned Step = DI.getLatency(MII);
      assert(Step >= 0.0 && "Unexpected Negative Schedule!");

      NewStep = std::max(Step, NewStep);

      continue;
    }

    unsigned DepASAP = Dep->isScheduled() ? Dep->getSchedule() : getASAPStep(Dep);
    unsigned DepLatency = Dep->getLatency();

    // Get the correct latency in MII.
    unsigned Step = DepASAP + DepLatency + DI.getLatency(MII);
    assert(Step >= 0.0 && "Unexpected Negative Schedule!");

    NewStep = std::max(Step, NewStep);
  }

  return NewStep;
}

unsigned SIRIMSScheduler::calculateALAP(const SIRSchedUnit *A) const {
  unsigned NewStep = CriticalPathEnd;

  // All loop SUnit should be schedule to MII.
  if (A == G.getLoopSU(LoopBB))
    return CriticalPathEnd;

  typedef SIRSchedUnit::const_use_iterator iterator;
  for (iterator UI = A->use_begin(), UE = A->use_end(); UI != UE; ++UI) {
    SIRSchedUnit *Use = *UI;

    // Ignore the back-edge.
    if (Use->getIdx() <= A->getIdx())
      continue;

    // Ignore the dependency across the BB
    if (Use->getParentBB() != LoopBB)
      continue;

    SIRDep UseEdge = Use->getEdgeFrom(A);

    unsigned UseALAP = Use->isScheduled() ? Use->getSchedule() : getALAPStep(Use);
    unsigned ALatency = A->getLatency();

    // Get the correct latency in MII.
    unsigned Step = UseALAP - ALatency - UseEdge.getLatency(MII);
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
      unsigned NewSchedule = calculateASAP(U);

      // Get the max ASAP as the CriticalPathEnd.
      CriticalPathEnd = std::max(CriticalPathEnd, NewSchedule);

      // Update the ASAP step.
      unsigned &ASAPSchedule = SUnitToTF[U].ASAP;
      if (ASAPSchedule == NewSchedule) continue;
      ASAPSchedule = NewSchedule;

      // If NeedToReCalc is true, then we need to re-calculate
      // whole graph later. "continue" here is to avoid determining
      // NeedToReCalc anymore to save time since once it is true,
      // it will always be true in this loop.
      if (NeedToReCalc) continue;

      // We need to re-calculate the ASAP steps if the sink
      // of the back-edges need to be update.
      typedef SIRSchedUnit::const_use_iterator use_iterator;
      for (use_iterator UI = U->use_begin(), UE = U->use_end(); UI != UE; ++UI) {
        const SIRSchedUnit *Use = *UI;
        // Use this pointer to make sure we call the right version.
        NeedToReCalc |= (Use->getIdx() < U->getIdx())
          && (this->calculateASAP(Use) != getASAPStep(Use));
      }
    }

    if (NeedToReCalc) {
      ++NumCalcTimes;
      // Only iterating |V|-1 times, otherwise there is negative cycle.
      if (NumCalcTimes >= GraphSize) return true;
    }
  }

  // The ASAP of LoopSU should always be set to CriticalPathEnd regardless of the
  // data dependency.
  SIRSchedUnit *LoopSU = G.getLoopSU(LoopBB);
  unsigned &LoopSUASAP = SUnitToTF[LoopSU].ASAP;
  LoopSUASAP = CriticalPathEnd;

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
      unsigned NewSchedule = calculateALAP(U);

      // Update the ALAP step.
      unsigned &ALAPSchedule = SUnitToTF[U].ALAP;
      if (ALAPSchedule == NewSchedule) continue;

      // Here we should make sure the ASAP is smaller than the ALAP.
      assert(getASAPStep(U) <= NewSchedule && "Broken ALAP schedule!");

      ALAPSchedule = NewSchedule;

      // We need to re-calculate the ALAP steps if the source
      // of the edges need to be update.
      typedef SIRSchedUnit::const_dep_iterator dep_iterator;
      for (dep_iterator DI = U->dep_begin(), DE = U->dep_end(); DI != DE; ++DI) {
        const SIRSchedUnit *Dep = *DI;
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
        RecMII = std::max(RecMII, DI.getLatency(MII));
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

bool SIRIMSScheduler::scheduleLoopSUs() {
  SIRSchedUnit *LoopSU = G.getLoopSU(LoopBB);
  // To ensure the LoopSU is scheduled to the end of Steady State,
  // we may need to delay the ASAP result of it. In this process,
  // we will schedule it into the end of origin Stage.
  int OriginResult = getASAPStep(LoopSU);
  int Stage = (OriginResult - EntrySchedule) / MII;
  int ScheduleResult = (Stage + 1) * MII - 1;

  for (iterator I = LoopSUs.begin(), E = LoopSUs.end(); I != E; ++I) {
    SIRSchedUnit *SU = *I;

    assert(getASAPStep(SU) <= OriginResult
           && "All LoopSUs should be scheduled to same slot!");

    scheduleSUTo(SU, ScheduleResult);

    // Update the CriticalPathEnd.
    CriticalPathEnd = std::max(CriticalPathEnd, unsigned(ScheduleResult));
  }

  return true;
}

bool SIRIMSScheduler::schedulePHINodes() {
  for (iterator I = PHINodes.begin(), E = PHINodes.end(); I != E; ++I) {
    SIRSchedUnit *SU = *I;

    assert(getASAPStep(SU) <= CriticalPathEnd
           && "Wrong ASAP Step of PHI Node!");
    
    scheduleSUTo(SU, CriticalPathEnd);
  }

  return true;
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

SIRIMSScheduler::Result SIRIMSScheduler::schedule() {
  // Calculate the MII according to the Rec&Res.
  computeMII();

  // Build TimeFrame for the SUnits in this local BB.
  buildTimeFrame();

  // If the MII is equal to CriticalPathEnd, then it means the
  // loop pipeline is not able to be implemented.
  if (MII > CriticalPathEnd)
    return Fail;

  ArrayRef<SIRSchedUnit *> SUs = G.getSUsInBB(LoopBB);
  for (iterator I = SUs.begin(), E = SUs.end(); I != E; ++I) {
    SIRSchedUnit *SU = *I;

    /// Collect all the LoopSUnits and ignore them since they will
    /// be scheduled to the end of the Steady State regardless of
    /// the dependencies.
    // The SUnit transition back to the EntrySlot of BB is LoopSUnit.
    if (SU == G.getLoopSU(LoopBB)) {
      LoopSUs.push_back(SU);

      continue;
    }
    // The SUnit transition to EntrySlot of successor BB is LoopSUnit
    if (SU->isSlotTransition()) {
      SIRSlotTransition *SST = dyn_cast<SIRSlotTransition>(SU->getSeqOp());
      SIRSlot *SrcSlot = SST->getSrcSlot();
      SIRSlot *DstSlot = SST->getDstSlot();

      if (DstSlot->getParent() != SrcSlot->getParent()) {
        LoopSUs.push_back(SU);

        continue;
      }
    }

    /// Ignore PHINodes since they will be scheduled to the end of the
    /// CriticalPath regardless of the dependencies.
    if (SU->isPHI())
      continue;

    // Other SUnits will be pushed into ReadyQueue to prepare to be
    // scheduled.
    ReadyQueue.push(SU);
  }

  // Schedule all LoopSUnits to the end of the Steady State.
  scheduleLoopSUs();

  // Schedule all PHINodes to the end of CriticalPath.
  schedulePHINodes();

  while (!ReadyQueue.empty()) {
    SIRSchedUnit *SU = ReadyQueue.top();
    ReadyQueue.pop();

    unsigned EarliestResult = 0;
    for (unsigned i = getASAPStep(SU), e = getALAPStep(SU); i <= e; i += 1) {
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

  if (!varifySchedule())
    return Fail;

  emitSchedule();

  /// Debug code
  std::string TimeFrameResult = LuaI::GetString("IMSResult");
  std::string Error;
  raw_fd_ostream Output(TimeFrameResult.c_str(), Error);

  Output << "MII is set to " << MII << "\n";

  for (iterator I = begin(), E = end(); I != E; ++I) {
    SIRSchedUnit *U = *I;     

    Output << "SU#" << U->getIdx() << " is scheduled to "
      << "step " << getStep(U) << "\n";
  }

  return Success;
}

SIRSlot *SIRIMSScheduler::createSlotForPrologue(unsigned Step) {
  SIRSlot *S = C_Builder.createSlot(LoopBB, Step);

  PrologueSlots.push_back(S);

  return S;
}

SIRSlot *SIRIMSScheduler::createSlotForSteadyState(unsigned Step) {
  SIRSlot *S = C_Builder.createSlot(LoopBB, Step);

  SteadyStateSlots.push_back(S);

  return S;
}

SIRSlot *SIRIMSScheduler::createSlotForEpilogue(unsigned Step) {
  SIRSlot *S = C_Builder.createSlot(LoopBB, Step);

  EpilogueSlots.push_back(S);

  return S;
}

void SIRIMSScheduler::rebuildSTM() {
  // Get the EntrySlot/ExitSlot of Prologue.
  SIRSlot *PrologueEntrySlot = PrologueSlots.front();
  SIRSlot *PrologueExitSlot = PrologueSlots.back();

  // Get the EntrySlot/ExitSlot of SteadyState.
  SIRSlot *SteadyStateEntrySlot = SteadyStateSlots.front();
  SIRSlot *SteadyStateExitSlot = SteadyStateSlots.back();

  // Get the EntrySlot/ExitSlot of Epilogue.
  SIRSlot *EpilogueEntrySlot = EpilogueSlots.front();
  SIRSlot *EpilogueExitSlot = EpilogueSlots.back();

  for (unsigned i = 0; i < PrologueSlots.size() - 1; ++i) {
    // Create the slot transition from last slot to this slot.
    SIRSlot *S = PrologueSlots[i];
    SIRSlot *SuccSlot = PrologueSlots[i + 1];

    C_Builder.createStateTransition(S, SuccSlot,
                                    C_Builder.createIntegerValue(1, 1));
  }

  for (unsigned i = 0; i < SteadyStateSlots.size() - 1; ++i) {
    // Create the slot transition from last slot to this slot.
    SIRSlot *S = SteadyStateSlots[i];
    SIRSlot *SuccSlot = SteadyStateSlots[i + 1];

    C_Builder.createStateTransition(S, SuccSlot,
                                    C_Builder.createIntegerValue(1, 1));
  }

  for (unsigned i = 0; i < EpilogueSlots.size() - 1; ++i) {
    // Create the slot transition from last slot to this slot.
    SIRSlot *S = EpilogueSlots[i];
    SIRSlot *SuccSlot = EpilogueSlots[i + 1];

    C_Builder.createStateTransition(S, SuccSlot,
                                    C_Builder.createIntegerValue(1, 1));
  }

  // Create the State Transition from Previous BB to the Prologue of Loop BB by
  // inherit the processors of origin EntrySlot.
  typedef SIRSlot::pred_iterator pred_iterator;
  for (pred_iterator I = OriginEntrySlot->pred_begin(), E = OriginEntrySlot->pred_end();
       I != E;) {
    SIRSlot *Pred = I->getSlot();

    // The edge is coming from current BB that means this is the loop edge.
    // we should handle it specially.
    if (Pred->getParent() == LoopBB) {
      // We should inherit the condition of this loop edge.
      C_Builder.createStateTransition(SteadyStateExitSlot, SteadyStateEntrySlot,
                                      I->getCnd());
      // Increment here to avoid the unlink affects the iterator.
      ++I;
      continue;
    }

    // Link the edge from the Pred to PrologueEntrySlot.
    C_Builder.createStateTransition(Pred, PrologueEntrySlot, I->getCnd());
    // Increment here to avoid the unlink affects the iterator.
    ++I;
    // Unlink the origin edge.
    Pred->unlinkSucc(OriginEntrySlot);
  }

  // Create the State Transition from the Prologue to Steady State.
  C_Builder.createStateTransition(PrologueExitSlot, SteadyStateEntrySlot,
                                  C_Builder.createIntegerValue(1, 1));

  // Create the State Transition from the Steady State to Epilogue of Loop BB by
  // inherit the successors of origin ExitSlot.
  typedef SIRSlot::succ_iterator succ_iterator;
  for (succ_iterator I = OriginExitSlot->succ_begin(), E = OriginExitSlot->succ_end();
       I != E;) {
    SIRSlot *Succ = I->getSlot();

    // Ignore the slot transition inside this Loop BB since this will only
    // happen when we transition from ExitSlot to EntrySlot, which we already
    // handled in previous step.
    if (Succ->getParent() == LoopBB) {
      // Increment here to avoid the unlink affects the iterator.
      ++I;
      continue;
    }

    // Link the edge from the SteadyStateExitSlot to EpilogueEntrySlot.
    C_Builder.createStateTransition(SteadyStateExitSlot, EpilogueEntrySlot, I->getCnd());
    // Also link the edge from the EpilogueExitSLot to Succ.
    C_Builder.createStateTransition(EpilogueExitSlot, Succ, I->getCnd());
    // Increment here to avoid the unlink affects the iterator.
    ++I;
    // Unlink the origin edge.
    OriginExitSlot->unlinkSucc(Succ);
  }

  SM->IndexBB2Slots(LoopBB, PrologueEntrySlot, EpilogueExitSlot);
}

void SIRIMSScheduler::generatePrologue() {
  for (int i = 0; i < StageNum - 1; ++i) {
    for (int j = 0; j < MII; ++j) {
      // Create a new Slot for this step.
      SIRSlot *S = createSlotForPrologue(j);

      // Get all SeqOps scheduled to this step, and emit them to this NewSlot.
      typedef
        std::map<SIRSchedUnit *, std::pair<unsigned, unsigned> >::iterator iterator;
      for (iterator I = ScheduleResult.begin(), E = ScheduleResult.end();
           I != E; ++I) {
        if (I->second.first != j || I->second.second > i)
          continue;

        SIRSchedUnit *SU = I->first;

        // Ignore the BBEntry and SlotTransition SUnits.
        if (SU->isBBEntry() || SU->isSlotTransition()) continue;

        ArrayRef<SIRSeqOp *> SeqOps = SU->getSeqOps();
        for (int k = 0; k < SeqOps.size(); ++k) {
          SIRSeqOp *SeqOp = SeqOps[k];

          C_Builder.assignToReg(S, C_Builder.createIntegerValue(1, 1),
                                SeqOp->getSrc(), SeqOp->getDst());
        }
      }    
    }
  }
}

void SIRIMSScheduler::generateEpilogue() {
  for (int i = StageNum - 1; i > 0; --i) {
    for (int j = 0; j < MII; ++j) {
      // Create a new Slot for this step.
      SIRSlot *S = createSlotForEpilogue(j);

      // Get all SeqOps scheduled to this step, and emit them to this NewSlot.
      typedef
        std::map<SIRSchedUnit *, std::pair<unsigned, unsigned> >::iterator iterator;
      for (iterator I = ScheduleResult.begin(), E = ScheduleResult.end();
           I != E; ++I) {
        if (I->second.first != j || I->second.second < i)
          continue;

        SIRSchedUnit *SU = I->first;

        // Ignore the BBEntry and SlotTransition SUnits.
        if (SU->isBBEntry() || SU->isSlotTransition()) continue;

        ArrayRef<SIRSeqOp *> SeqOps = SU->getSeqOps();
        for (int k = 0; k < SeqOps.size(); ++k) {
          SIRSeqOp *SeqOp = SeqOps[k];

          C_Builder.assignToReg(S, C_Builder.createIntegerValue(1, 1),
                                SeqOp->getSrc(), SeqOp->getDst());
        }
      }
    }
  }
}

void SIRIMSScheduler::emitSUnitsToSlot() {
  for (unsigned i = 0; i < MII; ++i) {
    // Create a new Slot for this step.
    SIRSlot *S = createSlotForSteadyState(i);

    // Get all SeqOps scheduled to this step,
    // and emit them to this NewSlot.
    typedef std::map<SIRSchedUnit *, std::pair<unsigned, unsigned> >::iterator iterator;
    for (iterator I = ScheduleResult.begin(), E = ScheduleResult.end();
         I != E; ++I) {
      if (I->second.first != i)
        continue;

      SIRSchedUnit *SU = I->first;

      // Ignore the BBEntry and SlotTransition SUnits.
      if (SU->isBBEntry() || SU->isSlotTransition()) continue;

      ArrayRef<SIRSeqOp *> SeqOps = SU->getSeqOps();
      for (int i = 0; i < SeqOps.size(); ++i)
        SeqOps[i]->setSlot(S);
    }
  }
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

    // Ignore the BBEntry since it will schedule to step 0 without
    // any dependencies.
    if (U->isBBEntry()) continue;

    typedef SIRSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator I = U->dep_begin(), E = U->dep_end(); I != E;) {
      SIRSchedUnit *DepU = *I;
      int OriginLatency = I->getLatency();

      // Move forward here since we will remove dependency later
      // which may affect the iterator.
      I++;

      // Remove all dependencies inside of the BB, then we can
      // add a new constraint.
      if (DepU->getParentBB() == U->getParentBB()) {
        U->removeDep(DepU);
        continue;
      }

      // If the dependency is coming from outside of the BB, we can
      // transform it into a dependency between Src SUnit to the BBEntry
      // so that origin dependency will not affect the step result.
      unsigned NewLatency = std::max(0, OriginLatency - int(getStep(U)));
      U->removeDep(DepU);
      EntrySU->addDep(DepU, SIRDep::CreateCtrlDep(NewLatency));
    }

    // Add a new constraint.
    U->addDep(EntrySU, SIRDep::CreateCtrlDep(getStep(U)));
  }

  // Also constraint the LoopSUnit to locate before the Exit.
  SIRSchedUnit *LoopSU = G.getLoopSU(LoopBB);
  SIRSchedUnit *Exit = G.getExit();

  Exit->addDep(LoopSU, SIRDep::CreateCtrlDep(0));

  // Emit the SUnits to corresponding Slot.
  emitSUnitsToSlot();

  generatePrologue();
  generateEpilogue();

  // Re-build the STM.
  rebuildSTM();
}