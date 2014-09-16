//===------------------------- SIRPass.h ------------------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Declaration of the SIRPass pass, which is the base of all passed operated 
// on the SIR module.
//
//===----------------------------------------------------------------------===//

#include "llvm/Pass.h"

#ifndef SIR_PASS_H
#define SIR_PASS_H

namespace llvm {
class Function;
class SIR;

/// ScopPass - This class adapts the FunctionPass interface to allow convenient
/// creation of passes that operate on the SIR. Instead of overriding
/// runOnFunction, subclasses override runOnSIR.
class SIRPass : public FunctionPass {
protected:
  explicit SIRPass(char &ID);

public:
  /// runOnSIR - This method must be overloaded to perform the
  /// desired SIR transformation or analysis.
  ///
  virtual bool runOnSIR(SIR &SM) = 0;

  /// getAnalysisUsage - Subclasses that override getAnalysisUsage
  /// must call this.
  ///
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;

  /// print - Print the status of the pass.
  ///
  //virtual void print(raw_ostream &OS, const Module *) const {
  //print(OS);
  //}

  //virtual void print(raw_ostream &OS) const;

private:
  bool runOnFunction(Function &F);
};
}

#endif