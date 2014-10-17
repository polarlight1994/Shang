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

    bool synthesizeSelector(SIRSelector *Sel, Instruction *InsertBefore,
                            SIRDatapathBuilder &Builder);
  };
}

char SIRSelectorSynthesis::ID = 0;

INITIALIZE_PASS_BEGIN(SIRSelectorSynthesis,
                      "SIR-selector-synthesis",
                      "Implement the MUX for the Sequantal Logic in SIR",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(SIRSelectorSynthesis,
                    "SIR-selector-synthesis",
                    "Implement the MUX for the Sequantal Logic in SIR",
                    false, true)

void SIRSelectorSynthesis::getAnalysisUsage(AnalysisUsage &AU) const {
  SIRPass::getAnalysisUsage(AU);
  AU.addRequired<DataLayout>();
  AU.setPreservesAll();
}

bool SIRSelectorSynthesis::runOnSIR(SIR &SM) {
  bool Changed = false;

  DataLayout &TD = getAnalysis<DataLayout>();

  // Initialize a SIRDatapathBuilder to build expr for guard and fanin
  SIRDatapathBuilder Builder(&SM, TD);

  typedef SIR::register_iterator iterator;

  for (iterator I = SM.registers_begin(), E = SM.registers_end(); I != E; ++I) {
    SIRSelector *Sel = (*I)->getSelector();
    Instruction *InsertBefore = (*I)->getSeqInst();
    Changed |= synthesizeSelector(Sel, InsertBefore, Builder);
  }

  return Changed;
}

bool SIRSelectorSynthesis::synthesizeSelector(SIRSelector *Sel, Instruction *InsertBefore,
                                              SIRDatapathBuilder &Builder) {  

    // Since LLVM IR is in SSA form, there'll not be two same value
    SmallVector<Value *, 4> OrVec;
    SmallVector<Value *, 4> Fanins, FaninGuards;

    for (SIRSelector::const_iterator I = Sel->assign_begin(),
         E = Sel->assign_end(); I != E; ++I) 
      Fanins.push_back(*I);
    for (SIRSelector::const_guard_iterator I = Sel->guard_begin(),
         E = Sel->guard_end(); I != E; ++I)
      FaninGuards.push_back(*I);

    if (Fanins.empty() || FaninGuards.empty())
      return false;

    unsigned Bitwidth = Sel->getBitWidth();

    assert(Fanins.size() == FaninGuards.size() && "Size not compatible!");

    for (unsigned i = 0; i <Fanins.size(); i++) {
      Value *FaninMask = Builder.createSBitRepeatInst(FaninGuards[i], Bitwidth, InsertBefore, true);
      Value *GuardedFIVal = Builder.createSAndInst(Fanins[i], FaninMask, InsertBefore, true);
      OrVec.push_back(GuardedFIVal);
    }

    Value *FI = Builder.createSConstantInt(1, 1);
    FI = Builder.createSOrInst(OrVec, InsertBefore, true);

    Sel->setMux(FI, Builder.createSOrInst(FaninGuards, InsertBefore, true));
}


