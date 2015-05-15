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

#define DEBUG_TYPE "sir-list-scheduler"
#include "llvm/Support/Debug.h"

namespace llvm {
struct PriorityHeuristic {
	typedef SIRScheduleBase::TimeFrame TimeFrame;
	mutable std::map<const SIRSchedUnit*, TimeFrame> TFCache;
	const SIRScheduleBase &S;

	PriorityHeuristic(const SIRScheduleBase &S) : S(S) {}

	TimeFrame calculateTimeFrame(const SIRSchedUnit *SU) const {
		TimeFrame &TF = TFCache[SU];
		if (!TF)
			TF = S.calculateTimeFrame(SU);

		return TF;
	};

	bool operator()(const SIRSchedUnit *LHS, const SIRSchedUnit *RHS) const {
		// we consider the priority from these aspects:
		// Size Of TF, ALAP, ASAP

		TimeFrame LHSTF = calculateTimeFrame(LHS),
			        RHSTF = calculateTimeFrame(RHS);
		if (LHSTF.size() < RHSTF.size()) return true;
		if (LHSTF.size() > RHSTF.size()) return false;

		// Ascending order using ALAP.
		if (LHSTF.ALAP < RHSTF.ALAP) return true;
		if (LHSTF.ALAP > RHSTF.ALAP) return false;

		// Ascending order using ASAP.
		if (LHSTF.ASAP < RHSTF.ASAP) return true;
		if (LHSTF.ASAP > RHSTF.ASAP) return false;

		// Tie breaker: Original topological order.
		return LHS->getIdx() < RHS->getIdx();
	}
};

struct BBContext {
	SIRScheduleBase &S;
	BasicBlock *BB;

	// The first/last step of the current BB.
	unsigned StartSlot, EndSlot;

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