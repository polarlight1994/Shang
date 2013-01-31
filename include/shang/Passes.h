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
#ifndef VBE_HARDWARE_ATOM_PASSES_H
#define VBE_HARDWARE_ATOM_PASSES_H

#include <string>

namespace llvm {
class LLVMContext;
class Pass;
class FunctionPass;
class raw_ostream;
class PassRegistry;

class MallocAllocator;
template<typename T, typename AllocatorTy> class StringMap;

//BasicBlockTopOrder Pass - Place the MachineBasicBlocks in topological order.
extern char &BasicBlockTopOrderID;

FunctionPass *createDesignMetricsPass();

// Always inline function.
Pass *createHLSInlinerPass();

Pass *createTrivialLoopUnrollPass();

Pass *createMemoryAccessAlignerPass();

Pass *createFunctionFilterPass(raw_ostream &O,
                               const StringMap<std::string, MallocAllocator> &
                               TopHWFUnctions);

// Analyse the Combination Path Delay.
Pass *createCombPathDelayAnalysisPass();

// Analysis the dependency between registers
Pass *createSeqLiveVariablesPass();
Pass *createSeqReachingDefAnalysisPass();

// RTL code generation.
Pass *createRTLCodeGenPass(raw_ostream &O);

Pass *createScriptingPass(const char *Name, const char *FScript,
                          const char *GScript);

void initializeBasicBlockTopOrderPass(PassRegistry &Registry);
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
