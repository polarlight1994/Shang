//===-- Allocation.h - High-level Synthesis Resource Allocation --*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the resource allocation interface in the Shang high-lvel
// synthesis framework.
//
//===----------------------------------------------------------------------===//

#ifndef SHANG_ALLOCATION_H
#define SHANG_ALLOCATION_H

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class Value;
class GlobalVariable;
class StoreInst;
class LoadInst;
class Function;
class FuncUnitId;
class DataLayout;
class Pass;
class AnalysisUsage;

class HLSAllocation {
  // Previous HLSAllocation to chain to.
  HLSAllocation *Allocation;

protected:
  const DataLayout *TD;

  HLSAllocation() : Allocation(0), TD(0) {}

  void InitializeHLSAllocation(Pass *P);

  /// getAnalysisUsage - All HLSAllocation implementations should invoke this
  /// directly (using HLSAllocation::getAnalysisUsage(AU)).
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
public:
  static char ID;

  virtual ~HLSAllocation() {}

  virtual FuncUnitId getMemoryPort(const LoadInst &I) const;
  virtual FuncUnitId getMemoryPort(const StoreInst &I) const;
  virtual FuncUnitId getMemoryPort(const GlobalVariable &GV) const;

  FuncUnitId getMemoryPort(const Value &V) const;

  virtual ArrayRef<const GlobalVariable*>
  getBRAMAllocation(const Function *F) const;
};
}

#endif
