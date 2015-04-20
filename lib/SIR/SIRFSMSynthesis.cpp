//===-------------------- SIRFSMSynthesis.cpp -------------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of the SIRFSMSynthesis pass, which synthesis the FSM in SIR.
//
//===----------------------------------------------------------------------===//

#include "sir/SIR.h"
#include "sir/SIRPass.h"
#include "sir/SIRBuild.h"
#include "sir/Passes.h"

using namespace llvm;

namespace llvm {
struct SIRFSMSynthesis : public SIRPass {
	static char ID;

	SIRFSMSynthesis() : SIRPass(ID) {
		initializeSIRFSMSynthesisPass(*PassRegistry::getPassRegistry());
	}

	bool runOnSIR(SIR &SM);

	void getAnalysisUsage(AnalysisUsage &AU) const;	

	bool synthesisStateTransistion(SIRSlot *SrcSlot, SIRSlot *DstSlot,
		                             Value *Cnd, SIR *SM, SIRDatapathBuilder &Builder);
};
}

char SIRFSMSynthesis::ID = 0;
char &llvm::SIRFSMSynthesisID = SIRFSMSynthesis::ID;

INITIALIZE_PASS_BEGIN(SIRFSMSynthesis,
	                    "SIR-FSM-synthesis",
	                    "Implement the FSM in SIR",
	                    false, true)
	INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(SIRFSMSynthesis,
	                  "SIR-FSM-synthesis",
	                  "Implement the FSM in SIR",
	                  false, true)

void SIRFSMSynthesis::getAnalysisUsage(AnalysisUsage &AU) const {
	SIRPass::getAnalysisUsage(AU);
	AU.addRequired<DataLayout>();
	AU.setPreservesAll();
}

bool SIRFSMSynthesis::runOnSIR(SIR &SM) {
	bool Changed = false;

	DataLayout &TD = getAnalysis<DataLayout>();

	// Initialize a SIRDatapathBuilder to build expression for guard and Fanin
	SIRDatapathBuilder Builder(&SM, TD);

	typedef SIR::slot_iterator slot_iterator;
	for (slot_iterator I = SM.slot_begin(), E = SM.slot_end(); I != E; ++I) {
		SIRSlot *SrcSlot = *I;

		assert(!SrcSlot->succ_empty() && "Successors should not be empty!");

		typedef SIRSlot::const_succ_iterator const_succ_iterator;
		for (const_succ_iterator I = SrcSlot->succ_begin(), E = SrcSlot->succ_end(); I != E; ++I) {
			SIRSlot *DstSlot = *I;
			Value *Cnd = I->getCnd();

			Changed |= synthesisStateTransistion(SrcSlot, DstSlot, Cnd, &SM, Builder);
		}
	}

	return Changed;
}

bool SIRFSMSynthesis::synthesisStateTransistion(SIRSlot *SrcSlot, SIRSlot *DstSlot,
	                                              Value *Cnd, SIR *SM, SIRDatapathBuilder &Builder) {
	SIRRegister *Reg = DstSlot->getSlotReg();

	assert(Builder.getBitWidth(Cnd) == 1 && "BitWidth not matches!");

	// Build the assignment condition.
	Value *Guard = Builder.createSAndInst(SrcSlot->getGuardValue(), Cnd,
		                                    Cnd->getType(), Reg->getLLVMValue(), true);

	// The State Transition is actually a SeqOp which assign a true/false value to
	// the Slot Reg in the right time to active DstSlot.
	Reg->addAssignment(SM->creatConstantBoolean(true), Guard);

	// Index the slot register to this slot and the normal register will
	// be indexed in SIRSchedulingPass.
	SM->IndexReg2Slot(Reg, DstSlot);

	return true;
}