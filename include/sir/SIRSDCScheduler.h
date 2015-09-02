//===--------- SIRSDCScheduler.h ------- SDCScheduler -----------*- C++ -*-===//
//
//                          The SIR HLS framework                             //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the scheduler based on the System of Difference
// Constraints formation.
//
//===----------------------------------------------------------------------===//

#ifndef SIR_SDC_SCHEDULER_H
#define SIR_SDC_SCHEDULER_H

#include "SIRSchedulerBase.h"

namespace llvm {
class SIRSDCScheduler : public SIRScheduleBase {
private:
	// Add constraints.
	void addDependencyConstraints();

public:
	SIRSDCScheduler(SIRSchedGraph &G, unsigned EntrySlot)
		: SIRScheduleBase(G, EntrySlot) {}

	void scheduleBB(BasicBlock *BB);
	bool schedule();
};
}


#endif