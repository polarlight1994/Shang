#include "sir/SIRScheduling.h"
#include "sir/Passes.h"

using namespace llvm;

char SIRScheduling::ID = 0;
char &llvm::SIRSchedulingID = SIRScheduling::ID;

SIRScheduling::SIRScheduling() : SIRPass(ID) {
	initializeSIRSchedulingPass(*PassRegistry::getPassRegistry());
}

void SIRScheduling::getAnalysisUsage(AnalysisUsage &AU) const {
	SIRPass::getAnalysisUsage(AU);
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
	// Simply return the BBEntry if it had already existed.
	if (G->hasSU(BB)) {
		ArrayRef<SIRSchedUnit *> &SUs = G->lookupSUs(BB);
		for (unsigned I = 0, E = SUs.size(); I != E; ++I)
			if (SUs[I]->isBBEntry())
				return SUs[I];
	}

	SIRSchedUnit *Entry = G->createSUnit(0, BB,
		                                   SIRSchedUnit::BlockEntry, 0);

	if (pred_begin(BB) == pred_end(BB)) {
		// If the BB has no Preds, which means it's a Entry BB.
		// The Entry SUnit of Entry BB should have a SIRDep
		// coming from the Entry of Scheduling Graph.
		Entry->addDep(G->getEntry(), SIRDep::CreateCtrlDep(0));
	}

	// Save the mapping between the SUnit with the Value.
	G->indexSU2IR(Entry, BB);

	// Also create the SUnit for the PHI nodes.
	typedef BasicBlock::iterator iterator;
	for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
		PHINode *PN = dyn_cast<PHINode>(I);
		if (PN) {
			SIRSchedUnit *U = G->createSUnit(PN, BB, SIRSchedUnit::PHI, 0);

			// No need to add the dependency edges from the incoming values, because
			// the SU is anyway scheduled to the same slot as the entry of the BB.
			// And we will build the conditional dependencies for the conditional
			// CFG edge between BBs.
			G->indexSU2IR(U, PN);
		}		
	}

	return Entry;
}

void SIRScheduling::constraintTerminators(BasicBlock *BB) {
	TerminatorInst *Inst = BB->getTerminator();

	ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(Inst);
	assert(SUs.size() == 1 && "Bad number of Scheduling Units!");
	
	SIRSchedUnit *Exit = G->getExit();
	Exit->addDep(SUs[0], SIRDep::CreateCtrlDep(0));
}

void SIRScheduling::buildDataFlowDependencies(SIRSchedUnit *DstU, Value *Src,
	                                            float delay) {	
	if (Instruction *Inst = dyn_cast<Instruction>(Src)) {
		if(!G->hasSU(Inst)) {
			// If we cannot find the source SU, then it must be located
			// in other SchedGraph corresponding to other Function.
			// And we ignore this dependency.
			assert(Inst->getParent()->getParent() != &G->getFunction()
				     && "Cannot find source SU in this Function!");
			return;
		}
	} else if (isa<BasicBlock>(Src))
		// The dependencies from BasicBlocks are control dependencies, we will
		// calculate them based on post dominance frontier later.
		return;

	assert(Src && "Not a valid source!");
	assert((!isa<Instruction>(Src)
			    || DT->dominates(cast<Instruction>(Src)->getParent(), DstU->getParentBB()))
			   && "Flow dependency should be a dominance edge!");
	assert((!isa<BasicBlock>(Src)
			    || DT->dominates(cast<BasicBlock>(Src), DstU->getParentBB()))
			   && "Flow dependency should be a dominance edge!");

	SIRSchedUnit *SrcSU = getDataFlowSU(Src);
	assert(delay >= 0.0f && "Unexpected negative delay!");
	DstU->addDep(SrcSU, SIRDep::CreateValDep(ceil(delay)));
}

void SIRScheduling::buildDataFlowDependencies(SIRSchedUnit *U) {
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
		// The SrcVal must be a Leaf Value.
		assert((isa<Argument>(SrcVal) || isa<ConstantInt>(SrcVal)
			      || SM->lookupSIRReg(dyn_cast<Instruction>(SrcVal)))
			      && "This is not a LeafVal!");

		float delay = I->second.Delay;
		buildDataFlowDependencies(U, SrcVal, delay);
	}

	// If this Unit has no any data flow dependency,
	// then we should create a ctrl dependency for it
	// to limit its scheduling arrange.
	if (U->dep_empty())
		U->addDep(G->getEntry(), SIRDep::CreateCtrlDep(0));
}

void SIRScheduling::buildControlFlowDependencies(BasicBlock *TargetBB,
	                                               ArrayRef<SIRSchedUnit *> SUs) {
	SIRSchedUnit *Entry = 0;

	for (unsigned i = 0; i < SUs.size(); ++i)
		if (SUs[i]->isBBEntry())	
			Entry = SUs[i];

	for (unsigned i = 0; i < SUs.size(); ++i) {
		if (SUs[i]->isBBEntry())
			continue;

	assert(isa<TerminatorInst>(SUs[i]->getInst())
			   && "Unexpected instruction type!");
	assert(SUs[i]->getTargetBB() == TargetBB && "Wrong target BB!");

	Entry->addDep(SUs[i], SIRDep::CreateCtrlDep(0));
	}
}

void SIRScheduling::buildControlFlowDependencies() {
	Function *F = SM->getFunction();

	typedef Function::iterator iterator;
	for (iterator I = F->begin(), E = F->end(); I != E; ++I) {
		BasicBlock *BB = I;

		// When we pass the BB as LLVM Value to the function
		// lookupSUs, we'll get the SUs which contain the
		// terminate instructions targeting this BB and 
		// the entry SU of this BB.
		buildControlFlowDependencies(BB, G->lookupSUs(BB));
	}
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

	// Since the BB Value can have mutil-SUnits, so the IR2SUMap maintain the structure
	// of map<Value *, SmallVector<SIRSchedUnit *, 4>>. However, normally one instruction
	// maps with one SUnit.
	ArrayRef<SIRSchedUnit *> SrcUs = G->lookupSUs(SrcInst), DstUs = G->lookupSUs(DstInst);
	assert((SrcUs.size() == DstUs.size() == 1) && "Only BB Value can have mutil-SUnits!");
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

void SIRScheduling::buildMemoryDependencies() {
	Function &F = G->getFunction();

	// Build the local memory dependencies inside basic blocks.
	ReversePostOrderTraversal<BasicBlock *> RPO(&F.getEntryBlock());

	typedef ReversePostOrderTraversal<BasicBlock *>::rpo_iterator bb_top_iterator;
	for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
		buildLocalMemoryDependencies(*I);

	// Hack: If we want to schedule BB globally, we need to 
	// analysis memory access globally.
}

SIRSchedUnit *SIRScheduling::getDataFlowSU(Value *V) {
	if (isa<Argument>(V)) return G->getEntry();

	assert(G->hasSU(V) && "Flow dependencies missed!");

	// We should get the corresponding SUnit of SeqOp.
	ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(V);

	assert(SUs.size() == 1 || isa<BasicBlock>(V)
		&& "Only BasicBlock can have many SUnits!");

	if(!isa<BasicBlock>(V) && !isa<PHINode>(V))
		return SUs[0];

	for (unsigned I = 0; I < SUs.size(); I++) {
		SIRSchedUnit *CurSU = SUs[I];

		// If this is a BasicBlock, then we return its Entry SUnit.
		if (isa<BasicBlock>(V) && CurSU->isBBEntry())
			return CurSU;

		// If this is a PHINode, then we return the PHI SUnit.
		if (isa<PHINode>(V) && CurSU->isPHI())
			return CurSU;

		continue;
	}	

	assert(false && "There must be something wrong!");
}

void SIRScheduling::buildSchedulingUnits(SIRSlot *S) {
	BasicBlock *BB = S->getParent();

	SIRSchedUnit *BBEntry = 0;
	// If the BB is NULL, this slot should be the entry
	// or the exit of the state-transition graph.
	if (!BB) BBEntry = G->getEntry();
	// Or we create the Entry SUnit for this BB.
	else BBEntry = getOrCreateBBEntry(BB);

	// Collect all the SeqOps in this slot, and we
	// will create SUnits for each of them.
	std::vector<SIRSeqOp *> Ops;
	Ops.insert(Ops.end(), S->op_begin(), S->op_end());

	typedef std::vector<SIRSeqOp *>::iterator op_iterator;
	for (op_iterator OI = Ops.begin(), OE = Ops.end(); OI != OE; ++OI) {
		SIRSeqOp *Op = *OI;
		Instruction *Inst = dyn_cast<Instruction>(Op->getLLVMValue());		

		// We should consider the SeqOp with corresponding IR instruction
		// and no corresponding IR instruction differently. If it is a
		// Shang pseudo instruction, then it is a SlotReg assign operation,
		// which do not need a corresponding SUnit.
		if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst)) {
			// Here we should treat the Ret instruction differently, since it
			// different from others in two ways:
			// 1) The module may have mutil-RetInst, but only one Ret Register.
			// 2) The SeqInst in Ret Register is a pseudo instruction.
			if (Op->getDst()->isOutPort()) {
				// Hack: we just set the Inst to the last Ret Instruction in module.
				BB = Inst->getParent();
				Inst = BB->getTerminator();
			} 
 				else if (II->getIntrinsicID() == Intrinsic::shang_pseudo)
 				continue;
		}			
		
		SIRSchedUnit *U = G->createSUnit(Inst, BB, SIRSchedUnit::Normal, Op);

		buildDataFlowDependencies(U);

		G->indexSU2IR(U, Inst);
		continue;
	}
}

void SIRScheduling::finishBuildingSchedGraph() {
	SIRSchedUnit *Exit = G->getExit();

	typedef SIRSchedGraph::iterator iterator;
	for (iterator I = std::next(G->begin()), E = Exit; I != E; ++I) {
		SIRSchedUnit *U = I;

		// Ignore the BBEntry.
		if (U->isBBEntry()) continue;

		// Terminators will be handled later.
		if (U->isTerminator()) continue;

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
	ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *>>
		RPO(SM->getStartSlot());

	// Build the Scheduling Units according to the SeqOps in Slot.
	typedef
		ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *>>::rpo_iterator
		slot_top_iterator;

	for (slot_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
		buildSchedulingUnits(*I);

	buildControlFlowDependencies();

	buildMemoryDependencies();

	// Constraint all nodes that do not have a user by adding SIRDep to
	// the terminator in its parent BB.
	finishBuildingSchedGraph();

	G->topologicalSortSUs();
}

void SIRScheduling::schedule() {
	ListScheduler LS(*G, G->getEntry()->getSchedule());

	LS.schedule();
}

void SIRScheduling::emitSchedule() {
	// Since the RegVal and RegGuard will be regenerate
	// in ScheduleEmitter to associate the guard condition
	// with the SlotGuard, so drop it first.
	typedef SIR::register_iterator iterator;
	for (iterator I = SM->registers_begin(), E = SM->registers_end(); I != E; ++I) {
		SIRRegister *Reg = *I;
		Reg->dropMux();
	}		

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

void SIRScheduleEmitter::emitToSlot(SIRSeqOp *SeqOp, SIRSlot *ToSlot) {
	SeqOp->setSlot(ToSlot);

	Value *SrcVal = SeqOp->getSrc();
	SIRRegister *Dst = SeqOp->getDst();	
	Value *GuardVal = SeqOp->getGuard();

	Value *InsertPosition = Dst->getLLVMValue();

	// Associate the guard with the Slot guard.
	Value *NewGuardVal = D_Builder.createSAndInst(GuardVal, ToSlot->getGuardValue(),
		                                            GuardVal->getType(), InsertPosition, true);

	assert(getBitWidth(NewGuardVal) == 1 && "Bad BitWidth of Guard Value!");

	Dst->addAssignment(SrcVal, NewGuardVal);
}

void SIRScheduleEmitter::emitSUsInBB(MutableArrayRef<SIRSchedUnit *> SUs) {
	assert(SUs[0]->isBBEntry() && "BBEntry must be placed at the beginning!");
	unsigned EntrySchedSlot = SUs[0]->getSchedule();

	BasicBlock *BB = SUs[0]->getParentBB(); 
	SIRSlot *CurSlot = SM->getLandingSlot(BB);
	assert(CurSlot && "Landing Slot not created?");

	for (unsigned i = 1; i < SUs.size(); ++i) {
		SIRSchedUnit *CurSU = SUs[i];

		unsigned TargetSchedSlot = CurSU->getSchedule();

		// Calculate the real Slot we should emit to according to the
		// difference value between CurScheSlot and EntrySchedSlot.
		// Create the slot if it is not created.
		while (EntrySchedSlot != TargetSchedSlot) {
			++EntrySchedSlot;
			SIRSlot *NextSlot = C_Builder.createSlot(BB, EntrySchedSlot);

			// Replace the Old Slot with the CurSlot int STG
			CurSlot->replaceAllUsesWith(NextSlot);

			C_Builder.createStateTransition(CurSlot, NextSlot, SM->creatConstantBoolean(true));
			CurSlot = NextSlot;
		}

		assert(CurSlot->getSchedule() == TargetSchedSlot && "Schedule not match!");
		emitToSlot(CurSU->getSeqOp(), CurSlot);
	}

 	// Update the latest slot of BB.
 	SM->IndexBB2Slots(BB, SM->getLandingSlot(BB), CurSlot);	
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
}