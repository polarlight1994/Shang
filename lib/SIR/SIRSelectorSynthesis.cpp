//===----------------- SIRSelectorSynthesis.cpp -----------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of the SIRSelectorSynthesis pass, which synthesis the 
// selector as MUX.
//
//===----------------------------------------------------------------------===//
#include "sir/SIR.h"
#include "sir/SIRPass.h"
#include "sir/SIRBuild.h"
#include "sir/Passes.h"

#include "llvm/ADT/PostOrderIterator.h"

using namespace llvm;

namespace llvm {
  struct SIRSelectorSynthesis : public SIRPass {
    static char ID;

    SIRSelectorSynthesis() : SIRPass(ID) {
      initializeSIRSelectorSynthesisPass(*PassRegistry::getPassRegistry());
    }

    bool runOnSIR(SIR &SM);

    void getAnalysisUsage(AnalysisUsage &AU) const;

    bool synthesizeSelector(SIRRegister *Reg,
                            Value *InsertPosition,
                            SIRDatapathBuilder &Builder);
  };
}

char SIRSelectorSynthesis::ID = 0;
char &llvm::SIRSelectorSynthesisID = SIRSelectorSynthesis::ID;
INITIALIZE_PASS_BEGIN(SIRSelectorSynthesis,
                      "SIR-selector-synthesis",
                      "Implement the MUX for the Sequential Logic in SIR",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(SIRSelectorSynthesis,
                    "SIR-selector-synthesis",
                    "Implement the MUX for the Sequential Logic in SIR",
                    false, true)

void SIRSelectorSynthesis::getAnalysisUsage(AnalysisUsage &AU) const {
  SIRPass::getAnalysisUsage(AU);
  AU.addRequired<DataLayout>();
  AU.setPreservesAll();
}

bool SIRSelectorSynthesis::runOnSIR(SIR &SM) {
  bool Changed = false;

  DataLayout &TD = getAnalysis<DataLayout>();

  // Initialize a SIRDatapathBuilder to build expression for guard and Fanin
  SIRDatapathBuilder Builder(&SM, TD);

  typedef SIR::seqop_iterator seqop_iterator;
  for (seqop_iterator I = SM.seqop_begin(), E = SM.seqop_end(); I != E; ++I) {
		SIRSeqOp *SeqOp = *I;
		SIRRegister *Reg = SeqOp->getDst();
		SIRSelector *Sel = Reg->getSelector();

		Value *InsertPosition = Reg->getSeqInst();
		// If the register is constructed for port or slot, 
		// we should appoint a insert point for it since
		// the SeqInst it holds is pseudo instruction.
		if (Reg->isOutPort() || Reg->isSlot()) {
			InsertPosition = SM.getFunction()->getEntryBlock().getFirstNonPHI();
		}

		// Extract the assignments for selectors.
		Value *Src = SeqOp->getSrc(), *Guard = SeqOp->getGuard();
		Value *SlotGuard = SeqOp->getSlot()->getGuardValue();
		// The guarding condition should consider the SlotGuard.
		Value *AssignGuard = Builder.createSAndInst(Guard, SlotGuard, InsertPosition, true);
		Reg->addAssignment(Src, AssignGuard);          
  }

	typedef SIR::register_iterator reg_iterator;
	for (reg_iterator I = SM.registers_begin(), E = SM.registers_end(); I != E; ++I) {
		SIRRegister *Reg = *I;

		Value *InsertPosition = Reg->getSeqInst();
		// If the register is constructed for port or slot, 
		// we should appoint a insert point for it since
		// the SeqInst it holds is pseudo instruction.
		if (Reg->isOutPort() || Reg->isSlot()) {
			InsertPosition = SM.getFunction()->getEntryBlock().getFirstNonPHI();
		}

		Changed |= synthesizeSelector(Reg, InsertPosition, Builder);
	}

  return Changed;
}

bool SIRSelectorSynthesis::synthesizeSelector(SIRRegister *Reg,
                                              Value *InsertPosition,
                                              SIRDatapathBuilder &Builder) {
    // Since LLVM IR is in SSA form, there'll not be two same value
    SmallVector<Value *, 4> OrVec;
    SmallVector<Value *, 4> Fanins, FaninGuards;

		SIRSelector *Sel = Reg->getSelector();

    for (SIRSelector::const_iterator I = Sel->assign_begin(),
         E = Sel->assign_end(); I != E; ++I) {
      Value *Temp = *I;
      Fanins.push_back(Temp);
    }
      //Fanins.push_back(*I);
    for (SIRSelector::const_guard_iterator I = Sel->guard_begin(),
         E = Sel->guard_end(); I != E; ++I) {
      Value *Temp = *I;
      FaninGuards.push_back(Temp);
    }
      
    if (Fanins.empty() || FaninGuards.empty())
      return false;

    unsigned Bitwidth = Sel->getBitWidth();

    assert(Fanins.size() == FaninGuards.size() && "Size not compatible!");

		// If the register is a SlotReg, then just need to calculate the guard,
		// since the Src Value will always be 1'b1.
		if (Reg->isSlot()) {
			Value *Guard = Builder.createSOrInst(FaninGuards, InsertPosition, true);
			Sel->setMux(Builder.createSConstantInt(1, 1), Guard);

			return true;
		}

		// If there are only 1 Fanin, we can simplify the Verilog code.
		if (Fanins.size() == 1) {
			Sel->setMux(Fanins[0], FaninGuards[0]);

			return true;
		}

    for (unsigned i = 0; i <Fanins.size(); i++) {
      Value *FaninMask = Builder.createSBitRepeatInst(FaninGuards[i], Bitwidth, InsertPosition, true);
      Value *GuardedFIVal = Builder.createSAndInst(Fanins[i], FaninMask, InsertPosition, true);
      OrVec.push_back(GuardedFIVal);
    }

    Value *FI = Builder.createSOrInst(OrVec, InsertPosition, true);
    Value *Guard = Builder.createSOrInst(FaninGuards, InsertPosition, true);    

    Sel->setMux(FI, Guard);

		return true;
}


