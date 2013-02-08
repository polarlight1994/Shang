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
extern char &DatapathNamerID;
extern char &TimingNetlistID;

FunctionPass *createDesignMetricsPass();

// Always inline function.
Pass *createHLSInlinerPass();

Pass *createTrivialLoopUnrollPass();

Pass *createMemoryAccessAlignerPass();

Pass *createFunctionFilterPass(raw_ostream &O,
                               const StringMap<std::string, MallocAllocator> &
                               TopHWFUnctions);

Pass *createLowerAllocaPass();
Pass *createMemoryAccessCoalescingPass();
Pass *createSimpleBlockRAMAllocationPass();
Pass *createShangTargetTransformInfoPass();
Pass *createDatapathHoistingPass();

Pass *createLUTMappingPass();
Pass *createTimingNetlistPass();

// Analyse the Combination Path Delay.
Pass *createCombPathDelayAnalysisPass();

// Analysis the dependency between registers
Pass *createSeqLiveVariablesPass();
Pass *createSeqReachingDefAnalysisPass();

// RTL code generation.
Pass *createRTLCodeGenPass(raw_ostream &O);

Pass *createScriptingPass(const char *Name, const char *FScript,
                          const char *GScript);

void initializeShangTTIPass(PassRegistry &Registry);
void initializeDatapathHoistingPass(PassRegistry &Registry);
void initializeMemoryAccessCoalescingPass(PassRegistry &Registry);
void initializeBasicBlockTopOrderPass(PassRegistry &Registry);
void initializeHLSAllocationAnalysisGroup(PassRegistry &Registry);
void initializeSimpleBlockRAMAllocationPass(PassRegistry &Registry);
void initializeBasicAllocationPass(PassRegistry &Registry);
void initializeLowerAllocaPass(PassRegistry &Registry);
void initializeLUTMappingPass(PassRegistry &Registry);
void initializeTimingNetlistPass(PassRegistry &Registry);
void initializeControlLogicSynthesisPass(PassRegistry &Registry);
void initializeDatapathNamerPass(PassRegistry &Registry);
void initializeCombPathDelayAnalysisPass(PassRegistry &Registry);
void initializeSeqLiveVariablesPass(PassRegistry &Registry);
void initializeSeqReachingDefAnalysisPass(PassRegistry &Registry);
void initializeRTLCodeGenPass(PassRegistry &Registry);
void initializeFunctionFilterPass(PassRegistry &Registry);
void initializeHLSInlinerPass(PassRegistry &Registry);
void initializeTrivialLoopUnrollPass(PassRegistry &Registry);
void initializeLoopVectorizerPass(PassRegistry &Registry);
} // end namespace

#endif
