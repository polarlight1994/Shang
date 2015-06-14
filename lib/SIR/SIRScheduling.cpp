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
	// Simply return the BBEntry if it had already existed.
	if (G->hasSU(BB)) {
		ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(BB);
		for (unsigned I = 0, E = SUs.size(); I != E; ++I)
			if (SUs[I]->isBBEntry() && SUs[I]->getParentBB() == BB)
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

	return Entry;
}

void SIRScheduling::constraintTerminators(BasicBlock *BB) {
	TerminatorInst *Inst = BB->getTerminator();

	ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(Inst);
	
	SIRSchedUnit *Exit = G->getExit();

	for (int i = 0; i < SUs.size(); i++) {
		Exit->addDep(SUs[i], SIRDep::CreateCtrlDep(0));
	}	
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
		assert(!(isa<ConstantInt>(SrcVal) || isa<ConstantVector>(SrcVal) ||
			       isa<ConstantAggregateZero>(SrcVal) ||
						 isa<ConstantPointerNull>(SrcVal) ||
						 isa<Argument>(SrcVal) || isa<ConstantInt>(SrcVal) ||
						 isa<GlobalValue>(SrcVal))
			     && "Should be ignored in extract arrivals!");
		assert(SM->lookupSIRReg(dyn_cast<Instruction>(SrcVal))
			     && "This is not a SeqVal in SIR!");

		if(isa<Argument>(SrcVal) || isa<ConstantInt>(SrcVal) || isa<GlobalValue>(SrcVal))
			Value *temp = SrcVal;

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

	// Get the Entry SUnit of this BB.
	for (unsigned i = 0; i < SUs.size(); ++i) {		
		if (SUs[i]->isBBEntry() && SUs[i]->getParentBB() == TargetBB)  	
			Entry = SUs[i];			
	}


	// Create the CtrlDep from SUnits which targets this BB
	// to Entry SUnit to this BB.
	for (unsigned i = 0; i < SUs.size(); ++i) {
		if (SUs[i]->isBBEntry() && SUs[i]->getParentBB() == TargetBB)
			continue;
  
  Instruction *Inst = SUs[i]->getInst();
	assert(!Inst && (SUs[i]->isEntry() || SUs[i]->isBBEntry())
		     && "Unexpected NULL instruction!");
	assert(!Inst || isa<TerminatorInst>(Inst) && "Unexpected instruction type!");
	assert(!Inst || SUs[i]->getTargetBB() == TargetBB && "Wrong target BB!");

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
}

SIRSchedUnit *SIRScheduling::getDataFlowSU(Value *V) {
	if (isa<Argument>(V)) return G->getEntry();

	assert(G->hasSU(V) && "Flow dependencies missed!");

	// We should get the corresponding SUnit of SeqOp.
	ArrayRef<SIRSchedUnit *> SUs = G->lookupSUs(V);

// 	assert(SUs.size() == 1 || isa<BasicBlock>(V) || isa<PHINode>(V)
// 		     && "Only BasicBlock can have many SUnits!");
//
// 	if(!isa<BasicBlock>(V) && !isa<PHINode>(V))
// 		return SUs[0];
	// Still only BB Value and PHI node can have mutil-SUnits,
	// however, we create a pseudo instruction to hold the value
	// in PHINode, so the value here is not the PHINode itself.
	// For now, we can't detect whether this value is associated
	// with PHINode through the type of Value itself.
	if (SUs.size()) return SUs[0];

	for (unsigned I = 0; I < SUs.size(); I++) {
		SIRSchedUnit *CurSU = SUs[I];

// 		// If this is a BasicBlock, then we return its Entry SUnit.
// 		if (isa<BasicBlock>(V) && CurSU->isBBEntry())
// 			return CurSU;
//
// 		// If this is a PHINode, then we return the PHI SUnit.
// 		if (isa<PHINode>(V) && CurSU->isPHI())
// 			return CurSU;
		// Still only BB Value and PHI node can have mutil-SUnits,
		// however, we create a pseudo instruction to hold the value
		// in PHINode, so the value here is not the PHINode itself.
		// For now, we can't detect whether this value is associated
		// with PHINode through the type of Value itself.
		if (CurSU->isPHI() || CurSU->isBBEntry())
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

	// Index all the TargetBB of this Entry SUnit according to
	// the successor of this Slot.
	typedef SIRSlot::succ_iterator iterator;
	for (iterator I = S->succ_begin(), E = S->succ_end(); I != E; I++) {
		SIRSlot *DstSlot = *I;
		BasicBlock *DstBB = DstSlot->getParent();

		// If it's not a BB transition, ignore it. And note that
		// if DstBB and BB are the same and not empty, then only 
		// when S is the latest slot, then there are a BB 
		// transition from BB to BB itself.
		if (DstBB == BB && (!BB || S!= SM->getLatestSlot(BB))) 
			continue;

		G->indexSU2IR(BBEntry, DstBB);
	}

	// Collect all SeqOps in this slot and create SUnits for them.
	std::vector<SIRSeqOp *> Ops;
	Ops.insert(Ops.end(), S->op_begin(), S->op_end());

	typedef std::vector<SIRSeqOp *>::iterator op_iterator;
	for (op_iterator OI = Ops.begin(), OE = Ops.end(); OI != OE; ++OI) {
		SIRSeqOp *Op = *OI;

		// Do not create SUnit for the SlotTransition, since we know which
		// slot to emit it. And the dependency between the SUnits in DstSlot
		// to this SlotTransition will be handled in emitting process.
		if (isa<SIRSlotTransition>(Op)) continue;

		Instruction *Inst = dyn_cast<Instruction>(Op->getLLVMValue()); 
		
		// Detect whether the type of SIRSchedUnit from the DstReg.
		SIRSchedUnit::Type Ty;
		SIRRegister *DstReg = Op->getDst();
		if (DstReg->isPHI())			  Ty = SIRSchedUnit::PHI;
		else                        Ty = SIRSchedUnit::Normal;

		SIRSchedUnit *U = G->createSUnit(Inst, BB, Ty, Op);

		if (U->isPHI())
			// Since the PHI latches are predicated, their depends on the parent BB,
			// i.e. the PHI latches are implicitly guard by the 'condition' of their
			// parent BB.
			U->addDep(BBEntry, SIRDep::CreateCtrlDep(0));

		buildDataFlowDependencies(U);

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