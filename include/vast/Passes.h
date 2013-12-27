//===------------- Passes.h - Passes Implemented in VAST --------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// SUnits optimization passes
//
//===----------------------------------------------------------------------===//
#ifndef VAST_PASSES_H
#define VAST_PASSES_H

#include <string>

namespace llvm {
class Pass;
class FunctionPass;
class raw_ostream;
class PassRegistry;
}

namespace vast {
using namespace llvm;

extern char &ControlLogicSynthesisID;
extern char &TimingDrivenSelectorSynthesisID;
extern char &DatapathNamerID;
extern char &BitlevelOptID;
extern char &SeqLiveVariablesID;
extern char &STGDistancesID;
extern char &DataflowID;
extern char &PreSchedBindingID;

Pass *createObjectBasedAliasAnalyaisPass();
FunctionPass *createDesignMetricsPass();
// Always inline function.
Pass *createHLSInlinerPass();
Pass *createGotoExpansionPass();

Pass *createTrivialLoopUnrollPass();

Pass *createMemoryAccessAlignerPass();

Pass *createFunctionFilterPass();

Pass *createLowerAllocaPass();
Pass *createLowerIntrinsicPass();
Pass *createGlobalToStackPass();
Pass *createMemoryAccessCoalescingPass();
Pass *createMemoryPartitionPass();
Pass *createShangTargetTransformInfoPass();
Pass *createAlwaysSpeculatePass();

Pass *createBitlevelOptPass();
Pass *createOptimizePHINodesPass();
Pass *createTimingNetlistPass();
Pass *createDataflowAnnotationPass();
Pass *createVASTSchedulingPass();
Pass *createIterativeSchedulingPass();

// Generate the multi-cycle path constraints.
Pass *createTimingScriptGenPass();

// Analysis the dependency between registers
Pass *createSeqLiveVariablesPass();
Pass *createRegisterSharingPass();
Pass *createSelectorPipeliningPass();
Pass *createLowerGetElementPtrPass();

// RTL code generation.
Pass *createRTLCodeGenPass();
} // end namespace vast

namespace llvm {
void initializeShangTTIPass(PassRegistry &Registry);
void initializeObjectBasedAliasAnalyaisPass(PassRegistry &Registry);

void initializeAlwaysSpeculatePass(PassRegistry &Registry);
void initializeLowerGetElementPtrPass(PassRegistry &Registry);
void initializeFunctionFilterPass(PassRegistry &Registry);
void initializeHLSInlinerPass(PassRegistry &Registry);
void initializeGotoExpansionPass(PassRegistry &Registry);
void initializeTrivialLoopUnrollPass(PassRegistry &Registry);
void initializeMemoryAccessAlignerPass(PassRegistry &Registry);

void initializeMemoryAccessCoalescingPass(PassRegistry &Registry);
void initializeBasicBlockTopOrderPass(PassRegistry &Registry);

void initializeHLSAllocationAnalysisGroup(PassRegistry &Registry);
void initializeBasicAllocationPass(PassRegistry &Registry);
void initializeMemoryPartitionPass(PassRegistry &Registry);

void initializeLowerAllocaPass(PassRegistry &Registry);
void initializeGlobalToStackPass(PassRegistry &Registry);
void initializeLowerIntrinsicPass(PassRegistry &Registry);
void initializeBitlevelOptPass(PassRegistry &Registry);
void initializeOptimizePHINodesPass(PassRegistry &Registry);
void initializeDataflowPass(PassRegistry &Registry);
void initializeDataflowAnnotationPass(PassRegistry &Registry);
void initializeTimingNetlistPass(PassRegistry &Registry);
void initializeVASTSchedulingPass(PassRegistry &Registry);
void initializeIterativeSchedulingPass(PassRegistry &Registry);
void initializeSTGDistancesPass(PassRegistry &Registry);

void initializeControlLogicSynthesisPass(PassRegistry &Registry);
void initializeTimingDrivenSelectorSynthesisPass(PassRegistry &Registry);
void initializeSelectorPipeliningPass(PassRegistry &Registry);
void initializeDatapathNamerPass(PassRegistry &Registry);
void initializeCachedStrashTablePass(PassRegistry &Registry);
void initializeCombPatternTablePass(PassRegistry &Registry);
void initializeTimingScriptGenPass(PassRegistry &Registry);
void initializeSeqLiveVariablesPass(PassRegistry &Registry);
void initializeRegisterSharingPass(PassRegistry &Registry);
void initializePreSchedBindingPass(PassRegistry &Registry);

void initializeRTLCodeGenPass(PassRegistry &Registry);
} // end namespace

#endif
