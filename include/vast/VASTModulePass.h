//===--------- VASTModulePass.h - Pass for VASTModules -----------*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the VASTModulePass class. VASTModulePass are just
// FunctionPass, except they operate on VAST. Because they operate on VAST, not
// the LLVM IR, VASTModulePass are not allowed to modify the LLVM IR. Due to this
// limitation, the VASTModulePass class takes care of declaring that no LLVM
// passes are invalidated.
//
//===----------------------------------------------------------------------===//

#ifndef SHANG_VASTMODULE_PASS_H
#define SHANG_VASTMODULE_PASS_H

#include "llvm/Pass.h"

namespace llvm {
class VASTModule;
class Function;

/// ScopPass - This class adapts the FunctionPass interface to allow convenient
/// creation of passes that operate on the VAST. Instead of overriding
/// runOnFunction, subclasses override runOnVASTModule.
class VASTModulePass : public FunctionPass {
protected:
  explicit VASTModulePass(char &ID);

  /// rebuildModule - Delete the current VASTModule and rebuild it from scratch.
  /// this is supposed to be used by the driver of the iterative scheduling only.
  ///
  VASTModule &rebuildModule();

public:
  /// runOnVASTModule - This method must be overloaded to perform the
  /// desired VAST transformation or analysis.
  ///
  virtual bool runOnVASTModule(VASTModule &VM) = 0;

  /// getAnalysisUsage - Subclasses that override getAnalysisUsage
  /// must call this.
  ///
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;

  /// print - Print the status of the pass.
  ///
  virtual void print(raw_ostream &OS, const Module *) const {
    print(OS);
  }

  virtual void print(raw_ostream &OS) const;

private:
  bool runOnFunction(Function &F);

};
}

#endif
