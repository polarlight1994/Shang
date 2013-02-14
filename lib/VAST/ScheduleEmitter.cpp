//===------ ScheduleEmitter.cpp - Emit the Schedule -------------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Schedule emitter, which reimplement the
// state-transition graph according to the scheduling results. It also re-time
// the data-path if necessary.
//
//===----------------------------------------------------------------------===//
//

#include "VASTExprBuilder.h"
#include "shang/ScheduleEmitter.h"
#include "shang/VASTModule.h"

#include "llvm/IR/Function.h"
#include "llvm/ADT/STLExtras.h"
#define DEBUG_TYPE "shang-schedule-emitter"
#include "llvm/Support/Debug.h"

using namespace llvm;

ScheduleEmitter::ScheduleEmitter(VASTModule &VM) : VM(VM) {
  Function &F = VM;
  // Initialize the entry slot.
  PredSlots[&F.getEntryBlock()][VM.getStartSlot()]
    = VM.getPort(VASTModule::Start).getValue();
}
