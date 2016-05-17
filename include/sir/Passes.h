//===------------- Passes.h - Passes Implemented in SIR----------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// SIR optimization passes
//
//===----------------------------------------------------------------------===//
#include <string>

#ifndef SIR_PASSES_H
#define SIR_PASSES_H

namespace llvm {
class Pass;
class FunctionPass;
class raw_ostream;
class PassRegistry;
}

namespace llvm {
extern char &SIRBitMaskAnalysisID;
extern char &SIRDatapathOptID;
extern char &SIRAddMulChainID;
extern char &SIRFindCriticalPathID;
extern char &SIRRegisterSynthesisForAnnotationID;
extern char &SIRRegisterSynthesisForCodeGenID;
extern char &SIRSchedulingID;
extern char &SIRTimingScriptGenID;

Pass *createSIRLowerIntrinsicPass();
Pass *createSIRInitPass();
Pass *createSIRAllocationPass();
Pass *createSIR2RTLPass();
Pass *createSIRSchedulingPass();

void initializeSIRLowerIntrinsicPass(PassRegistry &Registry);
void initializeSIRAllocationPass(PassRegistry &Registry);
void initializeSIRMemoryPartitionPass(PassRegistry &Registry);
void initializeSIRBitMaskAnalysisPass(PassRegistry &Registry);
void initializeSIRDatapathOptPass(PassRegistry &Registry);
void initializeSIRAddMulChainPass(PassRegistry &Registry);
void initializeSIRFindCriticalPathPass(PassRegistry &Registry);
void initializeSIRSchedulingPass(PassRegistry &Registry);
void initializeSIRTimingAnalysisPass(PassRegistry &Registry);
void initializeSIRFSMSynthesisPass(PassRegistry &Registry);
void initializeSIRRegisterSynthesisForAnnotationPass(PassRegistry &Registry);
void initializeSIRRegisterSynthesisForCodeGenPass(PassRegistry &Registry);
void initializeSIRInitPass(PassRegistry &Registry);
void initializeSIRSTGDistancePass(PassRegistry &Registry);
void initializeSIRTimingScriptGenPass(PassRegistry &Registry);
void initializeSIR2RTLPass(PassRegistry &Registry);
} // end namespace

#endif
