#include "sir/SIRScheduling.h"
#include "sir/Passes.h"

using namespace llvm;
using namespace std;

char SIRScheduling::ID = 0;
char &llvm::SIRSchedulingID = SIRScheduling::ID;

SIRScheduling::SIRScheduling() : SIRPass(ID) {
	initializeSIRSchedulingPass(*PassRegistry::getPassRegistry());
}

void SIRScheduling::getAnalysisUsage(AnalysisUsage &AU) const {
	SIRPass::getAnalysisUsage(AU);
	AU.addRequired<SIRInit>();
	AU.addRequired<SIRTimingAnalysis>();
	AU.addRequired<AliasAnalysis>();
	AU.addRequired<DominatorTree>();
	AU.addRequired<DataLayout>();
	AU.setPreservesAll();
}

INITIALIZE_PASS_BEGIN(SIRScheduling,
	                    "sir-scheduling", "Perform Scheduling on the SIR",
	                    false, true)
	INITIALIZE_PASS_DEPENDENCY(SIRTimingAnalysis)
	INITIALIZE_PASS_DEPENDENCY(DominatorTree)
	INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(SIRScheduling,
	                  "sir-scheduling", "Perform Scheduling on the SIR",
	                  false, true)

Pass *llvm::createSIRSchedulingPass() {
	return new SIRScheduling();
}

SIRSchedUnit *SIRScheduling::getOrCreateBBEntry(BasicBlock *BB) {
	// Take the Entry SUnit as the pseudo BBEntry of these SUnit in
	// Slot0r which do not have a real parent BB.
	if (!BB) return G->getEntry();

	// Simply return the BBEntry if it had already existed.
	if (G->hasSU(BB)) {
		ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(BB);
		// Only PHI can have mutil-SUnits now.
		assert(SUs.size() == 1 && "Unexpected mutil-SUnits!");

		return SUs.front();
	}

	// Or we have to create the BBEntry.
	SIRSchedUnit *Entry = G->createSUnit(0, BB,
		                                   SIRSchedUnit::BlockEntry, 0);

	// If the BB has no Preds, which means it's a Entry BB.
	// The Entry SUnit of Entry BB should have a SIRDep
	// coming from the Entry of Scheduling Graph.
	if (pred_begin(BB) == pred_end(BB))
		Entry->addDep(G->getEntry(), SIRDep::CreateCtrlDep(0));

	// The BBEntry should be indexed into EntrySlot.
	SIRSlot *EntrySlot = SM->getLandingSlot(BB);
	G->indexSU2Slot(Entry, EntrySlot);

	// Save the mapping between the SUnit with the Value.
	G->indexSU2IR(Entry, BB);

	return Entry;
}

void SIRScheduling::constraintTerminators(BasicBlock *BB) {
	// Get the terminator of this BB and its corresponding SUnits.
	TerminatorInst *Inst = BB->getTerminator();
	ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(Inst);
	
	// The ExitSUnit is depended on these terminator SUnits.
	SIRSchedUnit *Exit = G->getExit();
	for (int i = 0; i < SUs.size(); i++)
		Exit->addDep(SUs[i], SIRDep::CreateCtrlDep(0));
}

void SIRScheduling::buildDependencies() {
	// The dependencies need to be built includes
	// 1) data dependency
	// 2) control dependency
	// 3) memory dependency

	// Visit all SUnits to build data & control dependency.
	typedef SIRSchedGraph::iterator iterator;
	for (iterator I = G->begin(), E = G->end(); I != E; I++) {
		SIRSchedUnit *SU = I;

		buildDataDependencies(SU);
		buildControlDependencies(SU);
	}

	// Visit all BBs to build the memory dependencies.
	Function &F = G->getFunction();
	ReversePostOrderTraversal<BasicBlock *> RPO(&F.getEntryBlock());
	typedef ReversePostOrderTraversal<BasicBlock *>::rpo_iterator bb_top_iterator;
	for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
		buildLocalMemoryDependencies(*I);
}

void SIRScheduling::buildDataDependencies(SIRSchedUnit *U) {
	// Entry/Exit/BBEntry do not have any data dependencies.
	if (U->isEntry() || U->isExit() || U->isBBEntry()) return;

	SIRSeqOp *Op = U->getSeqOp();

	// Construct the data flow dependencies according
	// to the Timing Analysis result.	
	SIRTimingAnalysis::ArrivalMap AT;
	// Extract all the dependencies coming from
	// the Src value of current SIRSeqOp. 
	TA->extractArrivals(SM, Op, AT);

	typedef SIRTimingAnalysis::ArrivalMap::iterator iterator;
	for (iterator I = AT.begin(), E = AT.end(); I != E; I++) {
		Value *SrcVal = I->first;
		// The SrcVal must be a SIRSeqValue.
		SIRRegister *Reg = SM->lookupSIRReg(dyn_cast<Instruction>(SrcVal));
		assert(Reg && "This is not a SeqVal in SIR!");

		// If the SIRSeqValue is a memrdata, then there will be no
		// corresponding SIRSchedUnit, since the register is not
		// assigned inside the SIR module. We can ignore it as it
		// behaves like the argument. And it is safe since
		// this data dependency can be honored by the SlotTransition
		// we set in createMemoryTransaction function.
		if (Reg->isFUOutput()) continue;

		// Get the delay.
		float delay = I->second.Delay;
		assert(delay >= 0.0f && "Unexpected negative delay!");

		// Get the SrcSUnits.
		ArrayRef<SIRSchedUnit *> SrcSUs = getDataFlowSU(SrcVal);
		assert(SrcSUs.size() && "Unexpected NULL SrcSUs!");

		for (int i = 0; i < SrcSUs.size(); i++)
			U->addDep(SrcSUs[i], SIRDep::CreateValDep(ceil(delay)));
	}
}

void SIRScheduling::buildControlDependencies(SIRSchedUnit *U) {
	// Entry do not have any control dependencies except the
	// dependency to the slot transition which will be handled
	// later.
	if (U->isEntry()) return;

	// The first kind of control dependency is that all SUnits
	// is depending on the EntrySU of this BB.
	BasicBlock *ParentBB = U->getParentBB();
	SIRSchedUnit *EntrySU = getOrCreateBBEntry(ParentBB);
	U->addDep(EntrySU, SIRDep::CreateCtrlDep(0));

	// Other two kinds of control dependency is about the slot transition.
	// 1) transition to next slot in current basic block.
	// 2) transition to successor basic block.
	if (!U->isSlotTransition()) return;

	// Get the SlotTransition.
	SIRSlotTransition *SST = dyn_cast<SIRSlotTransition>(U->getSeqOp());
	assert (SST && "Unexpected NULL SIRSlotTransition!");

	// Get the destination slot.
	SIRSlot *DstSlot = SST->getDstSlot();

	// All SUnits in destination slot are depended on this SlotTransition.
	ArrayRef<SIRSchedUnit *> SUsInDstSlot = G->lookupSUs(DstSlot);

	// If the fist SUnit in destination slot is a BBEntry, that means we
	// are transiting to successor BB. In this circumstance, we can just
	// constraint the BBEntry, since other SUnits is constrained by the
	// BBEntry already.
	if (SUsInDstSlot[0]->isBBEntry())
		SUsInDstSlot[0]->addDep(U, SIRDep::CreateCtrlDep(0));
	else
		for (int i = 0; i < SUsInDstSlot.size(); i++)
			SUsInDstSlot[i]->addDep(U, SIRDep::CreateCtrlDep(0));

	// Constraint the non-dep SUnit to the Entry.
	if (U->dep_empty())
		U->addDep(G->getEntry(), SIRDep::CreateCtrlDep(0));
}

void SIRScheduling::buildMemoryDependency(Instruction *SrcInst, Instruction *DstInst) {
	// If either of them are call instruction, we need a dependencies,
	// because we are not sure the memory locations accessed by the call.
	if (!isCall(SrcInst) && !isCall(DstInst)) {
		// Ignore the RAR dependencies
		if (!SrcInst->mayWriteToMemory() && !DstInst->mayWriteToMemory())
			return;

		// There is no dependencies if the memory locations do not alias each other.
		if (isNoAlias(SrcInst, DstInst, AA)) return;
	}

	ArrayRef<SIRSchedUnit *> SrcUs = G->lookupSUs(SrcInst), DstUs = G->lookupSUs(DstInst);
	// Only one SUnit expected here.
	assert((SrcUs.size() == DstUs.size() == 1) && "Unexpected mutil-SUnits!");
	SIRSchedUnit *SrcU = SrcUs.front(), *DstU = DstUs.front();

	unsigned Latency = 1;

	// If the DstInst is call instruction, then the real
	// dependency is from the callee instruction to Src.
	// So we can minus the delay by 1.
	if (isa<CallInst>(DstInst)) {
		Latency = 0;
	}

	DstU->addDep(SrcU, SIRDep::CreateMemDep(Latency, 0));
}

void SIRScheduling::buildLocalMemoryDependencies(BasicBlock *BB) {
	typedef BasicBlock::iterator iterator;
	SmallVector<Instruction *, 16> PiorMemInsts;

	for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
		Instruction *Inst = I;

		if (!isLoadStore(Inst) && !isCall(Inst))
			continue;

		if (!G->hasSU(Inst))
			continue;

		for (unsigned I = 0, E = PiorMemInsts.size(); I < E; ++I)
			buildMemoryDependency(PiorMemInsts[I], Inst);

		PiorMemInsts.push_back(Inst);
	}
}

ArrayRef<SIRSchedUnit *> SIRScheduling::getDataFlowSU(Value *V) {
	// If we are getting the corresponding SUnit of the argument,
	// then we can just return the Entry SUnit of the SchedGraph.
	if (isa<Argument>(V)) return G->getEntry();

	assert(G->hasSU(V) && "Flow dependencies missed!");

	// We should get the corresponding SUnit of this LLVM IR.
	// To be noted that, if we are passing in BB as Value here,
	// then we will get the Entry SUnit of the BB.
	ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(V);

	if (isa<BasicBlock>(V))
		assert(SUs.size() == 1 && "Unexpected multi-SUnits!");

	return SUs;
}

void SIRScheduling::buildSchedulingUnits(SIRSlot *S) {
	int temp = S->getSlotNum();

	if (temp == 220)
		int i = 1;

	BasicBlock *BB = S->getParent();

	SIRSchedUnit *BBEntry = 0;
	// If the BB is NULL, this slot should be the entry
	// or the exit of the state-transition graph.
	if (!BB) BBEntry = G->getEntry();
	// Or we create the Entry SUnit for this BB.
	else BBEntry = getOrCreateBBEntry(BB);

	// Collect all SeqOps in this slot and create SUnits for them.
	std::vector<SIRSeqOp *> Ops;
	Ops.insert(Ops.end(), S->op_begin(), S->op_end());

	typedef std::vector<SIRSeqOp *>::iterator op_iterator;
	for (op_iterator OI = Ops.begin(), OE = Ops.end(); OI != OE; ++OI) {
		SIRSeqOp *Op = *OI;

		Instruction *Inst = dyn_cast<Instruction>(Op->getLLVMValue()); 
		
		// Detect whether the type of SIRSchedUnit according to the DstReg.
		SIRSchedUnit::Type Ty;
		SIRRegister *DstReg = Op->getDst();
		if (DstReg->isPHI())			  Ty = SIRSchedUnit::PHI;
		else if (DstReg->isSlot())	Ty = SIRSchedUnit::SlotTransition;
		else                        Ty = SIRSchedUnit::Normal;

		SIRSchedUnit *U = G->createSUnit(Inst, BB, Ty, Op);

		// Index the SUnit to the Slot and the LLVM IR.
		G->indexSU2Slot(U, S);
		G->indexSU2IR(U, Inst);

		continue;
	}
}

void SIRScheduling::finishBuildingSchedGraph() {
	SIRSchedUnit *Exit = G->getExit();

	typedef SIRSchedGraph::iterator iterator;
	for (iterator I = llvm::next(G->begin()), E = Exit; I != E; ++I) {
		SIRSchedUnit *U = I;

		// Ignore the BBEntry.
		if (U->isBBEntry()) continue;

		// Terminators will be handled later.
		if (U->isTerminator()) continue;

		// Constraint the non-use SUnit to the Exit.
		if(U->use_empty())
			Exit->addDep(U, SIRDep::CreateCtrlDep(0));
	}

	// Handle the Terminators.
	Function &F = G->getFunction();
	for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
		constraintTerminators(I);
	}
}

void SIRScheduling::buildSchedulingGraph() {
	// Build the scheduling units according to the original scheduling.
	ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> >
		RPO(SM->getStartSlot());

	// Build the Scheduling Units according to the SeqOps in Slot.
	typedef	
		ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> >::rpo_iterator
		slot_top_iterator;

	for (slot_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
		buildSchedulingUnits(*I);

	buildDependencies();

	// Constraint all nodes that do not have a user by adding SIRDep to
	// the terminator in its parent BB.
	finishBuildingSchedGraph();

	// Sort all SUnits according to the dependencies and reassign the
	// index based on the result, so we can easily recognize the back-edge
	// according to the index.
	G->topologicalSortSUs();
}

void SIRScheduling::schedule() {
	ListScheduler LS(*G, G->getEntry()->getSchedule());

	LS.schedule();
}

void SIRScheduling::emitSchedule() {
	SIRScheduleEmitter SSE(*TD, SM, *G);

	SSE.emitSchedule();
}

bool SIRScheduling::runOnSIR(SIR &SM) {
	// Initialize the SIR and corresponding LLVM Function.
	this->SM = &SM;
	this->TA = &getAnalysis<SIRTimingAnalysis>();
	this->AA = &getAnalysis<AliasAnalysis>();
	this->DT = &getAnalysis<DominatorTree>();
	this->TD = &getAnalysis<DataLayout>();
	Function &F = *SM.getFunction();

	OwningPtr<SIRSchedGraph> GPtr(new SIRSchedGraph(F));
	G = GPtr.get();

	// Build the Scheduling Graph and all the dependencies
	// between the SUnits.
	buildSchedulingGraph();

	schedule();

	emitSchedule();

	return true;
}

namespace {
	bool SUnitLess(SIRSchedUnit *LHS, SIRSchedUnit *RHS) {
		return LHS->getSchedule() < RHS->getSchedule();
	}
}

void SIRScheduleEmitter::insertSlotBefore(SIRSlot *S, SIRSlot *DstS,
	                                        SIRSlot::EdgeType T, Value *Cnd) {
	SmallVector<SIRSlot::EdgePtr, 4> Preds;
	for (SIRSlot::pred_iterator I = DstS->pred_begin(), E = DstS->pred_end(); I != E; I++) {
		Preds.push_back(*I);
	}

	typedef SmallVector<SIRSlot::EdgePtr, 4>::iterator iterator;
	for (iterator I = Preds.begin(), E = Preds.end(); I != E; I++) {
		SIRSlot *Pred = I->getSlot();

		// Unlink the edge from Pred to DstS.
		Pred->unlinkSucc(DstS);

		// Link the edge from Pred to S.
		C_Builder.createStateTransition(Pred, S, I->getCnd());
	}

	// Link the edge from S to DstS.
	C_Builder.createStateTransition(S, DstS, Cnd);
}

void SIRScheduleEmitter::emitSUsInBB(MutableArrayRef<SIRSchedUnit *> SUs) {
	assert(SUs[0]->isBBEntry() && "BBEntry must be placed at the beginning!");

	BasicBlock *BB = SUs[0]->getParentBB(); 
	SIRSlot *EntrySlot = SM->getLandingSlot(BB);

	assert(EntrySlot && "Landing Slot not created?");
	assert(EntrySlot->getStepInLocalBB() == 0 && "Unexpected local step!");

	// The global schedule result of the Entry SUnit.
	unsigned EntrySUSchedule = SUs[0]->getSchedule();

	std::vector<SIRSchedUnit *> NewSUs(SUs.begin() +1, SUs.end());
	// Sort the SUs to make sure they are ranged by schedule in ascending order.
	std::sort(NewSUs.begin(), NewSUs.end(), SUnitLess);

	assert(SUs[0]->isBBEntry() && "BBEntry must be placed at the beginning!");

	for (unsigned i = 0; i < NewSUs.size(); ++i) {
		SIRSchedUnit *CurSU = NewSUs[i];
		SIRSeqOp *SeqOp = CurSU->getSeqOp();
		SIRSlot *EmitSlot = SeqOp->getSlot();

		// The local step of specified slot.
		unsigned EmitSlotStep = EmitSlot->getStepInLocalBB();
		// The target local step of the SUnit. Since we may specify the target
		// slot in SIRBuild pass, so we must consider the constraint it brings.
		unsigned TargetStep = CurSU->getSchedule() - EntrySUSchedule;
		TargetStep = std::max(TargetStep, EmitSlotStep);

		// The numbers of slots need to allocate to meet the
    // target local step of the SUnit.
		unsigned SlotsNeedToAlloca = TargetStep - EmitSlotStep;

		// Set the schedule of the EmitSlot to the target step
		// since we will allocate enough slots before it.
		EmitSlot->setStep(TargetStep);

		while(SlotsNeedToAlloca--) {
			SIRSlot *NewSlot = C_Builder.createSlot(BB, TargetStep--);

			// Insert the NewSlot from bottom to up before the EmitSlot.
			insertSlotBefore(NewSlot, EmitSlot, SIRSlot::Sucessor, D_Builder.createIntegerValue(1, 1));

			EmitSlot = NewSlot;
		}
	}
}

void SIRScheduleEmitter::emitSchedule() {
	// Get some basic information.
	Function &F = *SM->getFunction();

	// Visit the basic block in topological order to emit all SUnits in BB.
	ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
	typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;
	for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
		BasicBlock *BB = *I;

		if (!G.isBBReachable(BB))
			continue;

		MutableArrayRef<SIRSchedUnit *> SUs(G.getSUsInBB(BB));
		emitSUsInBB(SUs);
	}

	// After ScheduleEmit, lots of SlotTransitions will be replaced by new ones
	// and become useless, so we remove them here.
	typedef SIR::seqop_iterator seqop_iterator;
	for (seqop_iterator I = SM->seqop_begin(), E = SM->seqop_end(); I != E;) {
		// We must move forward the iterator here to avoid the error caused by
		// iterator erase function called below.
		SIRSeqOp *SeqOp = I++;

		if (SIRSlotTransition *SST = dyn_cast<SIRSlotTransition>(SeqOp)) {
			SIRSlot *SrcSlot = SST->getSrcSlot();
			SIRSlot *DstSlot = SST->getDstSlot();

			if (!SrcSlot->hasNextSlot(DstSlot))
				SM->deleteUselessSeqOp(SeqOp);
		}
	}
}