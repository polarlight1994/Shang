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
  static char ID;

  SelectorSynthesis() : VASTModulePass(ID) {
    initializeSelectorSynthesisPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<CachedStrashTable>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
  }

  bool runOnVASTModule(VASTModule &VM);

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

bool SelectorSynthesis::runOnVASTModule(VASTModule &VM) {
  CachedStrashTable &CST = getAnalysis<CachedStrashTable>();

  {
    MinimalExprBuilderContext Context(VM);
    VASTExprBuilder Builder(Context);

    typedef VASTModule::selector_iterator iterator;

    // Build the MUXs
    for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
      I->buildMux(Builder, CST);
  }

  // Perform LUT Mapping on the newly synthesized selectors.
  VASTModulePass *P = static_cast<VASTModulePass*>(createLUTMappingPass());
  AnalysisResolver *AR = new AnalysisResolver(*getResolver());
  P->setResolver(AR);
  P->runOnVASTModule(VM);
  delete P;

  return true;
}
