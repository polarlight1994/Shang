//===--------- SIRIMSScheduler.h ------- IMSScheduler -----------*- C++ -*-===//
//
//                         The SIR HLS framework                             //
//
//This file is distributed under the University of Illinois Open Source
//License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//This file define the scheduler based on the Iterative Modulo Schedule
//algorithm which is targeting on the pipelining the loop.
//
//===----------------------------------------------------------------------===//

#ifndef SIR_IMS_SCHEDULER_H
#define SIR_IMS_SCHEDULER_H

#include "llvm/Analysis/LoopInfo.h"

#include "sir/SIRBuild.h"
#include "SIRSchedulerBase.h"

namespace llvm {
class SIRIMSScheduler : public SIRScheduleBase {
public:
  enum Result {
    Success,
    MIITooSmall,
    Fail
  };

private:
  SIR *SM;
  // SIRCtrlRgnBuilder to re-build STM according to the
  // IMS schedule result.
  SIRCtrlRgnBuilder C_Builder;

  // The minimal initial interval.
  unsigned MII;
  // The number of Stages in steady state.
  unsigned StageNum;
  // The BasicBlock contained in the loop.
  BasicBlock *LoopBB;
  // Origin EntrySlot/ExitSlot.
  SIRSlot *OriginEntrySlot, *OriginExitSlot;
  // The SUnits in BasicBlock.
  SmallVector<SIRSchedUnit *, 4> SUnits;
  // The Loop SUnits in BasicBlock.
  SmallVector<SIRSchedUnit *, 4> LoopSUs;
  // The PHI SUnits in BasicBlocak.
  SmallVector<SIRSchedUnit *, 4> PHINodes;
  // The Slots created for Prologue.
  SmallVector<SIRSlot *, 4> PrologueSlots;
  // The Slots created for SteadyState.
  SmallVector<SIRSlot *, 4> SteadyStateSlots;
  // The Slots created for Epilogue.
  SmallVector<SIRSlot *, 4> EpilogueSlots;

  // Mapping between the SUnit and step & stage scheduled.
  std::map<SIRSchedUnit *, std::pair<unsigned, unsigned> > ScheduleResult;
  // Mapping between the SUnit and corresponding illegal slots.
  std::map<SIRSchedUnit *, std::set<unsigned> > IllegalSlots;

  typedef PriorityQueue<SIRSchedUnit *, std::vector<SIRSchedUnit *>,
                        PriorityHeuristic> SUQueue;
  SUQueue ReadyQueue;

public:
  SIRIMSScheduler(SIR *SM, DataLayout *TD, SIRSchedGraph &G, BasicBlock *LoopBB)
    : SIRScheduleBase(G, 0), LoopBB(LoopBB), MII(0), StageNum(0),
    SM(SM), C_Builder(SM, *TD), ReadyQueue(PriorityHeuristic(*this)) {}

  SIRSchedGraph &operator*() const { return G; }
  SIRSchedGraph *operator->() const { return &G; }

  unsigned calculateASAP(const SIRSchedUnit *A) const;
  unsigned calculateALAP(const SIRSchedUnit *A) const;
  bool buildASAPStep();
  bool buildALAPStep();

  void resetTimeFrame();
  void buildTimeFrame();

  typedef SmallVector<SIRSchedUnit *, 4>::iterator iterator;
  typedef SmallVector<SIRSchedUnit *, 4>::reverse_iterator reverse_iterator;

  iterator begin() { return SUnits.begin(); }
  iterator end() { return SUnits.end(); }
  reverse_iterator rbegin() { return SUnits.rbegin(); }
  reverse_iterator rend() { return SUnits.rend(); }

  // Compute the MII constrained by Recurrence.
  unsigned computeRecMII();
  // Compute the MII constrained by Resource.
  unsigned computeResMII();
  // Compute the final MII.
  unsigned computeMII() {
    MII = std::max(computeRecMII(), computeResMII());
    return MII;
  }

  unsigned getMII() const { return MII; }
  void setMII(unsigned II) { MII = II; }
  void increaseMII() { ++MII; }
  void decreaseMII() { --MII; }

  // Set/Detect a schedule is illegal for SUnit U.
  void setStepIllegal(SIRSchedUnit *U, unsigned step);
  bool isStepIllegal(SIRSchedUnit *U, unsigned Schedule);

  // Try to schedule SU to schedule considering all constraints
  // like resource constraints etc.
  bool tryToScheduleSUTo(SIRSchedUnit *U, unsigned schedule);

  // Get the conflict SUnits in schedule with U.
  ArrayRef<SIRSchedUnit *> getConflictSUnit(SIRSchedUnit *U, unsigned schedule);

  // Schedule the SUnit.
  bool scheduleSUTo(SIRSchedUnit *U, unsigned Step);
  // UnSchedule the SUnit.
  void unscheduleSU(SIRSchedUnit *U);

  // Get the schedule result.
  unsigned getStep(SIRSchedUnit *U) {
    assert(U->isScheduled() && "Unexpected un-scheduled SUnit!");

    return ScheduleResult[U].first;
  }
  unsigned getStage(SIRSchedUnit *U) {
    assert(U->isScheduled() && "Unexpected un-scheduled SUnit!");

    return ScheduleResult[U].second;
  }

  // Collect basic information.
  void collectBasicInfo();

  bool scheduleLoopSUs();
  bool schedulePHINodes();
  Result schedule();

  SIRSlot *createSlotForPrologue(unsigned Step);
  SIRSlot *createSlotForSteadyState(unsigned Step);
  SIRSlot *createSlotForEpilogue(unsigned Step);

  void emitSUnitsToSlot();
  void rebuildSTM();

  // Generate the Prologue/Epilogue.
  void generatePrologue();
  void generateEpilogue();

  bool verifySchedule();

  void emitSchedule();
};
}


#endif