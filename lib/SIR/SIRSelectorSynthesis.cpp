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

using namespace llvm;

namespace llvm {
  struct SIRSelectorSynthesis : public SIRPass {
    static char ID;

    SIRSelectorSynthesis() : SIRPass(ID) {
      initializeSIRSelectorSynthesisPass(*PassRegistry::getPassRegistry());
    }

    bool runOnSIR(SIR &SM);

    void getAnalysisUsage(AnalysisUsage &AU) const;

    bool synthesizeSelector(SIRSelector *Sel,
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

  // Initialize a SIRDatapathBuilder to build expr for guard and Fanin
  SIRDatapathBuilder Builder(&SM, TD);

  typedef SIR::register_iterator iterator;

  for (iterator I = SM.registers_begin(), E = SM.registers_end(); I != E; ++I) {
    SIRSelector *Sel = (*I)->getSelector();
    Value *InsertPosition = (*I)->getSeqInst();
    // If the register is constructed for port or slot, 
    // we should appoint a insert point for it since
    // the SeqInst it holds is pseudo instruction.
    if ((*I)->isOutPort() || (*I)->isSlot()) {
      InsertPosition = &SM.getFunction()->getBasicBlockList().back();
    }    

    Changed |= synthesizeSelector(Sel, InsertPosition, Builder);
  }

  return Changed;
}

bool SIRSelectorSynthesis::synthesizeSelector(SIRSelector *Sel,
                                              Value *InsertPosition,
                                              SIRDatapathBuilder &Builder) {
    // Since LLVM IR is in SSA form, there'll not be two same value
    SmallVector<Value *, 4> OrVec;
    SmallVector<Value *, 4> Fanins, FaninGuards;

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
      //FaninGuards.push_back(*I);

    if (Fanins.empty() || FaninGuards.empty())
      return false;

    unsigned Bitwidth = Sel->getBitWidth();

    assert(Fanins.size() == FaninGuards.size() && "Size not compatible!");

    for (unsigned i = 0; i <Fanins.size(); i++) {
      Value *FaninMask = Builder.createSBitRepeatInst(FaninGuards[i], Bitwidth, InsertPosition, true);
      Value *GuardedFIVal = Builder.createSAndInst(Fanins[i], FaninMask, InsertPosition, true);
      OrVec.push_back(GuardedFIVal);
    }

    Value *FI = Builder.createSConstantInt(1, 1);
    FI = Builder.createSOrInst(OrVec, InsertPosition, true);

    Value *Guard = Builder.createSOrInst(FaninGuards, InsertPosition, true);    

    Sel->setMux(FI, Guard);
}


