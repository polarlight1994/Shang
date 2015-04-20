//===-------------------- SIRBuild.cpp - IR2SIR -----------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of the SIRBuild pass, which construct the SIR from LLVM IR
//
//===----------------------------------------------------------------------===//
#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/Passes.h"

#include "llvm/InstVisitor.h"
#include "llvm/PassSupport.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

//------------------------------------------------------------------------//
char SIRInit::ID = 0;

Pass *llvm::createSIRInitPass() {
  return new SIRInit();
}
//------------------------------------------------------------------------//

INITIALIZE_PASS_BEGIN(SIRInit,
                      "shang-ir-init", "SIR Init",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
	INITIALIZE_PASS_DEPENDENCY(SIRAllocation)
INITIALIZE_PASS_END(SIRInit,
                    "shang-ir-init", "SIR Init",
                    false, true)

bool SIRInit::runOnFunction(Function &F) {
  DataLayout &TD = getAnalysis<DataLayout>();
	SIRAllocation &SA = getAnalysis<SIRAllocation>();

  SM = new SIR(&F);

  // Initialize SIR from IR by transform llvm-inst to Shang-inst.
  SIRBuilder Builder(SM, TD, SA);

  // Build the general interface(Ports) of the module.
  Builder.buildInterface(&F);

  // Visit the basic block in topological order.
  ReversePostOrderTraversal<BasicBlock *> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock *>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    Builder.visitBasicBlock(*(*I));

  return false;
}

void SIRInit::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DataLayout>();
	AU.addRequired<SIRAllocation>();
  AU.setPreservesAll();
}

void SIRBuilder::buildInterface(Function *F) {
  // Add basic ports by create Arguments for Function.
  C_Builder.createPort(SIRPort::Clk, "clk", 1);
  C_Builder.createPort(SIRPort::Rst, "rstN", 1);
  C_Builder.createPort(SIRPort::Start, "start", 1);

  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
       I != E; ++I) {
    Argument *Arg = I;
    StringRef Name = Arg->getName();
    unsigned BitWidth = TD.getTypeSizeInBits(Arg->getType());

    C_Builder.createPort(SIRPort::ArgPort, Name, BitWidth);
  }
  
  Type *RetTy = F->getReturnType();
  if (!RetTy->isVoidTy()) {
    assert(RetTy->isIntegerTy() && "Only support return integer now!");
    unsigned BitWidth = TD.getTypeSizeInBits(RetTy);

    C_Builder.createPort(SIRPort::RetPort, "return_value", BitWidth);
  }

  // Create StartSlot for module.
  SIRSlot *IdleStartSlot = C_Builder.createSlot(0, 0);
  assert(IdleStartSlot && "We need to create a start slot here!");

  // Get the Start signal of module.
  Value *Start 
    = cast<SIRInPort>(SM->getPort(SIRPort::Start))->getValue();

  // Whole module will be run only when the Start signal is true.
  Value *InsertPosition = F->getEntryBlock().getFirstInsertionPt();
  Value *IdleLoopCondition = D_Builder.createSNotInst(Start, Start->getType(), InsertPosition, true);
  C_Builder.createStateTransition(IdleStartSlot, IdleStartSlot, IdleLoopCondition);

  // If the Start signal is true, then slot will jump to the slot of first BB.
  BasicBlock *EntryBB = &F->getEntryBlock();
  SIRSlot *EntryBBSlot = C_Builder.getOrCreateLandingSlot(EntryBB);
  C_Builder.createStateTransition(IdleStartSlot, EntryBBSlot, Start);
}

void SIRBuilder::visitBasicBlock(BasicBlock &BB) {
  // Create the landing slot for this BB.
  SIRSlot *S = C_Builder.getOrCreateLandingSlot(&BB);

  // Hack: After implement the DataflowPass, we should treat
  // the Unreachable BB differently.

  typedef BasicBlock::iterator iterator;
  for (iterator I = BB.begin(), E = BB.end(); I != E; ++I) 
    visit(I);
}

//-----------------------------------------------------------------------//
// All Data-path instructions should be built by SIRDatapathBuilder
//-----------------------------------------------------------------------//

void SIRBuilder::visitTruncInst(TruncInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitZExtInst(ZExtInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitSExtInst(SExtInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitPtrToIntInst(PtrToIntInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitIntToPtrInst(IntToPtrInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitBitCastInst(BitCastInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitSelectInst(SelectInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitICmpInst(ICmpInst &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitBinaryOperator(BinaryOperator &I) {
  D_Builder.visit(I);
}

void SIRBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  D_Builder.visit(I);
}

//-----------------------------------------------------------------------//
// All Control-path instructions should be built by SIRCtrlRgnBuilder
//-----------------------------------------------------------------------//

void SIRBuilder::visitBranchInst(BranchInst &I) {
  C_Builder.visit(I);
}

void SIRBuilder::visitSwitchInst(SwitchInst &I) {
	C_Builder.visit(I);
}

void SIRBuilder::visitReturnInst(ReturnInst &I) {
  C_Builder.visit(I);
}

void SIRBuilder::visitStoreInst(StoreInst &I) {
	// Get the corresponding memory bank.
	SIRMemoryBank *Bank = SA.getMemoryBank(I);

	C_Builder.createMemoryTransaction(I.getPointerOperand(), I.getValueOperand(), Bank, I);
}

void SIRBuilder::visitLoadInst(LoadInst &I) {
	// Get the corresponding memory bank.
	SIRMemoryBank *Bank = SA.getMemoryBank(I);

	C_Builder.createMemoryTransaction(I.getPointerOperand(), 0, Bank, I);
}


/// Functions to provide basic information
unsigned SIRCtrlRgnBuilder::getBitWidth(Value *U) {
  return TD.getTypeSizeInBits(U->getType());
}

/// Functions to build Control Logic
Instruction *SIRCtrlRgnBuilder::findInsertPostion(BasicBlock *BB, bool IsSlot) {
	assert(BB && "Unexpected empty BB!");

	// If the pseudo instruction is created for OutPort,
	// then the insert position should be right before 
	// the Ret instruction.
	if (!IsSlot) {
		TerminatorInst *TI = BB->getTerminator();
		assert(isa<ReturnInst>(TI) && "Wrong pseudo instruction or Wrong BB!");
		return TI;
	}

	// Otherwise, The insert position should be the front of non-PHI
	// instructions and all pseudo instructions should be sorted by
	// its creating time order.
	BasicBlock *EntryBB = &SM->getFunction()->getEntryBlock();
	typedef BasicBlock::iterator iterator;
	for (iterator I = EntryBB->begin(), E = EntryBB->end(); I != E; ++I) {
		// Skip all the PHI instruction.
		if (isa<PHINode>(I)) continue;
		if (isa<IntrinsicInst>(I)) continue;
		return I;
	}
}

Instruction *SIRCtrlRgnBuilder::createPseudoInst(unsigned BitWidth, Value *InsertPosition) {
  // For slots and ports, we create this pseudo SeqInst,
  // which will represent the assign operation to
  // the register of this slot or port.
  // The pseudo SeqInst will be:
  // SeqInst = 1'b1;
  // And the insert position will be the front of whole module.
  Value *PseudoSrcVal = SM->createIntegerValue(BitWidth, 1);
	Value *PseudoGuardVal = SM->creatConstantBoolean(true);
	Value *Ops[] = { PseudoSrcVal, PseudoGuardVal };

  Value *PseudoInst = D_Builder.createShangInstPattern(Ops,
                                                       SM->createIntegerType(BitWidth),
                                                       InsertPosition, Intrinsic::shang_pseudo,
                                                       true);
  return dyn_cast<Instruction>(PseudoInst);
}

SIRRegister *SIRCtrlRgnBuilder::createRegister(StringRef Name, unsigned BitWidth,
	                                             BasicBlock *ParentBB,
                                               Instruction *SeqInst, uint64_t InitVal, 
                                               SIRRegister::SIRRegisterTypes T) {
  assert(SeqInst || T == SIRRegister::OutPort || T == SIRRegister::SlotReg
         && "Only Reg for Port or Slot can have no corresponding LLVM IR instruction!");

	// If the SeqInst is empty, then this register is created for
	// Slot or OutPort. In this circumstance, we can construct a
	// pseudo SeqInst for it. To be noted that, the insert position
	// of this pseudo instruction for SlotReg and OutPort is different.

  if (!SeqInst && T == SIRRegister::SlotReg) {
		if (!ParentBB) ParentBB = &SM->getFunction()->getBasicBlockList().front();
		Value *InsertPosition = findInsertPostion(ParentBB, T == SIRRegister::SlotReg);
    SeqInst = createPseudoInst(BitWidth, InsertPosition);
  }

	// For the OutPort Register, we also create a pseudo instruction. 
	// But this pseudo instruction will be inserted into the back of
	// whole module.
	if (!SeqInst && T == SIRRegister::OutPort) {
		assert(!ParentBB && "Unexpected ParentBB!");
		ParentBB = &SM->getFunction()->getBasicBlockList().back();
		Value *InsertPosition = findInsertPostion(ParentBB, false);
		SeqInst = createPseudoInst(BitWidth, InsertPosition);
	}

	assert(SeqInst && "Unexpected empty PseudoSeqInst!");
	assert(ParentBB && "Unexpected empty ParentBB!");
	assert(!SM->lookupSIRReg(SeqInst) && "Register already created before!");

	// For the General Register.
	SIRRegister *Reg = new SIRRegister(Name, BitWidth, InitVal, ParentBB, T, SeqInst);

	// Index the register and index it with the SeqInst.
	SM->IndexRegister(Reg);
	SM->IndexSeqInst2Reg(SeqInst, Reg);

	return Reg;
}

SIRPort *SIRCtrlRgnBuilder::createPort(SIRPort::SIRPortTypes T, StringRef Name, 
                                       unsigned BitWidth) {
  // InPort or OutPort?
  if (T <= SIRPort::InPort) {
    LLVMContext &C = SM->getContext();
    SIRPort *P = new SIRInPort(T, BitWidth, Name, C);
    SM->IndexPort(P);
    return P;
  } else {
    // Record the Idx of RetPort
    if (T == SIRPort::RetPort) SM->setRetPortIdx(SM->getPortsSize()); 

    // Create the register for OutPort. To be noted that,
		// here we cannot assign the ParentBB for this register.
    SIRRegister *Reg = createRegister(Name, BitWidth, 0, 0, 0,
                                      SIRRegister::OutPort);
    SIROutPort *P = new SIROutPort(T, Reg, BitWidth, Name);
    SM->IndexPort(P);
    return P;
  }  
}

void SIRCtrlRgnBuilder::createPortsForMemoryBank(SIRMemoryBank *SMB) {
	// Address pin
	SIRRegister *Addr = createRegister(SMB->getAddrName(), SMB->getAddrWidth(), 0, 0, 0, SIRRegister::FUInput);
	SMB->addFanin(Addr);

	// Write (to memory) data pin
	unsigned WDataWidth = SMB->getDataWidth();
	SIRRegister *WData = createRegister(SMB->getWDataName(), WDataWidth, 0, 0, 0, SIRRegister::FUInput);
	SMB->addFanout(WData);

	// Read (from memory) data pin
	unsigned RDataWidth = SMB->getDataWidth();
	SIRRegister *RData = createRegister(SMB->getRDataName(), RDataWidth, 0, 0, 0, SIRRegister::FUOutput);
	SMB->addFanout(RData);

	// Enable pin
	SIRRegister *Enable = createRegister(SMB->getEnableName(), 1, 0, 0,	0, SIRRegister::FUInput);
	SMB->addFanin(Enable);

	// Write enable pin
	SIRRegister *WriteEn = createRegister(SMB->getWriteEnName(), 1, 0, 0,	0, SIRRegister::FUInput);
	SMB->addFanin(WriteEn);
}

SIRMemoryBank *SIRCtrlRgnBuilder::createMemoryBank(unsigned BusNum, unsigned AddrSize,
	                                                 unsigned DataSize, unsigned ReadLatency) {
	SIRMemoryBank *SMB = new SIRMemoryBank(BusNum, AddrSize, DataSize, ReadLatency);

	return SMB;
}

void SIRCtrlRgnBuilder::createMemoryTransaction(Value *Addr, Value *Data,
	                                              SIRMemoryBank *Bank, Instruction &I) {
	// Get ParentBB of this instruction.
	BasicBlock *ParentBB = I.getParent();
	// Get the slot.
	SIRSlot *Slot = SM->getLatestSlot(ParentBB);

	/// Handle the address pin.
	// Clamp the address width, to the address width of the memory bank.
	Type *RetTy = SM->createIntegerType(Bank->getAddrWidth());
	Value *AddrVal = D_Builder.createSBitExtractInst(Addr, Bank->getAddrWidth(), 0, RetTy, &I, true);
	
	assignToReg(Slot, SM->createIntegerValue(1, 1), AddrVal, Bank->getAddr());

	/// Handle the data pin and write enable pin.
	// If Data != NULL, then this intruction is writing to memory.
	if (Data) {
		assert(getBitWidth(Data) <= Bank->getDataWidth() && "Unexpected data width!");

		Type *RetTy = SM->createIntegerType(Bank->getDataWidth());
		Value *DataVal = D_Builder.createSZExtInstOrSelf(Data, Bank->getDataWidth(), RetTy, &I, true);

		// Handle the data pin.
		assignToReg(Slot, SM->createIntegerValue(1, 1), DataVal, Bank->getWData());
		// Handle the write enable pin.
		assignToReg(Slot, SM->createIntegerValue(1, 1), SM->createIntegerValue(1, 1), Bank->getWriteEnable());
	} 
	// If Data == NULL, then this intruction is reading from memory.
	else {
		// According the read latency, advance to the slot
		// that we can get the RData.
		unsigned Latency = Bank->getReadLatency();
		Slot = advanceToNextSlot(Slot, Latency);

		
	}

	/// Handle the enable pin.
	assignToReg(Slot, SM->createIntegerValue(1, 1), SM->createIntegerValue(1, 1), Bank->getEnable());


}

SIRSlot *SIRCtrlRgnBuilder::createSlot(BasicBlock *ParentBB, unsigned Schedule) {
  // To be noted that, the SlotNum is decided by the creating order,
  // so it has no connection with the state transition order.
  unsigned SlotNum = SM->getSlotsSize();
  // Create a slot register for this slot.
  std::string Name = "Slot" + utostr_32(SlotNum) + "r";
  // If the slot is start slot, the InitVal should be 1.
  unsigned InitVal = !SlotNum ? 1: 0;
  SIRRegister *SlotGuardReg = createRegister(Name, 1, ParentBB, 0, InitVal,
                                             SIRRegister::SlotReg);

  SIRSlot *S = new SIRSlot(SlotNum, ParentBB, SlotGuardReg->getLLVMValue(),
                           SlotGuardReg, Schedule);

  // Store the slot.
  SM->IndexSlot(S);

  return S;  
}

SIRSlot *SIRCtrlRgnBuilder::getOrCreateLandingSlot(BasicBlock *BB) {
  // Get the landing slot if it is already created.
	typedef std::pair<SIRSlot *, SIRSlot *> slot_pair;
  std::map<BasicBlock *, slot_pair> BB2SlotMap 
    = SM->getBB2SlotMap();

  std::map<BasicBlock*, slot_pair>::const_iterator
    at = BB2SlotMap.find(BB);  

  if (at == BB2SlotMap.end()) { 
    // Create the landing slot and latest slot for this BB.
    SIRSlot *S = createSlot(BB, 0);
    SM->IndexBB2Slots(BB, S, S);

    return S;
  }
  
  slot_pair &Slots = BB2SlotMap[BB];
  return Slots.first;
}

SIRSlot *SIRCtrlRgnBuilder::advanceToNextSlot(SIRSlot *CurSlot) {
	BasicBlock *BB = CurSlot->getParent();
	SIRSlot *Slot = SM->getLatestSlot(BB);

	assert(Slot == CurSlot && "CurSlot is not the last slot in BB!");
	assert(CurSlot->succ_empty() && "CurSlot already have successors!");

	// Create the next slot.
	SIRSlot *NextSlot = createSlot(BB, 0);

	// Connect the slots.
	CurSlot->addSuccSlot(NextSlot, SIRSlot::Sucessor, SM->createIntegerValue(1, 1));
	// Index the new latest slot to BB.
	SM->IndexBB2Slots(BB, SM->getLandingSlot(BB), NextSlot);

	return NextSlot;
}

SIRSlot *SIRCtrlRgnBuilder::advanceToNextSlot(SIRSlot *CurSlot, unsigned NumSlots) {
	SIRSlot *S = CurSlot;

	for (unsigned i = 0; i < NumSlots; ++i)
		S = advanceToNextSlot(S);

	return S;
}

void SIRCtrlRgnBuilder::createConditionalTransition(BasicBlock *DstBB,
                                                    SIRSlot *SrcSlot, Value *Guard) {
  SIRSlot *DstSlot = getOrCreateLandingSlot(DstBB);
  createStateTransition(SrcSlot, DstSlot, Guard);
  // The assignment is still be launched in SrcSlot, which lead to
  // a state transition to landing slot of DstBB.
  visitPHIsInSucc(SrcSlot, DstSlot, Guard, SrcSlot->getParent());
}

void SIRCtrlRgnBuilder::visitPHIsInSucc(SIRSlot *SrcSlot, SIRSlot *DstSlot,
                                        Value *Guard, BasicBlock *SrcBB) {
  BasicBlock *DstBB = DstSlot->getParent();
  assert(DstBB && "Unexpected null BB!");

  typedef BasicBlock::iterator iterator;
  for (iterator I = DstBB->begin(), E = DstBB->getFirstNonPHI(); I != E; ++I) {
    PHINode *PN = cast<PHINode>(I);
    unsigned BitWidth = TD.getTypeSizeInBits(PN->getType());

    // The value which lives out from SrcBB to DstBB;
    Value *LiveOutedFromSrcBB = PN->DoPHITranslation(DstBB, SrcBB);

    // Hack: we don't have methods to identify which BB is unreachable.
    // If the SrcBB cannot be reached, then the LiveOut will be zero.
    Value *LiveOut = LiveOutedFromSrcBB;
    
    // If the register already exist, then just assign to it.
    SIRRegister *PHISeqValReg = SM->lookupSIRReg(I);
    if (!PHISeqValReg) 
      PHISeqValReg = createRegister(PN->getName(), BitWidth, DstBB, I);

		// Make the assignment in SrcSlot, so we can schedule it in the latest slot
		// of SrcBB.
    assignToReg(SrcSlot, Guard, LiveOut, PHISeqValReg);   
  }
}

void SIRCtrlRgnBuilder::createStateTransition(SIRSlot *SrcSlot, SIRSlot *DstSlot, Value *Cnd) {
  assert(!SrcSlot->hasNextSlot(DstSlot) && "Edge already existed!");

  SrcSlot->addSuccSlot(DstSlot, SIRSlot::Sucessor, Cnd);
}

void SIRCtrlRgnBuilder::assignToReg(SIRSlot *S, Value *Guard, Value *Src,
                                    SIRRegister *Dst) {  
  SIRSeqOp *SeqOp = new SIRSeqOp(Src, Dst, Guard, S);
	// Add this SeqOp to the lists in SIRSlot.
	S->addSeqOp(SeqOp);

  SM->IndexSeqOp(SeqOp);
}

/// Functions to visit all control-path instructions

void SIRCtrlRgnBuilder::visitBranchInst(BranchInst &I) {
  SIRSlot *CurSlot = SM->getLatestSlot(I.getParent());
  // Treat unconditional branch differently.
  if (I.isUnconditional()) {
    BasicBlock *DstBB = I.getSuccessor(0);
    createConditionalTransition(DstBB, CurSlot, SM->creatConstantBoolean(true));
    return;
  }

  // Connect the slots according to the condition.
  Value *Cnd = I.getCondition();

  BasicBlock *TrueBB = I.getSuccessor(0);
  createConditionalTransition(TrueBB, CurSlot, Cnd);

  BasicBlock *FalseBB = I.getSuccessor(1);
  createConditionalTransition(FalseBB, CurSlot, D_Builder.createSNotInst(Cnd, Cnd->getType(), &I, true));
}

// Copy from LowerSwitch.cpp.
namespace {
struct CaseRange {
	APInt Low;
	APInt High;
	BasicBlock* BB;

	CaseRange(APInt low = APInt(), APInt high = APInt(), BasicBlock *bb = 0) :
		Low(low), High(high), BB(bb) { }

	};

typedef std::vector<CaseRange>           CaseVector;
typedef std::vector<CaseRange>::iterator CaseItr;

// The comparison function for sorting the switch case values in the vector.
struct CaseCmp {
	bool operator () (const CaseRange& C1, const CaseRange& C2) {
		return C1.Low.slt(C2.High);
	}
};}

// Clusterify - Transform simple list of Cases into list of CaseRange's
static unsigned Clusterify(CaseVector& Cases, SwitchInst *SI) {
	unsigned numCmps = 0;

	// Start with "simple" cases
	for (SwitchInst::CaseIt i = SI->case_begin(), e = SI->case_end(); i != e; ++i)
		Cases.push_back(CaseRange(i.getCaseValue()->getValue(),
		                          i.getCaseValue()->getValue(),
		                          i.getCaseSuccessor()));

	std::sort(Cases.begin(), Cases.end(), CaseCmp());

	// Merge case into clusters
	if (Cases.size() >= 2) {
		for (CaseItr I = Cases.begin(), J = llvm::next(Cases.begin()); J != Cases.end(); ) {
			int64_t nextValue = J->Low.getSExtValue();
			int64_t currentValue = I->High.getSExtValue();
			BasicBlock* nextBB = J->BB;
			BasicBlock* currentBB = I->BB;

			// If the two neighboring cases go to the same destination, merge them
			// into a single case.
			if ((nextValue-currentValue == 1) && (currentBB == nextBB)) {
				I->High = J->High;
				J = Cases.erase(J);
			} else {
				I = J++;
			}
		}
	}

	for (CaseItr I = Cases.begin(), E = Cases.end(); I != E; ++I, ++numCmps) {
		if (I->Low != I->High)
			// A range counts double, since it requires two compares.
			++numCmps;
	}

	return numCmps;
}

void SIRCtrlRgnBuilder::visitSwitchInst(SwitchInst &I) {
	SIRSlot *CurSlot = SM->getLatestSlot(I.getParent());

	// The Condition Value
	Value *CndVal = I.getCondition();
	// The Case Map
	std::map<BasicBlock *, Value *> CaseMap;

	// Prepare cases vector.
	CaseVector Cases;
	Clusterify(Cases, &I);

	// Build the condition map.
	for (CaseItr CI = Cases.begin(), CE = Cases.end(); CI != CE; ++CI) {
		const CaseRange &Case = *CI;
		// Simple case, test if the CndVal is equal to a specific value.
		if (Case.High == Case.Low) {
			Value *CaseVal = SM->createIntegerValue(Case.High);
			Value *Pred = D_Builder.createSEQInst(CndVal, CaseVal, SM->createIntegerType(1),
				                                    &I, true);
			Value *&BBPred = CaseMap[Case.BB];
			if (!BBPred) BBPred = Pred;
			else D_Builder.createSOrEqualInst(BBPred, Pred, BBPred->getType(), &I, true);

			continue;
	}

  // Test if Low <= CndVal <= High.
	Value *Low = SM->createIntegerValue(Case.Low);
	Value *LowCmp = D_Builder.createSIcmpOrEqInst(CmpInst::ICMP_UGT, CndVal, Low,
		                                            SM->createIntegerType(1), &I, true);
	Value *High = SM->createIntegerValue(Case.High);
	Value *HighCmp = D_Builder.createSIcmpOrEqInst(CmpInst::ICMP_UGT, High, CndVal,
		                                             SM->createIntegerType(1), &I, true);
	Value *Pred = D_Builder.createSAndInst(LowCmp, HighCmp, LowCmp->getType(), &I, true);
	Value *&BBPred = CaseMap[Case.BB];
	if (!BBPred) BBPred = Pred;
	else D_Builder.createSOrEqualInst(BBPred, Pred, BBPred->getType(), &I, true);
	}

	// The predicate for each non-default destination.
	SmallVector<Value *, 4> CasePreds;
	typedef std::map<BasicBlock*, Value *>::iterator CaseIt;
	for (CaseIt CI = CaseMap.begin(), CE = CaseMap.end(); CI != CE; ++CI) {
		BasicBlock *SuccBB = CI->first;
		Value *Pred = CI->second;
		CasePreds.push_back(Pred);

		createConditionalTransition(SuccBB, CurSlot, Pred);
	}

	// Jump to the default block when all the case value not match, i.e. all case
	// predicate is false.
	Value *DefaultPred 
		= D_Builder.createSNotInst(D_Builder.createSOrInst(CasePreds, SM->createIntegerType(1),
		                                                   &I, true),
															 SM->createIntegerType(1), &I, true);
	BasicBlock *DefBB = I.getDefaultDest();

	createConditionalTransition(DefBB, CurSlot, DefaultPred);
}

void SIRCtrlRgnBuilder::visitReturnInst(ReturnInst &I) {
  // Get the latest slot of CurBB.
  SIRSlot *CurSlot = SM->getLatestSlot(I.getParent());

  // Jump back to the start slot on return.
  createStateTransition(CurSlot, SM->getStartSlot(), SM->creatConstantBoolean(true));

  if (I.getNumOperands()) {
    SIRRegister *Reg = cast<SIROutPort>(SM->getRetPort())->getRegister();
    
    // Launch the instruction to assignment value to register.
		// Here we should know that the Ret instruction different from others:
		// The module may have mutil-RetInst, but only one Ret Register.
    assignToReg(CurSlot, SM->creatConstantBoolean(true),
                I.getReturnValue(), Reg);

		// Replace the Ret operand with the RegVal. So all Ret-instruction
		// will return the RetRegVal in the corresponding slot.
		I.setOperand(0, Reg->getLLVMValue());

    // Index the register with return instruction.
    SM->IndexSeqInst2Reg(&I, Reg);
  }
}



/// Functions to provide basic information of instruction

unsigned SIRDatapathBuilder::getBitWidth(Value *U) {
  // Since we create a pseudo instruction for these slots and ports register,
  // so we should handle these pseudo instruction differently when we meet them.
  if(!U) {
    SIRRegister *Reg = SM->lookupSIRReg(dyn_cast<Instruction>(U));
    assert(Reg && (Reg->isOutPort() || Reg->isSlot()) && "Unexpected Null Value!");

    return Reg->getBitWidth();
  }

  Type *Ty = U->getType();
  unsigned BitWidth = TD.getTypeSizeInBits(Ty);
  return BitWidth;
}

/// Functions to visit all data-path instructions

void SIRDatapathBuilder::visitTruncInst(TruncInst &I) {
  createSTruncInst(I.getOperand(0), getBitWidth(&I), 0,
                   I.getType(), &I, false);
}

void SIRDatapathBuilder::visitZExtInst(ZExtInst &I) {
  unsigned NumBits = getBitWidth(&I);
  createSZExtInst(I.getOperand(0), NumBits, I.getType(), &I, false);
}

void SIRDatapathBuilder::visitSExtInst(SExtInst &I) {
  unsigned NumBits = getBitWidth(&I);
  createSSExtInst(I.getOperand(0), NumBits, I.getType(), &I, false);
}

void SIRDatapathBuilder::visitPtrToIntInst(PtrToIntInst &I) {
  createSCastInst(I.getPointerOperand(), I.getType(), &I, false);
}

void SIRDatapathBuilder::visitIntToPtrInst(IntToPtrInst &I) {
  createSCastInst(I.getOperand(0), I.getType(), &I, false);
}

void SIRDatapathBuilder::visitBitCastInst(BitCastInst &I) {
  createSCastInst(I.getOperand(0), I.getType(), &I, false);
}

void SIRDatapathBuilder::visitSelectInst(SelectInst &I) {
  createSSelInst(I.getOperand(0), I.getOperand(1),
                 I.getOperand(2), I.getType(), &I, false);
}

void SIRDatapathBuilder::visitICmpInst(ICmpInst &I) {
  Value *Ops[] = {I.getOperand(0), I.getOperand(1)};
  createSICmpInst(I.getPredicate(), Ops, I.getType(), &I, false);
}

void SIRDatapathBuilder::visitBinaryOperator(BinaryOperator &I) {
  Value *Ops[] = {I.getOperand(0), I.getOperand(1)};
	Type *RetTy = I.getType();

  switch (I.getOpcode()) {
  case Instruction::Add: createSAddInst(Ops, RetTy, &I, false); return;
  case Instruction::Sub: createSSubInst(Ops, RetTy, &I, false); return;
  case Instruction::Mul: createSMulInst(Ops, RetTy, &I, false); return;

  case Instruction::Shl: createSShiftInst(Ops, RetTy, &I, Intrinsic::shang_shl, false); return;
  case Instruction::AShr: createSShiftInst(Ops, RetTy, &I, Intrinsic::shang_ashr, false); return;
  case Instruction::LShr: createSShiftInst(Ops, RetTy, &I, Intrinsic::shang_lshr, false); return;

  case Instruction::UDiv: createSUDivInst(Ops, RetTy, &I, false); return;
  case Instruction::SDiv: createSSDivInst(Ops, RetTy, &I, false); return;

  case Instruction::And: createSAndInst(Ops, RetTy, &I, false); return;
  case Instruction::Or:  createSOrInst(Ops, RetTy, &I, false); return;
  case Instruction::Xor: createSXorInst(Ops, RetTy, &I, false); return;

  default: llvm_unreachable("Unexpected opcode"); break;
  }

  return;
}

void SIRDatapathBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  return visitGEPOperator(cast<GEPOperator>(I), I);
}

void SIRDatapathBuilder::visitGEPOperator(GEPOperator &O, GetElementPtrInst &I) {
  //assert(O.getPointerOperand()->getType() == O.getType() && "Type not match!");
  Value *Ptr = O.getPointerOperand();

  // Cast the Ptr into int type so we can do the math operation below.
  Value *PtrVal = new PtrToIntInst(Ptr, IntegerType::getInt32Ty(I.getContext()),
    "SIRPtrToInt", &I);

  //Value *Test = cast<>
  // FIXME: All the pointer arithmetic are perform under the precision of
  // PtrSize, do we need to perform the arithmetic at the max available integer
  // width and truncate the result?
  unsigned PtrSize = getBitWidth(O.getPointerOperand());
  // Note that the pointer operand may be a vector of pointers. Take the scalar
  // element which holds a pointer.
  Type *Ty = O.getPointerOperandType()->getScalarType();

  typedef GEPOperator::op_iterator op_iterator;
  for (op_iterator OI = O.idx_begin(), E = O.op_end(); OI != E; ++OI) {
    Value *Idx = *OI;
    if (StructType *StTy = dyn_cast<StructType>(Ty)) {
      unsigned Field = getConstantIntValue(Idx);
      if (Field) {
        // N = N + Offset
        uint64_t Offset = TD.getStructLayout(StTy)->getElementOffset(Field);
        PtrVal = createSAddInst(PtrVal, createSConstantInt(Offset, PtrSize),
                                PtrVal->getType(), &I, true);
      }

      Ty = StTy->getElementType(Field);
    } else {
      Ty = cast<SequentialType>(Ty)->getElementType();

      // If this is a constant subscript, handle it quickly.
      if (const ConstantInt *CI = dyn_cast<ConstantInt>(Idx)) {
        if (CI->isZero()) continue;
        uint64_t Offs = TD.getTypeAllocSize(Ty)
                        * cast<ConstantInt>(CI)->getSExtValue();
        PtrVal = createSAddInst(PtrVal, createSConstantInt(Offs, PtrSize),
                                PtrVal->getType(), &I, true);
        continue;
      }

      // N = N + Idx * ElementSize;
      APInt ElementSize = APInt(PtrSize, TD.getTypeAllocSize(Ty));
      Value *IdxN = const_cast<Value*>(Idx);

      // If the index is smaller or larger than intptr_t, truncate or extend
      // it.
      IdxN = createSBitExtractInst(IdxN, PtrSize, 0, SM->createIntegerType(PtrSize), &I, true);

      // If this is a multiply by a power of two, turn it into a shl
      // immediately.  This is a very common case.
      if (ElementSize != 1) {
        if (ElementSize.isPowerOf2()) {
          unsigned Amt = ElementSize.logBase2();
          IdxN = createSShiftInst(IdxN, createSConstantInt(Amt, PtrSize),
                                  IdxN->getType(), &I, Intrinsic::shang_shl, true);
        } else {
          Value *Scale = createSConstantInt(ElementSize);
          IdxN = createSMulInst(IdxN, Scale, IdxN->getType(), &I, true);
        }
      }

      PtrVal = createSAddInst(PtrVal, IdxN, PtrVal->getType(), &I, true);
    }
  }

  Value *PtrResult = new IntToPtrInst(PtrVal, I.getType(), "SIRIntToPtr", &I);
  I.replaceAllUsesWith(PtrResult);
}

/// Functions to create Shang-Inst

Value *SIRDatapathBuilder::createShangInstPattern(ArrayRef<Value *> Ops, Type *RetTy,
                                                  Value *InsertPosition, Intrinsic::ID FuncID,
                                                  bool UsedAsArg) {
  // Insert a correctly-typed definition now.
  std::vector<Type *> FuncTy;
  // The return type
  FuncTy.push_back(RetTy);

  // The operand type
  for (unsigned i = 0; i < Ops.size(); ++i) {
    FuncTy.push_back(Ops[i]->getType());
  }  

  Module *M = SM->getModule();
  Value *Func = Intrinsic::getDeclaration(M, FuncID, FuncTy);

  if (Instruction *InsertBefore = dyn_cast<Instruction>(InsertPosition)) {
    Instruction *NewInst = CallInst::Create(Func, Ops, Func->getName(), InsertBefore);

    // Index all these data-path instructions.
    if (FuncID != Intrinsic::shang_pseudo)
      SM->IndexDataPathInst(NewInst);

    // If the inst is not used as an argument of other functions,
    // then it is used to replace the inst in IR
    if (!UsedAsArg) InsertBefore->replaceAllUsesWith(NewInst);
    return NewInst;
  }
  else if (BasicBlock *InsertAtEnd = dyn_cast<BasicBlock>(InsertPosition)) {
    Instruction *NewInst = CallInst::Create(Func, Ops, Func->getName(), InsertAtEnd);

    // Index all these data-path instructions.
    if (FuncID != Intrinsic::shang_pseudo)
      SM->IndexDataPathInst(NewInst);

    return NewInst;
  } 

  assert(false && "Unexpected InsertPosition!");
}

Value *SIRDatapathBuilder::createSBitCatInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                           Value *InsertPosition, bool UsedAsArg) {
  return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_bit_cat, UsedAsArg);
}

Value *SIRDatapathBuilder::createSBitExtractInst(Value *U, unsigned UB, unsigned LB, Type *RetTy,
                                                 Value *InsertPosition, bool UsedAsArg) {
  Value *Ops[] = {U, createSConstantInt(UB, 8), createSConstantInt(LB, 8)};

  assert(TD.getTypeSizeInBits(RetTy) == UB - LB && "RetTy not matches!");

  return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_bit_extract, UsedAsArg);
}

Value *SIRDatapathBuilder::createSCastInst(Value *U, Type *RetTy,
	                                         Value *InsertPosition, bool UsedAsArg) {
  if (!UsedAsArg) {
    assert((!U || getBitWidth(U) == getBitWidth(InsertPosition))
      && "Cast between types with different size found!");
    InsertPosition->replaceAllUsesWith(U);
  }
  return U;
}

Value *SIRDatapathBuilder::createSTruncInst(Value *U, unsigned UB, unsigned LB, Type *RetTy,
                                            Value *InsertPosition, bool UsedAsArg) {
  // Truncate the value by bit-extract expression.
  return createSBitExtractInst(U, UB, LB, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSZExtInst(Value *U, unsigned DstBitWidth, Type *RetTy,
	                                         Value *InsertPosition, bool UsedAsArg) {
  unsigned SrcBitWidth = getBitWidth(U);
  assert(DstBitWidth > SrcBitWidth && "Unexpected DstBitWidth!");
  unsigned NumExtendBits = DstBitWidth - SrcBitWidth;
  Value *Zero = createSConstantInt(0, 1);

  Value *ExtendBits = createSBitRepeatInst(Zero, NumExtendBits, SM->createIntegerType(NumExtendBits),
		                                       InsertPosition, true);
  Value *Ops[] = { ExtendBits, U };
  return createSBitCatInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSZExtInstOrSelf(Value *U, unsigned DstBitWidth, Type *RetTy,
	                                               Value *InsertPosition, bool UsedAsArg) {
  if (getBitWidth(U) < DstBitWidth)
    return createSZExtInst(U, DstBitWidth, RetTy, InsertPosition, UsedAsArg);
  else if (!UsedAsArg) InsertPosition->replaceAllUsesWith(U);

  return U;
}

Value *SIRDatapathBuilder::createSSExtInst(Value *U, unsigned DstBitWidth, Type *RetTy,
	                                         Value *InsertPosition, bool UsedAsArg) {
  unsigned SrcBitWidth = getBitWidth(U);
  assert(DstBitWidth > SrcBitWidth && "Unexpected DstBitWidth!");
  unsigned NumExtendBits = DstBitWidth - SrcBitWidth;
  Value *SignBit = getSignBit(U, InsertPosition);

  Value *ExtendBits = createSBitRepeatInst(SignBit, NumExtendBits, SM->createIntegerType(NumExtendBits),
		                                       InsertPosition, true);
  Value *Ops[] = { ExtendBits, U };
  return createSBitCatInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSExtInstOrSelf(Value *U, unsigned DstBitWidth, Type *RetTy,
	                                               Value *InsertPosition, bool UsedAsArg) {
  if (getBitWidth(U) < DstBitWidth)
    return createSSExtInst(U, DstBitWidth, RetTy, InsertPosition, UsedAsArg);
  else if (!UsedAsArg) InsertPosition->replaceAllUsesWith(U);

  return U;
}

Value *SIRDatapathBuilder::createSRAndInst(Value *U, Type *RetTy,
	                                         Value *InsertPosition, bool UsedAsArg) {
	assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");

  return createShangInstPattern(U, RetTy, InsertPosition, Intrinsic::shang_rand, UsedAsArg);
}

Value *SIRDatapathBuilder::createSRXorInst(Value *U, Type *RetTy,
	                                         Value *InsertPosition, bool UsedAsArg) {
  assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");

  return createShangInstPattern(U, RetTy, InsertPosition, Intrinsic::shang_rxor, UsedAsArg);
}

Value *SIRDatapathBuilder::createSROrInst(Value *U, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
  // A | B .. | Z = ~(~A & ~B ... & ~Z).
  Value *NotU = createSNotInst(U, U->getType(), InsertPosition, true);
  return createSNotInst(createSRAndInst(NotU, RetTy, InsertPosition, true), RetTy,
                        InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSNEInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                       Value *InsertPosition, bool UsedAsArg) {
	assert(getBitWidth(Ops[0]) == getBitWidth(Ops[1]) == TD.getTypeSizeInBits(RetTy)
		     && "RetTy not matches!");

  // Get the bitwise difference by Xor.
  Value *BitWissDiff = createSXorInst(Ops, Ops[0]->getType(), InsertPosition, true);
  // If there is any bitwise difference, then LHS and RHS is not equal.
  return createSROrInst(BitWissDiff, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSEQInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                       Value *InsertPosition, bool UsedAsArg) {
  return createSNotInst(createSNEInst(Ops, RetTy, InsertPosition, true),
                        RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSEQInst(Value *LHS, Value *RHS, Type *RetTy,
	                                       Value *InsertPosition, bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSEQInst(Ops, RetTy, InsertPosition, true);
}

Value *SIRDatapathBuilder::createSdpSGTInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                          Value *InsertPosition, bool UsedAsArg) {
  assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");

  return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_sgt, UsedAsArg);
}

Value *SIRDatapathBuilder::createSdpUGTInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                          Value *InsertPosition, bool UsedAsArg) {
  assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");

  return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_ugt, UsedAsArg);
}

Value *SIRDatapathBuilder::createSBitRepeatInst(Value *U, unsigned RepeatTimes, Type *RetTy,
	                                              Value *InsertPosition, bool UsedAsArg) {
  if (RepeatTimes == 1) {
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(U);
    return U;
  }

  assert(TD.getTypeSizeInBits(RetTy) == getBitWidth(U) * RepeatTimes && "RetTy not matches!");
  Value *Ops[] = { U, createSConstantInt(RepeatTimes, 32)};
  return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_bit_repeat, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSelInst(Value *Cnd, Value *TrueV, Value *FalseV,
                                          Type *RetTy, Value *InsertPosition, bool UsedAsArg) {
  assert(getBitWidth(Cnd) == 1 && "Bad condition width!");

  unsigned Bitwidth = getBitWidth(TrueV);
  assert(TD.getTypeSizeInBits(RetTy) == getBitWidth(FalseV) == Bitwidth && "Bad Bitwidth!");

  if (ConstantInt *C = dyn_cast<ConstantInt>(Cnd)) {
    Value *Result = C->getValue().getBoolValue() ? TrueV : FalseV; 
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Result);
    return Result;
  }

  Value *NewCnd = createSBitRepeatInst(Cnd, Bitwidth, RetTy, InsertPosition, true);
  return createSOrInst(createSAndInst(NewCnd, TrueV, RetTy, InsertPosition, true),
                       createSAndInst(createSNotInst(NewCnd, RetTy, InsertPosition, true),
                                      FalseV, RetTy, InsertPosition, true),
                       RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSIcmpOrEqInst(ICmpInst::Predicate Predicate, ArrayRef<Value *> Ops,
                                               Type *RetTy, Value *InsertPosition, bool UsedAsArg) {
	assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");  

  Value *ICmpResult = createSICmpInst(Predicate, Ops, RetTy, InsertPosition, true);
  Value *EQResult = createSEQInst(Ops, RetTy, InsertPosition, true);
  Value *NewOps[] = {EQResult, ICmpResult};
  return createSOrInst(NewOps, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSIcmpOrEqInst(ICmpInst::Predicate Predicate, Value *LHS, Value *RHS,
                                               Type *RetTy, Value *InsertPosition, bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSIcmpOrEqInst(Predicate, Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSICmpInst(ICmpInst::Predicate Predicate, ArrayRef<Value *> Ops,
	                                         Type *RetTy, Value *InsertPosition, bool UsedAsArg) {
  assert(Ops.size() == 2 && "There must be two operands!");
  assert(getBitWidth(Ops[0]) == getBitWidth(Ops[1]) && "Bad icmp bitwidth!");
	assert(TD.getTypeSizeInBits(RetTy) == 1 && "RetTy not matches!");
  SmallVector<Value *, 2> NewOps;
  for (int i = 0; i < Ops.size(); i++)
    NewOps.push_back(Ops[i]);

  switch (Predicate) {
  case CmpInst::ICMP_NE:
    return createSNEInst(NewOps, RetTy, InsertPosition, UsedAsArg);
  case CmpInst::ICMP_EQ:
    return createSEQInst(NewOps, RetTy, InsertPosition, UsedAsArg);

  case CmpInst::ICMP_SLT:
    std::swap(NewOps[0], NewOps[1]);
    // Fall though.
  case CmpInst::ICMP_SGT:
    return createSdpSGTInst(NewOps, RetTy, InsertPosition, UsedAsArg);

  case CmpInst::ICMP_ULT:
    std::swap(NewOps[0], NewOps[1]);
    // Fall though.
  case CmpInst::ICMP_UGT:
    return createSdpUGTInst(NewOps, RetTy, InsertPosition, UsedAsArg);

  case CmpInst::ICMP_SLE:
    std::swap(NewOps[0], NewOps[1]);
    // Fall though.
  case CmpInst::ICMP_SGE:
    return createSIcmpOrEqInst(CmpInst::ICMP_SGT, NewOps, RetTy, InsertPosition, UsedAsArg);
    //return buildICmpOrEqExpr(VASTExpr::dpSGT, LHS, RHS);

  case CmpInst::ICMP_ULE:
    std::swap(NewOps[0], NewOps[1]);
    // Fall though.
  case CmpInst::ICMP_UGE:
    return createSIcmpOrEqInst(CmpInst::ICMP_UGT, NewOps, RetTy, InsertPosition, UsedAsArg);

  default: break;
  }

  llvm_unreachable("Unexpected ICmp predicate!");
  return 0;
}

Value *SIRDatapathBuilder::createSNotInst(Value *U, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
//   // If the instruction is like: A = ~(~B), then we can simplify it.
  IntrinsicInst *Inst = dyn_cast<IntrinsicInst>(U);
	if(Inst && Inst->getIntrinsicID() == Intrinsic::shang_not) {
		Value *Operand = Inst->getOperand(0);

		// If the inst is not used as an argument of other functions,
		// then it is used to replace the inst in IR
		if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Operand);
		return Operand;
	}

	assert(U->getType() == RetTy && "RetTy not matches!");

  return createShangInstPattern(U, RetTy, InsertPosition, Intrinsic::shang_not, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAddInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
	assert(getBitWidth(Ops[0]) == getBitWidth(Ops[1]) && "BitWidth not matches!");
	// If the operands size is 3, then it should be the shang_addc.
  if (Ops.size() == 3) {
	  assert(TD.getTypeSizeInBits(Ops[2]->getType()) == 1 && "Bad BitWidth of Carry!");
		return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_addc, UsedAsArg);
	}			
  assert(Ops.size() == 2 && "Bad operands size!");
	return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_add, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAddInst(Value *LHS, Value *RHS, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
  Value *Ops[] = { LHS, RHS };
  return createSAddInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAddInst(Value *LHS, Value *RHS, Value *Carry, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
	Value *Ops[] = { LHS, RHS, Carry };
	return createSAddInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSubInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
  Value *NewOps[] = { Ops[0],
                      createSNotInst(Ops[1], Ops[1]->getType(), InsertPosition, true),
                      creatConstantBoolean(true) };
  return createSAddInst(NewOps, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSMulInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1) {
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Ops[0]);
    return Ops[0];
  }

  return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_mul, UsedAsArg);
}

Value *SIRDatapathBuilder::createSMulInst(Value *LHS, Value *RHS, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS};
    return createSMulInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSUDivInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                         Value *InsertPosition, bool UsedAsArg) {
  return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_udiv, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSDivInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                         Value *InsertPosition, bool UsedAsArg) {
  return createShangInstPattern(Ops, RetTy, InsertPosition, Intrinsic::shang_sdiv, UsedAsArg);
}

Value *SIRDatapathBuilder::createSShiftInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                          Value *InsertPosition, Intrinsic::ID FuncID,
																						bool UsedAsArg) {
  assert(Ops.size() == 2 && "The shift inst must have two operands!");
  Value *LHS = Ops[0]; Value *RHS = Ops[1];
  // Limit the shift amount so keep the behavior of the hardware the same as
  // the corresponding software.
  unsigned RHSMaxSize = Log2_32_Ceil(getBitWidth(LHS));
  if (getBitWidth(RHS) > RHSMaxSize) 
    RHS = createSBitExtractInst(Ops[1], RHSMaxSize, 0, SM->createIntegerType(RHSMaxSize), 
		                            InsertPosition, true);  
  Value *NewOps[] = {LHS, RHS};

	assert(LHS->getType() == RetTy && "RetTy not matches!");
  return createShangInstPattern(NewOps, RetTy, InsertPosition, FuncID, UsedAsArg);
}

Value *SIRDatapathBuilder::createSShiftInst(Value *LHS, Value *RHS, Type *RetTy,
	                                          Value *InsertPosition, Intrinsic::ID FuncID,
																						bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS };
    return createSShiftInst(Ops, RetTy, InsertPosition, FuncID, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAndInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
	for (int i = 0; i < Ops.size(); i++) {
		assert(Ops[0]->getType() == RetTy && "RetTy not matches!");
	}

  // Handle the trivial case trivially.
  if (Ops.size() == 1) {
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Ops[0]);
    return Ops[0];
  }

	// If the instruction is like: 
	// 1) A = 1'b1 & B & C,
	// 2) A = (~1'b1) & B & C,
	// then we can simplify it.
	bool hasOneValue = false;
	SmallVector<Value *, 4> NewOps;
	typedef ArrayRef<Value *>::iterator iterator;
	for (iterator I = Ops.begin(), E = Ops.end(); I != E; I++) {
		Value *Operand = *I;

		// 1) A = 1'b1 & B & C
		ConstantInt *CI = dyn_cast<ConstantInt>(Operand);
		if (CI && getConstantIntValue(CI) == 1) {
			hasOneValue = true;
			continue;
		}

		// 2) A = (~1'b1) & B & C
		IntrinsicInst *II = dyn_cast<IntrinsicInst>(Operand);
		if (II && II->getIntrinsicID() == Intrinsic::shang_not) {
			Value *NotInstOperand = II->getOperand(0);
			ConstantInt *CI = dyn_cast<ConstantInt>(NotInstOperand);
			if (CI && getConstantIntValue(CI) == 1) {
				// If the inst is not used as an argument of other functions,
				// then it is used to replace the inst in IR
				if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Operand);
				return Operand;
			}
		}

		NewOps.push_back(Operand);
	}

	// If all operand are removed, then they all are 1'b1;
	if (NewOps.size() == 0) return SM->creatConstantBoolean(true);

	if (hasOneValue)
		return createSAndInst(NewOps, RetTy, InsertPosition, UsedAsArg);
	else
		return createShangInstPattern(NewOps, RetTy, InsertPosition,
																	Intrinsic::shang_and, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAndInst(Value *LHS, Value *RHS, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
	assert(LHS->getType() == RHS->getType() && LHS->getType() == RetTy
		     && "BitWidth not match!");

  Value *Ops[] = {LHS, RHS};
  return createSAndInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                       Value *InsertPosition, bool UsedAsArg) {
  for (int i = 0; i < Ops.size(); i++)
		assert(Ops[i]->getType() == RetTy && "RetTy not matches!");

  // Handle the trivial case trivially.
  if (Ops.size() == 1) {
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(Ops[0]);
    return Ops[0];
  }

	// If there are more than two operands, transform it into mutil-SOrInst.
	if (Ops.size() > 2) {
		Value *TempSOrInst = createSOrInst(Ops[0], Ops[1], RetTy, InsertPosition, true);
		for (int i = 0; i < Ops.size() - 3; i++) {
			TempSOrInst = createSOrInst(TempSOrInst, Ops[i + 2], RetTy, InsertPosition, true);
		}

		int num = Ops.size();
		return createSOrInst(TempSOrInst, Ops[num - 1], RetTy, InsertPosition, UsedAsArg);
	}

  SmallVector<Value *, 8> NotInsts;
  // Build the operands of Or operation into not inst.
  for (unsigned i = 0; i < Ops.size(); ++i) 
    NotInsts.push_back(createSNotInst(Ops[i], RetTy, InsertPosition, true));

  // Build Or operation with the And Inverter Graph (AIG).
  Value *AndInst = createSAndInst(NotInsts, RetTy, InsertPosition, true);
  return createSNotInst(AndInst, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrInst(Value *LHS, Value *RHS, Type *RetTy,
	                                       Value *InsertPosition, bool UsedAsArg) {
	assert(LHS->getType() == RHS->getType() && LHS->getType() == RetTy
		     && "BitWidth not match!");	

  Value *Ops[] = {LHS, RHS};
  return createSOrInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSXorInst(ArrayRef<Value *> Ops, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
  assert (Ops.size() == 2 && "There should be more than one operand!!");

	for (int i = 0; i < Ops.size(); i++)
		assert(Ops[i]->getType() == RetTy && "RetTy not matches!");

  // Build the Xor Expr with the And Inverter Graph (AIG).
  Value *OrInst = createSOrInst(Ops, RetTy, InsertPosition, true);
  Value *AndInst = createSAndInst(Ops, RetTy, InsertPosition, true);
  Value *NotInst = createSNotInst(AndInst, RetTy, InsertPosition, true);

  Value *NewOps[] = {OrInst, NotInst};
  return createSAndInst(NewOps, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSXorInst(Value *LHS, Value *RHS, Type *RetTy,
	                                        Value *InsertPosition, bool UsedAsArg) {
  assert(LHS->getType() == RHS->getType() && LHS->getType() == RetTy
		     && "BitWidth not match!");

  Value *Ops[] = {LHS, RHS};
  return createSXorInst(Ops, RetTy, InsertPosition, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrEqualInst(Value *LHS, Value *RHS, Type *RetTy,
	                                            Value *InsertPosition, bool UsedAsArg) {
	assert(LHS->getType() == RHS->getType() && LHS->getType() == RetTy
		     && "BitWidth not match!");

  if (LHS == NULL) {
    if (!UsedAsArg) InsertPosition->replaceAllUsesWith(RHS);
    return (LHS = RHS);
  }

  return (LHS = createSOrInst(LHS, RHS, RetTy, InsertPosition, UsedAsArg));
}

/// Functions to help us create Shang-Inst.
Value *SIRDatapathBuilder::getSignBit(Value *U, Value *InsertPosition) {
  unsigned BitWidth = getBitWidth(U);
  return createSBitExtractInst(U, BitWidth, BitWidth - 1, SM->createIntegerType(1),
		                           InsertPosition, true);
}

Value *SIRDatapathBuilder::createSConstantInt(int16_t Value, unsigned BitWidth) {
  return SM->createIntegerValue(BitWidth, Value);
}

Value *SIRDatapathBuilder::createSConstantInt(const APInt &Value) {
	return SM->createIntegerValue(Value);
}

Value *SIRDatapathBuilder::creatConstantBoolean(bool True) {
	return SM->creatConstantBoolean(True);
}