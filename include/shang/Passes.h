//===--------- Passes.h - Passes for Verilog target machine -----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// SUnits optimization passes
//
//===----------------------------------------------------------------------===//
#ifndef SHANG_PASSES_H
#define SHANG_PASSES_H

#include <string>

namespace llvm {
class Pass;
class FunctionPass;
class raw_ostream;
class PassRegistry;

class MallocAllocator;
template<typename T, typename AllocatorTy> class StringMap;

//BasicBlockTopOrder Pass - Place the MachineBasicBlocks in topological order.
extern char &BasicBlockTopOrderID;
extern char &ControlLogicSynthesisID;
extern char &SeqSelectorSynthesisID;
extern char &DatapathNamerID;
extern char &SeqLiveVariablesID;

FunctionPass *createDesignMetricsPass();

// Always inline function.
Pass *createHLSInlinerPass();
Pass *createGotoExpansionPass();

Pass *createTrivialLoopUnrollPass();

Pass *createMemoryAccessAlignerPass();

Pass *createFunctionFilterPass(raw_ostream &O,
                               const StringMap<std::string, MallocAllocator> &
                               TopHWFUnctions);

Pass *createLowerAllocaPass();
Pass *createLowerIntrinsicPass();
Pass *createGlobalToStackPass();
Pass *createMemoryAccessCoalescingPass();
Pass *createSimpleBlockRAMAllocationPass();
Pass *createMemoryPartitionPass(bool EnableBanking);
Pass *createShangTargetTransformInfoPass();
Pass *createDatapathHoistingPass();

Pass *createLUTMappingPass();
Pass *createTimingNetlistPass();
Pass *createVASTSchedulingPass();

// Analyse the Combination Path Delay.
Pass *createTimingScriptGenPass(raw_ostream &O);

// Analysis the dependency between registers
Pass *createSeqLiveVariablesPass();
Pass *createRegisterSharingPass();
Pass *createLowerPseudoPHIsPass();

// RTL code generation.
Pass *createRTLCodeGenPass(raw_ostream &O);

Pass *createScriptingPass(const char *Name, const char *FScript,
                          const char *GScript);

void initializeShangTTIPass(PassRegistry &Registry);

void initializeDatapathHoistingPass(PassRegistry &Registry);
void initializeFunctionFilterPass(PassRegistry &Registry);
void initializeHLSInlinerPass(PassRegistry &Registry);
void initializeGotoExpansionPass(PassRegistry &Registry);
void initializeTrivialLoopUnrollPass(PassRegistry &Registry);
void initializeMemoryAccessAlignerPass(PassRegistry &Registry);

void initializeMemoryAccessCoalescingPass(PassRegistry &Registry);
void initializeBasicBlockTopOrderPass(PassRegistry &Registry);

void initializeHLSAllocationAnalysisGroup(PassRegistry &Registry);
void initializeBasicAllocationPass(PassRegistry &Registry);
void initializeSimpleBlockRAMAllocationPass(PassRegistry &Registry);
void initializeMemoryPartitionPass(PassRegistry &Registry);

void initializeLowerAllocaPass(PassRegistry &Registry);
void initializeGlobalToStackPass(PassRegistry &Registry);
void initializeLowerIntrinsicPass(PassRegistry &Registry);
void initializeLUTMappingPass(PassRegistry &Registry);
void initializeTimingNetlistPass(PassRegistry &Registry);
void initializeVASTSchedulingPass(PassRegistry &Registry);
void initializeSTGShortestPathPass(PassRegistry &Registry);
void initializeOverlappedSlotsPass(PassRegistry &Registry);

void initializeControlLogicSynthesisPass(PassRegistry &Registry);
void initializeSeqSelectorSynthesisPass(PassRegistry &Registry);
void initializeDatapathNamerPass(PassRegistry &Registry);
void initializeTimingScriptGenPass(PassRegistry &Registry);
void initializeSeqLiveVariablesPass(PassRegistry &Registry);
void initializeRegisterSharingPass(PassRegistry &Registry);
void initializeLowerPseudoPHIsPass(PassRegistry &Registry);

void initializeRTLCodeGenPass(PassRegistry &Registry);
} // end namespace

#endif
