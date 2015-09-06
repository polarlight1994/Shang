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

	// Schedule the BBEntry to Entry Slot.
	scheduleSUsToEntrySlot();

	ArrayRef<SIRSchedUnit *> SUs(S->getSUsInBB(BB));
	collectReadySUs(SUs);
}

void BBContext::exit(BasicBlock *BB) {
	// Schedule the PHI to Exit Slot.
	// To be noted that, the PHI scheduled here is actually from
	// the successor BB.
	scheduleSUsToExitSlot();

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

bool BBContext::isSUReady(SIRSchedUnit *SU) {
	// A SU is ready only when all SUs it depends on is scheduled.
	bool AllScheduled = true;

	typedef SIRSchedUnit::dep_iterator iterator;
	for (iterator DI = SU->dep_begin(), DE = SU->dep_end(); DI != DE; ++DI) {
		SIRSchedUnit *DepSU = *DI;

		// The BBEntry is definitely scheduled it into entry slot of this BB.
		// The PHI is definitely scheduled it into exit slot of this BB.
		// Also all back-edge will ended on the BBEntry/Entry, so we ignore
		// these SUnit.
		if (DepSU->isEntry() || DepSU->isBBEntry() || DepSU->isPHI())
			continue;

		// All SUnit in Slot0r is also definitely scheduled into Slot0r.
		if (!DepSU->getParentBB()) continue;

		AllScheduled &= DepSU->isScheduled();
	}

	return AllScheduled;
}

void BBContext::collectReadySUs(ArrayRef<SIRSchedUnit *> SUs) {
	typedef ArrayRef<SIRSchedUnit *>::iterator iterator;
	for (iterator I = SUs.begin(), E = SUs.end(); I != E; ++I) {
		SIRSchedUnit *SU = *I;

		// If the SUnit is already scheduled, that means this is
		// a back-edge. So we just ignore it.
		if (SU->isScheduled())
			continue;

		// Entrys, Exits and PHINodes will be handled elsewhere.
		if (SU->isBBEntry() || SU->isExit() || SU->isPHI())
			continue;

		// If the SUnit is located in other BB, ignore it since it
		// will be handled in that BB.
		if (SU->getParentBB() != BB)
			continue;

		if (isSUReady(SU))
			ReadyQueue.push(SU);
	}

	// Sort the SUnits.
	ReadyQueue.reheapify();
}

void BBContext::scheduleBB() {
	// Collect information of BB.
	enter(BB);

	while (!ReadyQueue.empty()) {
		SIRSchedUnit *SU = ReadyQueue.top();
		ReadyQueue.pop();

		unsigned Step = std::max(S.calculateASAP(SU), StartSlot);

		SU->scheduleTo(Step);

		// After we schedule a unit, we should reset the TimeFrame.
		S.resetTimeFrame();
		// Also we should reset the ready queue.
		SmallVector<SIRSchedUnit *, 4> Users = SU->getUseList();
		collectReadySUs(Users);
	}

	exit(BB);
}

void BBContext::scheduleSUsToEntrySlot() {
  // Get the BBEntry SUnit.
  ArrayRef<SIRSchedUnit *> SUs = S->lookupSUs(BB);
	assert(SUs.size() == 1 && "Unexpected mutil-SUnits!");
	SIRSchedUnit *Entry = SUs[0];

	// Calculate the EntrySlot and schedule the BBEntry into it.
	unsigned EntrySlot = S.calculateASAP(Entry);
	Entry->scheduleTo(EntrySlot);
}

void BBContext::scheduleSUsToExitSlot() {
	// The SUs that corresponds to the terminator of BB are exit nodes.
	ArrayRef<SIRSchedUnit *> SUnits = S->lookupSUs(BB->getTerminator());

	Exits.insert(SUnits.begin(), SUnits.end());

	// PHINodes also need to schedule to the end of the BB.
	ArrayRef<SIRSchedUnit *> SUs = S->getSUsInBB(BB);
	typedef ArrayRef<SIRSchedUnit *>::iterator array_iterator;
	for (array_iterator I = SUs.begin(), E = SUs.end(); I != E; I++) {
		SIRSchedUnit *SU = *I;
		if (SU->isPHI())
			Exits.insert(SU);
	}

  // Schedule the exit to the exit slot.
	typedef std::set<SIRSchedUnit *>::iterator set_iterator;
  for (set_iterator I = Exits.begin(), E = Exits.end(); I != E; ++I) {
		SIRSchedUnit *SU = *I;
		EndSlot = std::max(StartSlot, S.calculateASAP(SU));
  }

  for (set_iterator I = Exits.begin(), E = Exits.end(); I != E; ++I) {
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
