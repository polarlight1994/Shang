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

namespace llvm {
class LLVMContext;
class Pass;
class FunctionPass;
class raw_ostream;
class TargetMachine;
class PassRegistry;
class TargetIntrinsicInfo;
class VTargetMachine;
class MachineRegisterInfo;

extern char &AdjustLIForBundlesID;
//MachineBasicBlockTopOrder Pass - Place the MachineBasicBlocks in topological order.
extern char &MachineBasicBlockTopOrderID;

FunctionPass *createVISelDag(VTargetMachine &TM);

FunctionPass *createDesignMetricsPass();

// Always inline function.
Pass *createHLSInlinerPass();

Pass *createTrivialLoopUnrollPass();
Pass *createLoopVectorizerPass();
//Convert the AllocaInst to GlobalVariable.
Pass *createBlockRAMFormation(const TargetIntrinsicInfo &IntrInfo);
Pass *createMemoryAccessAlignerPass();

Pass *createFunctionFilterPass(raw_ostream &O);

// Bit level information analysis
Pass *createBitLevelInfoPass();
Pass *createPreSchedRTLOptPass(bool enableLUTMapping = false);
Pass *createFixMachineCodePass(bool IsPreOpt);
Pass *createHyperBlockFormationPass();
Pass *createPrebindUnbalanceMuxPass();
Pass *createBasicPrebindMuxPass();
Pass *createMemOpsFusingPass();
Pass *createDeadMemOpEliminationPass();
Pass *createHoistDatapathPass();
Pass *createBitLevelDelayInfoPass();
Pass *createVPreRegAllocSchedPass();

// Scheduling pass.
Pass *createVAliasAnalysisPass(const TargetIntrinsicInfo *II);
Pass *createVPreRegAllocSchedPass();

// Register allocation.
FunctionPass *createSimpleRegisterAllocator();

// Analyse the Combination Path Delay.
Pass *createCombPathDelayAnalysisPass();

// Analysis the dependency between registers
Pass *createSeqLiveVariablesPass();
Pass *createSeqReachingDefAnalysisPass();

// RTL code generation.
Pass *createVerilogASTBuilderPass();
Pass *createVerilogASTWriterPass(raw_ostream &O);
Pass *createRTLCodegenPreparePass();
Pass *createDataPathPromotionPass();

Pass *createScriptingPass(const char *Name, const char *FScript,
                          const char *GScript);

void initializeVerilogModuleAnalysisPass(PassRegistry &Registry);
void initializeMachineBasicBlockTopOrderPass(PassRegistry &Registry);
void initializeBitLevelDelayInfoPass(PassRegistry &Registry);
void initializeVPreRegAllocSchedPass(PassRegistry &Registry);
void initializeVAliasAnalysisPass(PassRegistry &Registry);
void initializeAdjustLIForBundlesPass(PassRegistry &Registry);
void initializePrebindMuxBasePass(PassRegistry &Registry);
void initializePrebindUnbalanceMuxPass(PassRegistry &Registry);
void initializeBitLevelInfoPass(PassRegistry &Registry);
void initializeHyperBlockFormationPass(PassRegistry &Registry);
void initializeCombPathDelayAnalysisPass(PassRegistry &Registry);
void initializeSeqLiveVariablesPass(PassRegistry &Registry);
void initializeSeqReachingDefAnalysisPass(PassRegistry &Registry);
void initializeVerilogASTBuilderPass(PassRegistry &Registry);
void initializeVerilogASTWriterPass(PassRegistry &Registry);
void initializeFunctionFilterPass(PassRegistry &Registry);
void initializeHLSInlinerPass(PassRegistry &Registry);
void initializeTrivialLoopUnrollPass(PassRegistry &Registry);
void initializeLoopVectorizerPass(PassRegistry &Registry);
} // end namespace


#endif
