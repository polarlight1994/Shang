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
extern char &DFGBuildID;
extern char &BitMaskAnalysisID;
extern char &DFGOptID;
extern char &DFGAnalysisID;
extern char &SIRMOAOptID;
extern char &SIRRegisterSynthesisForAnnotationID;
extern char &SIRRegisterSynthesisForCodeGenID;
extern char &SIRSchedulingID;
extern char &SIRTimingScriptGenID;

Pass *createSIRLowerIntrinsicPass();
Pass *createSIRInitPass();
Pass *createSIRAllocationPass();
Pass *createSIR2RTLPass();
Pass *createSIRSchedulingPass();

void initializeDFGBuildPass(PassRegistry &Registry);
void initializeSIRLowerIntrinsicPass(PassRegistry &Registry);
void initializeSIRAllocationPass(PassRegistry &Registry);
void initializeSIRMemoryPartitionPass(PassRegistry &Registry);
void initializeBitMaskAnalysisPass(PassRegistry &Registry);
void initializeDFGOptPass(PassRegistry &Registry);
void initializeDFGAnalysisPass(PassRegistry &Registry);
void initializeSIRMOAOptPass(PassRegistry &Registry);
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
