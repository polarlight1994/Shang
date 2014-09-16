//===------------- Passes.h - Passes Implemented in SIR----------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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
void initializeSIRInitPass(PassRegistry &Registry);
} // end namespace

#endif
