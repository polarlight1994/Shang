//===- SelectorSynthesis.cpp - Implement the Fanin Mux for Regs -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the control logic synthesis pass.
//
//===----------------------------------------------------------------------===//

#include "STGDistances.h"
#include "TimingNetlist.h"
#include "SeqLiveVariables.h"

#include "shang/FUInfo.h"
#include "shang/VASTMemoryPort.h"
#include "shang/VASTExprBuilder.h"

#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"
#include "shang/Passes.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-selector-mux-synthesis"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {

struct SelectorSynthesis : public VASTModulePass {
  TimingNetlist *TNL;
  SeqLiveVariables *SLV;
  STGDistances *SSP;
  unsigned MaxSingleCyleFINum;
  VASTExprBuilder *Builder;
  VASTModule *VM;

  static char ID;

  SelectorSynthesis() : VASTModulePass(ID), TNL(0), SLV(0), SSP(0) {
    initializeSelectorSynthesisPass(*PassRegistry::getPassRegistry());

    VFUMux *Mux = getFUDesc<VFUMux>();
    MaxSingleCyleFINum = 2;
    while (Mux->getMuxLatency(MaxSingleCyleFINum) < 0.9
           && MaxSingleCyleFINum < Mux->MaxAllowedMuxSize)
      ++MaxSingleCyleFINum;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addPreserved<SeqLiveVariables>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
    AU.addPreserved<STGDistances>();
  }

  bool runOnVASTModule(VASTModule &VM);

  void synthesizeSelector(VASTSelector *Sel, VASTExprBuilder &Builder);

  void releaseMemory() {}
};
}

INITIALIZE_PASS_BEGIN(SelectorSynthesis, "sequential-selector-synthesis",
                      "Implement the MUX for the Sequantal Logic", false, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
INITIALIZE_PASS_END(SelectorSynthesis, "sequential-selector-synthesis",
                    "Implement the MUX for the Sequantal Logic", false, true)

char SelectorSynthesis::ID = 0;

char &llvm::SelectorSynthesisID = SelectorSynthesis::ID;

static VASTValPtr StripKeep(VASTValPtr V) {
  if (VASTExprPtr Expr = dyn_cast<VASTExpr>(V))
    if (Expr->getOpcode() == VASTExpr::dpKeep)
      return Expr.getOperand(0);

  return V;
}

void SelectorSynthesis::synthesizeSelector(VASTSelector *Sel,
                                           VASTExprBuilder &Builder) {
  typedef std::vector<const VASTSeqOp*> OrVec;
  typedef VASTSelector::CSEMapTy CSEMapTy;
  typedef CSEMapTy::const_iterator iterator;

  CSEMapTy CSEMap;

  if (!Sel->buildCSEMap(CSEMap)) return;

  unsigned Bitwidth = Sel->getBitWidth();

  // Print the mux logic.
  std::set<VASTValPtr, VASTSelector::StructualLess> FaninGuards;
  bool KeepFI = CSEMap.size() > 1;

  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    VASTValPtr FIVal = I->first;

    VASTSelector::Fanin *FI = Sel->createFanin(FIVal);
    FaninGuards.clear();

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      SmallVector<VASTValPtr, 2> CurGuards;
      const VASTSeqOp *Op = *OI;

      if (VASTValPtr SlotActive = Op->getSlotActive())
        CurGuards.push_back(SlotActive);

      CurGuards.push_back(Op->getGuard());
      VASTValPtr CurGuard = Builder.buildAndExpr(CurGuards, 1);
      CurGuard = Builder.buildKeep(CurGuard);

      FaninGuards.insert(CurGuard);
      FI->AddSlot(CurGuard, Op->getSlot());
    }

    SmallVector<VASTValPtr, 4> Guards(FaninGuards.begin(), FaninGuards.end());
    VASTValPtr FIGuard = Builder.buildOrExpr(Guards, 1);
    FI->setCombinedGuard(FIGuard);

    // If there is only 1 slot guard for this fanin value, we do not need to
    // build the guarded fanin with the "keeped" guard. Because the dynamic
    // false paths only present when there are multiple slot guards guarding this
    // fanin value.
    if (Guards.size() == 1)
      FIGuard = StripKeep(FIGuard);

    VASTValPtr FIMask = Builder.buildBitRepeat(FIGuard, Bitwidth);
    VASTValPtr GuardedFIVal = Builder.buildAndExpr(FIVal, FIMask, Bitwidth);
    if (KeepFI)
      GuardedFIVal = Builder.buildKeep(GuardedFIVal);
    FI->GuardedFI.set(GuardedFIVal);
  }
}

bool SelectorSynthesis::runOnVASTModule(VASTModule &VM) {
  {
    MinimalExprBuilderContext Context(VM);
    Builder = new VASTExprBuilder(Context);
    this->VM = &VM;

    typedef VASTModule::selector_iterator iterator;

    // Eliminate the identical SeqOps.
    for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
      synthesizeSelector(I, *Builder);

    delete Builder;
  }

  // Perform LUT Mapping on the newly synthesized selectors.
  VASTModulePass *P = static_cast<VASTModulePass*>(createLUTMappingPass());
  AnalysisResolver *AR = new AnalysisResolver(*getResolver());
  P->setResolver(AR);
  P->runOnVASTModule(VM);
  delete P;

  return true;
}
