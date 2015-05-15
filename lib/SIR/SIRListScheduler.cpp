//===------- SIRListScheduler.cpp ------- ListScheduler ---------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement a heuristic list scheduler which supports global code
// motion.
//
//===----------------------------------------------------------------------===//
#include "sir/SIRListScheduler.h"

using namespace llvm;

void BBContext::enter(BasicBlock *BB) {
	// Initialize
	StartSlot = S.EntrySlot;
	EndSlot = S.MaxSlot;
	this->BB = BB;

	ArrayRef<SIRSchedUnit *> SUs(S->getSUsInBB(BB));

	collectSUsInEntrySlot(SUs);
	collectSUsInExitSlot(SUs);

	collectReadySUs(SUs);
}

void BBContext::exit(BasicBlock *BB) {
	// Make sure all the SUnits is scheduled.
	ArrayRef<SIRSchedUnit *> SUs(S->getSUsInBB(BB));
	typedef ArrayRef<SIRSchedUnit *>::iterator iterator;
	for (iterator I = SUs.begin(), E = SUs.end(); I != E; ++I) {
		SIRSchedUnit *SU = *I;
		assert(SU->isScheduled() && "All SUnits should be scheduled now!");
	}

	BB = NULL;
	Entrys.clear();
	Exits.clear();
}

void BBContext::collectSUsInEntrySlot(ArrayRef<SIRSchedUnit *> SUs) {
  // The SUs that corresponds to the entry of BB are entry nodes.
	typedef ArrayRef<SIRSchedUnit *>::iterator iterator;
	for (iterator I = SUs.begin(), E = SUs.end(); I != E; ++I) {
		SIRSchedUnit *SU = *I;
		if (SU->isBBEntry())
			Entrys.insert(SU);
	}
}

void BBContext::collectSUsInExitSlot(ArrayRef<SIRSchedUnit *> SUs) {
  // The SUs that corresponds to the terminator of BB are exit nodes.
  ArrayRef<SIRSchedUnit *> SUnits = S->lookupSUs(BB->getTerminator());

  Exits.insert(SUnits.begin(), SUnits.end());

  // PHINodes also need to schedule to the end of the BB.
	typedef ArrayRef<SIRSchedUnit *>::iterator iterator;
	for (iterator I = SUs.begin(), E = SUs.end(); I != E; I++) {
		SIRSchedUnit *SU = *I;
		if (SU->isPHI())
			Exits.insert(SU);
	}
}

bool BBContext::isSUReady(SIRSchedUnit *SU) {
	// A SU is ready only when all SUs it depends on is scheduled.
	bool AllScheduled = true;

	typedef SIRSchedUnit::dep_iterator iterator;
	for (iterator DI = SU->dep_begin(), DE = SU->dep_end(); DI != DE; ++DI) {
		SIRSchedUnit *DepSU = *DI;

// 		// Ignore the dependency cross the BB.
// 		if (DepSU->getParentBB() != SU->getParentBB())
// 			continue;

		AllScheduled &= DepSU->isScheduled();
	}

	// Hack: why BBEntry and PHI only need one of dependencies is ready.
	// 	if (SU->isBBEntry() || SU->isPHI())
	// 		return AnyScheduled;

	return AllScheduled;
}

void BBContext::collectReadySUs(ArrayRef<SIRSchedUnit *> SUs) {
	typedef ArrayRef<SIRSchedUnit *>::iterator iterator;
	for (iterator I = SUs.begin(), E = SUs.end(); I != E; ++I) {
		SIRSchedUnit *SU = *I;

		// If this SUnit is in Slot0r, then we do not need to add
		// it into the ReadySUs, since it must be scheduled to 0
		// without any doubt.
		if (SU->isInSlot0r())
			continue;

		// SUnit should not be scheduled unless the SUnit is
		// Entry, Exit or SUnit created for the Slot0r.
		assert((!SU->isScheduled() || SU->isInSlot0r())
			      && "SUnit should not be scheduled yet!");

		// Entrys, Exits and PHINodes will be handled elsewhere.
		if (SU->isBBEntry() || SU->isExit() || SU->isPHI())
			continue;

		if (isSUReady(SU) && SU->getParentBB() == BB)
			ReadyQueue.push(SU);
	}
}

void BBContext::scheduleBB() {
	enter(BB);

	scheduleSUsToEntrySlot();

	while (!ReadyQueue.empty()) {
		SIRSchedUnit *SU = ReadyQueue.top();
		ReadyQueue.pop();

		unsigned Step = std::max(S.calculateASAP(SU), StartSlot);

		SU->scheduleTo(Step);

		// After we schedule a unit, we should reset the ReadyQueue.
		S.resetTimeFrame();

		ArrayRef<SIRSchedUnit *> Users = SU->getUseList();

		collectReadySUs(Users);
		ReadyQueue.reheapify();
	}

	scheduleSUsToExitSlot();

	exit(BB);
}

void BBContext::scheduleSUsToEntrySlot() {
  // Schedule the entry and all PHIs to the same slot.
  typedef std::set<SIRSchedUnit *>::iterator iterator;
  for (iterator I = Entrys.begin(), E = Entrys.end(); I != E; ++I) {
    SIRSchedUnit *SU = *I;    
    StartSlot = std::max(StartSlot, S.calculateASAP(SU));
  }

  for (iterator I = Entrys.begin(), E = Entrys.end(); I != E; ++I) {
    SIRSchedUnit *SU = *I;
		SU->scheduleTo(StartSlot);
  }
}

void BBContext::scheduleSUsToExitSlot() {
  // Schedule the exit to the exit slot.
  typedef std::set<SIRSchedUnit *>::iterator iterator;
  for (iterator I = Exits.begin(), E = Exits.end(); I != E; ++I) {
		SIRSchedUnit *SU = *I;
		EndSlot = std::max(StartSlot + 1, S.calculateASAP(SU));
  }

  for (iterator I = Exits.begin(), E = Exits.end(); I != E; ++I) {
		SIRSchedUnit *SU = *I;
		SU->scheduleTo(EndSlot);
  }
}

void ListScheduler::scheduleBB(BasicBlock *BB) {
	// Initialize BBContext for this BB.
	BBContext Cnxt(*this, BB);
	// Schedule the BB in BBContext.
	Cnxt.scheduleBB();
}

bool ListScheduler::schedule() {
  // Initial the scheduler.
  buildTimeFrameAndResetSchedule(true);
  
	Function &F = G.getFunction();
	BasicBlock &EntryBB = F.getEntryBlock();

  ReversePostOrderTraversal<BasicBlock *> RPO(&F.getEntryBlock());
 	typedef ReversePostOrderTraversal<BasicBlock *>::rpo_iterator bb_top_iterator;
 
  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)  	
    scheduleBB(*I);

  // Hack: what if we don't schedule the exit specially?
  // Schedule the Exit ASAP.
  SIRSchedUnit *Exit = G.getExit();
  Exit->scheduleTo(calculateASAP(Exit));

  return true;
}
