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
Pass *createSIRInitPass();
Pass *createSIR2RTLPass();

void initializeSIRSelectorSynthesisPass(PassRegistry &Registry);
void initializeSIRInitPass(PassRegistry &Registry);
void initializeSIR2RTLPass(PassRegistry &Registry);
} // end namespace

#endif
