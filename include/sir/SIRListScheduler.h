//===--------- SIRListScheduler.h ------- ListScheduler ---------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declare a heuristic list scheduler which supports global code
// motion.
//
//===----------------------------------------------------------------------===//
#ifndef SIR_LIST_SCHEDULER
#define SIR_LIST_SCHEDULER

#include "SIRSchedulerBase.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/ADT/PriorityQueue.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/CFG.h"

#include "llvm/Support/Debug.h"

namespace llvm {
struct BBContext {
  SIRScheduleBase &S;
  BasicBlock *BB;

  // The first/last schedule of the current BB.
  float StartSchedule, EndSchedule;

  // The SchedUnits that should be schedule to
  // the EntrySlot/ExitSlot of the current BB.
  std::set<SIRSchedUnit *> Entrys, Exits;

  typedef PriorityQueue<SIRSchedUnit *, std::vector<SIRSchedUnit *>,
                        PriorityHeuristic> SUQueue;
  SUQueue ReadyQueue;

  BBContext(SIRScheduleBase &S, BasicBlock *BB)
    : S(S), BB(BB), ReadyQueue(PriorityHeuristic(S)) {}

  bool isSUReady(SIRSchedUnit *SU);

  void collectSUsInEntrySlot(ArrayRef<SIRSchedUnit *> SUs);
  void collectSUsInExitSlot(ArrayRef<SIRSchedUnit *> SUs);

  void collectReadySUs(ArrayRef<SIRSchedUnit *> SUs);

  void scheduleSUsToEntrySlot();
  void scheduleSUsToExitSlot();

  void enter(BasicBlock *BB);
  void exit(BasicBlock *BB);

  void scheduleBB();
};

struct ListScheduler : public SIRScheduleBase {
  ListScheduler(SIRSchedGraph &G, unsigned EntrySlot)
    : SIRScheduleBase(G, EntrySlot) {}

  void scheduleBB(BasicBlock *BB);
  bool schedule();
}; 
}

#endif