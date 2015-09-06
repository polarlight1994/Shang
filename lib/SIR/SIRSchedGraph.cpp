//===------- SIRScheduling.cpp - Scheduling Graph on SIR  ------*- C++ -*-===//
//
//                      The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the SIRSchedUnit and SIRSchedGraph. With these class we
// perform scheduling in High-level Synthesis. Please note that the scheduling
// is based on LLVM IR. After scheduling we will annotate the schedule of the
// LLVM Instructions in form of metadata. And we will rebuild the SIR according
// to the schedule.
//
//===----------------------------------------------------------------------===//

#include "sir/SIRSchedGraph.h"
#include "sir/SIRTimingAnalysis.h"
#include "sir/Passes.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

SIRSchedUnit::SIRSchedUnit(unsigned InstIdx, Instruction *Inst,
	                         Type T, BasicBlock *BB, SIRSeqOp *SeqOp)
													 : II(0), Schedule(0), InstIdx(InstIdx),
													   Inst(Inst), T(T), BB(BB), SeqOp(SeqOp) {
	if (T == SIRSchedUnit::Entry || T == SIRSchedUnit::Exit	||
			T == SIRSchedUnit::BlockEntry || T == SIRSchedUnit::Invalid)
		this->Latency = 0;
	else
		this->Latency = 1;
}

SIRSchedUnit::SIRSchedUnit() : InstIdx(0), Inst(0), 
	                             T(SIRSchedUnit::Invalid), BB(0), SeqOp(0) {}

void SIRSchedUnit::EdgeBundle::addEdge(SIRDep NewEdge) {
	// When we add a new edge here, we should choose a appropriate
	// place to insert. The principle is 
	// 1) keep the distance of edges in ascending order
	// 2) replace the old edge if new edge's latency is bigger
	unsigned InsertBefore = 0, Size = Edges.size();
	bool NeedToInsert = true;

	SIRDep::Types NewEdgeType = NewEdge.getEdgeType();
	unsigned NewDistance = NewEdge.getDistance();
	int NewLatency = NewEdge.getLatency();

	while (InsertBefore < Size) {
		SIRDep &CurEdge = Edges[InsertBefore];

		// Keep the edges in ascending order.
		if (CurEdge.getDistance() > NewEdge.getDistance())
			break;

		SIRDep::Types CurEdgeType = CurEdge.getEdgeType();
		unsigned CurDistance = CurEdge.getDistance();
		int CurLatency = CurEdge.getLatency();

		// Update the edge with the tighter constraint.
		if (CurDistance == NewDistance && CurLatency < NewLatency) {
			if (NewEdgeType == CurEdgeType) {
				NeedToInsert = false;
				CurEdge = NewEdge;
			}

			break;
		}

		// Now we have CurDistance < NewDistance, NewEdge is masked by
		// CurEdge if NewEdge has a smaller latency than CurEdge.
		if (NewLatency <= CurLatency && NewEdgeType == CurEdgeType)
			return;

		++InsertBefore;
	}

	assert((InsertBefore == Edges.size()
		      || (Edges[InsertBefore].getLatency() <= NewEdge.getLatency()
					    && Edges[InsertBefore].getDistance() >= NewEdge.getDistance()))
				 && "Bad insert position!");

	// Insert the new edge right before the edge with bigger iterative distance.
	if (NeedToInsert)
		Edges.insert(Edges.begin() + InsertBefore, NewEdge);
}

SIRDep SIRSchedUnit::EdgeBundle::getEdge(unsigned II) const {
	assert(!Edges.empty() && "Unexpected empty edge bundle!");

	SIRDep CurEdge = Edges.front();
	int CurLatency = CurEdge.getLatency(II);

	for (unsigned I = 1, E = Edges.size(); I != E; ++I) {
		SIRDep NewEdge = Edges[I];

		// Find the edge of II with biggest latency.
		int NewLatency = NewEdge.getLatency(II);
		if (NewLatency > CurLatency) {
			CurLatency = NewLatency;
			CurEdge = NewEdge;
		}
	}

	return CurEdge;
}

BasicBlock *SIRSchedUnit::getParentBB() const {
	return BB;
}

void SIRSchedUnit::print(raw_ostream &OS) const {
	if (isEntry()) {
		OS << "Entry Node";
		return;
	}

	if (isExit()) {
		OS << "Exit Node";
		return;
	}

	if (isBBEntry()) OS << "BB Entry\n";
	else if (isPHI()) OS << "PHI\n";
	else OS << "Normal\n";

	if (Inst) {
		OS << "Instruction contained:";
		Inst->dump();
	}

	OS << "Scheduled to " << Schedule;
}

void SIRSchedUnit::dump() const {
	print(dbgs());
	dbgs() << '\n';
}

SIRSchedGraph::SIRSchedGraph(Function &F) : F(F), TotalSUs(2) {
	// Create the entry SU.
	SUnits.push_back(new SIRSchedUnit(0, 0, SIRSchedUnit::Entry, 0, 0));
	// Create the exit SU.
	SUnits.push_back(new SIRSchedUnit(-1, 0, SIRSchedUnit::Exit, 0, 0));
}

SIRSchedGraph::~SIRSchedGraph() {}

ArrayRef<SIRSchedUnit *> SIRSchedGraph::lookupSUs(Value *V) const {
	IR2SUMapTy::const_iterator at = IR2SUMap.find(V);

	if (at == IR2SUMap.end())
		return ArrayRef<SIRSchedUnit *>();

	return at->second;
}

bool SIRSchedGraph::indexSU2IR(SIRSchedUnit *SU, Value *V) {
	// If there are already a map, then we just add the SU into it.
	if (hasSU(V)) {
		IR2SUMap[V].push_back(SU);
		return true;
	}
	
	// Or we need to create a new map.
	SmallVector<SIRSchedUnit *, 4> SUs;
	SUs.push_back(SU);
  IR2SUMap.insert(std::make_pair(V, SUs));
}

ArrayRef<SIRSchedUnit *> SIRSchedGraph::lookupSUs(SIRSlot *S) const {
	Slot2SUMapTy::const_iterator at = Slot2SUMap.find(S);

	if (at == Slot2SUMap.end())
		return ArrayRef<SIRSchedUnit *>();

	return at->second;
}

bool SIRSchedGraph::indexSU2Slot(SIRSchedUnit *SU, SIRSlot *S) {
	// If there are already a map, then we just add the SU into it.
	if (hasSU(S)) {
		Slot2SUMap[S].push_back(SU);

		return true;
	}

	// Or we need to create a new map.
	SmallVector<SIRSchedUnit *, 4> SUs;
	SUs.push_back(SU);
	Slot2SUMap.insert(std::make_pair(S, SUs));
}

void SIRSchedGraph::toposortCone(SIRSchedUnit *Root,
	                               std::set<SIRSchedUnit *> &Visited,
	                               BasicBlock *BB) {
	if (!Visited.insert(Root).second) return;

	typedef SIRSchedUnit::dep_iterator ChildIt;
	std::vector<std::pair<SIRSchedUnit *, ChildIt> > WorkStack;

	WorkStack.push_back(std::make_pair(Root, Root->dep_begin()));

	while (!WorkStack.empty()) {
		SIRSchedUnit *U = WorkStack.back().first;
		ChildIt I = WorkStack.back().second;

		// Visit the current node if all its dependencies are visited.
		if (U->isBBEntry() || I == U->dep_end()) {
			WorkStack.pop_back();
			SUnits.splice(SUnits.end(), SUnits, U);
			continue;
		}

		++WorkStack.back().second;

		SIRSchedUnit *Child = *I;

		// We have reach the top SUnit.
		if (Child->isEntry() || Child->getParentBB() != BB)
			continue;

		// Do not visit the same node twice!
		if (!Visited.insert(Child).second) continue;

		WorkStack.push_back(std::make_pair(Child, Child->dep_begin()));
	}
}

void SIRSchedGraph::topologicalSortSUs() {
	SIRSchedUnit *Entry = getEntry(), *Exit = getExit();
	assert(Entry->isEntry() && Exit->isExit() && "Bad order!");

	// Ensure the Entry is the first.
	SUnits.splice(SUnits.end(), SUnits, Entry);

	// Handle the SUnits located in Slot0r specially since they have
	// no ParentBB.
	bb_iterator at = BBMap.find(NULL);
	if (at != BBMap.end()) {
		MutableArrayRef<SIRSchedUnit *> SUsInSlot0r(at->second);
		for (unsigned i = 0; i < SUsInSlot0r.size(); ++i) {
			SIRSchedUnit *SUnitInSlot0r = SUsInSlot0r[i];

			// Ensure all SUnit in Slot0r is in the front of others.
			SUnits.splice(SUnits.end(), SUnits, SUnitInSlot0r);
		}
	}

	std::set<SIRSchedUnit *> Visited;

	ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
	typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

	for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
		Visited.clear();
		BasicBlock *BB = *I;

		bb_iterator at = BBMap.find(BB);
		if (at == BBMap.end())
			continue;

		MutableArrayRef<SIRSchedUnit *> SUs(at->second);
		for (unsigned i = 0; i < SUs.size(); ++i)
			toposortCone(SUs[i], Visited, BB);
	}

	// Ensure the Exit is the last.
	SUnits.splice(SUnits.end(), SUnits, Exit);

	unsigned Idx = 0;
	for (iterator I = begin(), E = end(); I != E; ++I)
		I->InstIdx = Idx++;

	assert(Idx == size() && "Topological sort is not applied to all SU?");
	assert(getEntry()->isEntry() && getExit()->isExit() && "Broken TopSort!");
}

MutableArrayRef<SIRSchedUnit *> SIRSchedGraph::getSUsInBB(BasicBlock *BB) {
	bb_iterator at = BBMap.find(BB);

	assert(at != BBMap.end() && "BB not found!");

	return MutableArrayRef<SIRSchedUnit *>(at->second);
}

SIRSchedUnit *SIRSchedGraph::createSUnit(Instruction *Inst, BasicBlock *ParentBB,
	                                       SIRSchedUnit::Type T, SIRSeqOp *SeqOp) {
	SIRSchedUnit *U = new SIRSchedUnit(TotalSUs++, Inst, T, ParentBB, SeqOp);
	// Insert the newly create SU before the exit.
	SUnits.insert(SUnits.back(), U);
	// Index the SUnit to the corresponding BB.
	BBMap[ParentBB].push_back(U);
  
	return U;
}

void SIRSchedGraph::resetSchedule() {
	// Reset all SUnits in graph.
	for (iterator I = begin(), E = end(); I != E; ++I)
		I->resetSchedule();
}

