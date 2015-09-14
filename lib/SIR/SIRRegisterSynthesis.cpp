//===----------------- SIRRegisterSynthesis.cpp -----------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of the SIRRegisterSynthesis pass, which synthesis the 
// Register as MUX.
//
//===----------------------------------------------------------------------===//
#include "sir/SIR.h"
#include "sir/SIRPass.h"
#include "sir/SIRBuild.h"
#include "sir/Passes.h"

#include "llvm/ADT/PostOrderIterator.h"

using namespace llvm;

namespace llvm {
struct SIRRegisterSynthesisForAnnotation : public SIRPass {
  static char ID;

  SIRRegisterSynthesisForAnnotation() : SIRPass(ID) {
    initializeSIRRegisterSynthesisForAnnotationPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSIR(SIR &SM);

  void getAnalysisUsage(AnalysisUsage &AU) const;

  bool synthesizeRegister(SIRRegister *Reg,
                          Value *InsertPosition,
                          SIRDatapathBuilder &Builder);
};
}

char SIRRegisterSynthesisForAnnotation::ID = 0;
char &llvm::SIRRegisterSynthesisForAnnotationID = SIRRegisterSynthesisForAnnotation::ID;
INITIALIZE_PASS_BEGIN(SIRRegisterSynthesisForAnnotation,
                      "SIR-Register-synthesis-for-annotation",
                      "Implement the MUX for the Sequential Logic in SIR for annotation",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(SIRRegisterSynthesisForAnnotation,
	                  "SIR-Register-synthesis-for-annotation",
	                  "Implement the MUX for the Sequential Logic in SIR for annotation",
	                  false, true)

void SIRRegisterSynthesisForAnnotation::getAnalysisUsage(AnalysisUsage &AU) const {
  SIRPass::getAnalysisUsage(AU);
  AU.addRequired<DataLayout>();
  AU.setPreservesAll();
}

bool SIRRegisterSynthesisForAnnotation::runOnSIR(SIR &SM) {
  bool Changed = false;

  DataLayout &TD = getAnalysis<DataLayout>();

  // Initialize a SIRDatapathBuilder to build expression for guard and Fanin
  SIRDatapathBuilder Builder(&SM, TD);

   typedef SIR::seqop_iterator seqop_iterator;
   for (seqop_iterator I = SM.seqop_begin(), E = SM.seqop_end(); I != E; ++I) {
		 SIRSeqOp *SeqOp = I;
		 SIRRegister *Reg = SeqOp->getDst();
    
		// Insert the implement of register just in front of the terminator instruction
		// at back of the module to avoid being used before declaration.
		Value *InsertPosition = SM.getPositionAtBackOfModule();

 		// Extract the assignments for Registers.
 		Value *Src = SeqOp->getSrc(), *Guard = SeqOp->getGuard();
		Value *GuardConsiderSlot = Builder.createSAndInst(Guard, SeqOp->getSlot()->getGuardValue(),
			                                                Guard->getType(), InsertPosition, true);
 		Reg->addAssignment(Src, Guard);          
  }

	typedef SIR::register_iterator reg_iterator;
	for (reg_iterator I = SM.registers_begin(), E = SM.registers_end(); I != E; ++I) {
		SIRRegister *Reg = *I;

		// Insert the implement of register just in front of the terminator instruction
		// at back of the module to avoid being used before declaration.
		Value *InsertPosition = SM.getPositionAtBackOfModule();

		Changed |= synthesizeRegister(Reg, InsertPosition, Builder);
	}

  return Changed;
}

bool SIRRegisterSynthesisForAnnotation::synthesizeRegister(SIRRegister *Reg,
                                                           Value *InsertPosition,
                                                           SIRDatapathBuilder &Builder) {
  // Since LLVM IR is in SSA form, there'll not be two same value
  SmallVector<Value *, 4> OrVec;
  SmallVector<Value *, 4> Fanins, FaninGuards;

  for (SIRRegister::const_iterator I = Reg->assign_begin(),
       E = Reg->assign_end(); I != E; ++I) {
    Value *Temp = *I;
    Fanins.push_back(Temp);
  }

  for (SIRRegister::const_guard_iterator I = Reg->guard_begin(),
       E = Reg->guard_end(); I != E; ++I) {
    Value *Temp = *I;
    FaninGuards.push_back(Temp);
  }
      
  if (Fanins.empty() || FaninGuards.empty())
    return false;

  unsigned Bitwidth = Reg->getBitWidth();

  assert(Fanins.size() == FaninGuards.size() && "Size not compatible!");

	// If there are only 1 Fanin, we can simplify the Verilog code.
	if (Fanins.size() == 1) {
		Reg->setMux(Fanins[0], FaninGuards[0]);
	}

  for (unsigned i = 0; i <Fanins.size(); i++) {
    Value *FaninMask = Builder.createSBitRepeatInst(FaninGuards[i], Bitwidth, Fanins[i]->getType(), InsertPosition, true);
    Value *GuardedFIVal = Builder.createSAndInst(Fanins[i], FaninMask, Fanins[i]->getType(), InsertPosition, true);
    OrVec.push_back(GuardedFIVal);
  }

  Value *FI = Builder.createSOrInst(OrVec, OrVec[0]->getType(), InsertPosition, true);
  Value *Guard = Builder.createSOrInst(FaninGuards, FaninGuards[0]->getType(), InsertPosition, true); 

  Reg->setMux(FI, Guard);

	return true;
}


namespace llvm {
struct SIRRegisterSynthesisForCodeGen : public SIRPass {
  static char ID;

  SIRRegisterSynthesisForCodeGen() : SIRPass(ID) {
    initializeSIRRegisterSynthesisForCodeGenPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSIR(SIR &SM);

  void getAnalysisUsage(AnalysisUsage &AU) const;

  bool synthesizeRegister(SIRRegister *Reg,
                          Value *InsertPosition,
                          SIRDatapathBuilder &Builder);
};
}

char SIRRegisterSynthesisForCodeGen::ID = 0;
char &llvm::SIRRegisterSynthesisForCodeGenID = SIRRegisterSynthesisForCodeGen::ID;
INITIALIZE_PASS_BEGIN(SIRRegisterSynthesisForCodeGen,
                      "SIR-Register-synthesis-for-code-generate",
                      "Implement the MUX for the Sequential Logic in SIR for CodeGen",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
	//INITIALIZE_PASS_DEPENDENCY(SIRFSMSynthesis)
INITIALIZE_PASS_END(SIRRegisterSynthesisForCodeGen,
	                  "SIR-Register-synthesis-for-code-generate",
	                  "Implement the MUX for the Sequential Logic in SIR for CodeGen",
	                  false, true)

void SIRRegisterSynthesisForCodeGen::getAnalysisUsage(AnalysisUsage &AU) const {
  SIRPass::getAnalysisUsage(AU);
  AU.addRequired<DataLayout>();
  AU.setPreservesAll();
}

bool SIRRegisterSynthesisForCodeGen::runOnSIR(SIR &SM) {
  bool Changed = false;

  DataLayout &TD = getAnalysis<DataLayout>();

  // Initialize a SIRDatapathBuilder to build expression for guard and Fanin
  SIRDatapathBuilder Builder(&SM, TD);

	// Since the RegVal and RegGuard will be regenerate to associate the guard
	// condition with the SlotGuard, so drop it first.
	typedef SIR::register_iterator reg_iterator;
	for (reg_iterator I = SM.registers_begin(), E = SM.registers_end(); I != E; ++I) {
		SIRRegister *Reg = *I;
		Reg->dropMux();
	}

	typedef SIR::seqop_iterator seqop_iterator;
	for(seqop_iterator I = SM.seqop_begin(), E = SM.seqop_end(); I != E; ++I) {
		SIRSeqOp *SeqOp = I;

		Value *SrcVal = SeqOp->getSrc();
		SIRRegister *Dst = SeqOp->getDst();	
		Value *GuardVal = SeqOp->getGuard();
		SIRSlot *Slot = SeqOp->getSlot();

		// Index the normal register to this slot and the slot register will
		// be indexed in SIRFSMSynthsisPass.
		SM.IndexReg2Slot(Dst, Slot);

		// Insert the implement of register just in front of the terminator instruction
		// at back of the module to avoid being used before declaration.
		Value *InsertPosition = SM.getPositionAtBackOfModule();

		// Associate the guard with the Slot guard.
		Value *NewGuardVal = Builder.createSAndInst(GuardVal, Slot->getGuardValue(),
			                                          GuardVal->getType(), InsertPosition, true);

		assert(Builder.getBitWidth(NewGuardVal) == 1 && "Bad BitWidth of Guard Value!");

		Dst->addAssignment(SrcVal, NewGuardVal);
	}

	for (reg_iterator I = SM.registers_begin(), E = SM.registers_end(); I != E; ++I) {
		SIRRegister *Reg = *I;

		// Insert the implement of register just in front of the terminator instruction
		// at back of the module to avoid being used before declaration.
		Value *InsertPosition = SM.getPositionAtBackOfModule();

		Changed |= synthesizeRegister(Reg, InsertPosition, Builder);
	}

  return Changed;
}

bool SIRRegisterSynthesisForCodeGen::synthesizeRegister(SIRRegister *Reg,
                                                        Value *InsertPosition,
                                                        SIRDatapathBuilder &Builder) {
  // Since LLVM IR is in SSA form, there'll not be two same value
  SmallVector<Value *, 4> OrVec;
  SmallVector<Value *, 4> Fanins, FaninGuards;

  for (SIRRegister::const_iterator I = Reg->assign_begin(),
       E = Reg->assign_end(); I != E; ++I) {
    Value *Temp = *I;
    Fanins.push_back(Temp);
  }

  for (SIRRegister::const_guard_iterator I = Reg->guard_begin(),
       E = Reg->guard_end(); I != E; ++I) {
    Value *Temp = *I;
    FaninGuards.push_back(Temp);
  }
      
  if (Fanins.empty() || FaninGuards.empty())
    return false;

  unsigned Bitwidth = Reg->getBitWidth();

  assert(Fanins.size() == FaninGuards.size() && "Size not compatible!");

	// If the register is a SlotReg, then just need to calculate the guard,
	// since the Src Value will always be 1'b1.
	if (Reg->isSlot()) {
	  Value *Guard = Builder.createSOrInst(FaninGuards, FaninGuards[0]->getType(), InsertPosition, true);
		Reg->setMux(Builder.createIntegerValue(1, 1), Guard);

		return true;
	}

	// If there are only 1 Fanin, we can simplify the Verilog code.
	if (Fanins.size() == 1) {
		Reg->setMux(Fanins[0], FaninGuards[0]);

		return true;
	}

  for (unsigned i = 0; i <Fanins.size(); i++) {
    Value *FaninMask = Builder.createSBitRepeatInst(FaninGuards[i], Bitwidth, Fanins[i]->getType(), InsertPosition, true);
    Value *GuardedFIVal = Builder.createSAndInst(Fanins[i], FaninMask, Fanins[i]->getType(), InsertPosition, true);
    OrVec.push_back(GuardedFIVal);
  }

  Value *FI = Builder.createSOrInst(OrVec, OrVec[0]->getType(), InsertPosition, true);
  Value *Guard = Builder.createSOrInst(FaninGuards, FaninGuards[0]->getType(), InsertPosition, true);    

  Reg->setMux(FI, Guard);

	return true;
}


