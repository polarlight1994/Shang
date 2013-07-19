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
#include "shang/FUInfo.h"
#include "shang/VASTMemoryPort.h"
#include "shang/VASTExprBuilder.h"

#include "shang/Strash.h"
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
STATISTIC(NumTrivialConesMerged, "Number of trivial cones merged");
STATISTIC(NumConesMerged, "Number of cones merged");

namespace {
struct SelectorSynthesis : public VASTModulePass {
  CachedStrashTable *CST;
  VASTExprBuilder *Builder;
  VASTModule *VM;

  static char ID;

  SelectorSynthesis() : VASTModulePass(ID), CST(0) {
    initializeSelectorSynthesisPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<CachedStrashTable>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
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

void SelectorSynthesis::synthesizeSelector(VASTSelector *Sel,
                                           VASTExprBuilder &Builder) {
  typedef std::vector<VASTLatch> OrVec;
  typedef std::map<unsigned, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator iterator;

  CSEMapTy CSEMap;

  for (VASTSelector::const_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U)) continue;

    // Index the input of the assignment based on the strash id. By doing this
    // we can pack the structural identical inputs together.
    CSEMap[CST->getOrCreateStrashID(VASTValPtr(U))].push_back(U);
  }

  if (CSEMap.empty()) return;

  unsigned Bitwidth = Sel->getBitWidth();

  SmallVector<VASTValPtr, 4> SlotGuards, SlotFanins, FaninGuards, Fanins;
  SmallVector<VASTSlot*, 4> Slots;

  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    SlotGuards.clear();
    SlotFanins.clear();

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      SmallVector<VASTValPtr, 2> CurGuards;
      const VASTLatch &L = *OI;
      SlotFanins.push_back(L);
      VASTSlot *S = L.getSlot();

      if (VASTValPtr SlotActive = L.getSlotActive())
        CurGuards.push_back(SlotActive);

      CurGuards.push_back(L.getGuard());
      VASTValPtr CurGuard = Builder.buildAndExpr(CurGuards, 1);

      // Simply keep all guarding condition, because they are only 1 bit nodes,
      // and their upper bound is the product of number of slots and number of
      // basic blocks.
      CurGuard = Builder.buildKeep(CurGuard);

      // The guarding condition itself is not guard, that is, the guarding
      // condition is read whenever the slot register is set. Hence, we should
      // annotate it with the control-equivalent group instead of the guarding
      // condition equivalent group!
      // FIXME: We can build apply the keep attribute according to the STG
      // subgroup hierarchy sequentially to relax the constraints.
      Sel->annotateReadSlot(S->getParentState(), CurGuard);
      SlotGuards.push_back(CurGuard);
      Slots.push_back(S);
    }

    VASTValPtr FIGuard = Builder.buildOrExpr(SlotGuards, 1);
    FaninGuards.push_back(FIGuard);

    VASTValPtr FIMask = Builder.buildBitRepeat(FIGuard, Bitwidth);
    VASTValPtr FIVal = Builder.buildAndExpr(SlotFanins, Bitwidth);
    VASTValPtr GuardedFIVal = Builder.buildAndExpr(FIVal, FIMask, Bitwidth);
    GuardedFIVal = Builder.buildKeep(GuardedFIVal);
    Fanins.push_back(GuardedFIVal);
    while (!Slots.empty())
      Sel->annotateReadSlot(Slots.pop_back_val(), GuardedFIVal);
  }

  // Strip the keep attribute if the keeped value is directly fan into the
  // register.
  Sel->setGuard(Builder.buildOrExpr(FaninGuards, 1));
  Sel->setFanin(Builder.buildOrExpr(Fanins, Bitwidth));
}

bool SelectorSynthesis::runOnVASTModule(VASTModule &VM) {
  CST = &getAnalysis<CachedStrashTable>();

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
