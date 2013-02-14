//===-------- ScheduleEmitter.h - Emit the Schedule -------------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the Schedule emitter, which reimplement the
// state-transition graph according to the scheduling results. It also re-time
// the data-path if necessary.
//
//===----------------------------------------------------------------------===//
//

#ifndef SHANG_SCHEDULE_EMITTER
#define SHANG_SCHEDULE_EMITTER
#include "shang/VASTNodeBases.h"

namespace llvm {
class BasicBlock;

class ScheduleEmitterImpl;
class VASTModule;
class VASTSlot;

class ScheduleEmitter {
  ScheduleEmitterImpl *Impl;
public:
  explicit ScheduleEmitter(VASTModule &VM);
  ~ScheduleEmitter();

  void addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd,
                   BasicBlock *DstBB = 0);

  VASTSeqOp *emitToSlot(VASTSeqOp *Op, VASTValPtr Pred, VASTSlot *ToSlot);

  void takeOldSlots();
};

}

#endif
