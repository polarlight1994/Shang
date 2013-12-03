//===-- Allocation.h - High-level Synthesis Resource Allocation --*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the resource allocation interface in the VAST high-lvel
// synthesis framework.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_ALLOCATION_H
#define VAST_ALLOCATION_H

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class Value;
class GlobalVariable;
class StoreInst;
class LoadInst;
class VASTModule;
class VASTMemoryBus;
class Function;
class FuncUnitId;
class DataLayout;
class Pass;
class AnalysisUsage;

class HLSAllocation {
  // Previous HLSAllocation to chain to.
  HLSAllocation *Allocation;

  // HLSAllocation also hold the pointer to the hardware module.
  VASTModule *M;
protected:
  void createModule();

  const DataLayout *TD;

  HLSAllocation() : Allocation(0), M(0), TD(0) {}

  void InitializeHLSAllocation(Pass *P);

  /// getAnalysisUsage - All HLSAllocation implementations should invoke this
  /// directly (using HLSAllocation::getAnalysisUsage(AU)).
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
public:
  static char ID;

  virtual ~HLSAllocation();

  VASTModule &getModule() const { return *M; }

  // Memory Bank allocation queries.
  virtual VASTMemoryBus *getMemoryBank(const GlobalVariable &GV) const;
  virtual VASTMemoryBus *getMemoryBank(const LoadInst &I) const;
  virtual VASTMemoryBus *getMemoryBank(const StoreInst &I) const;
  VASTMemoryBus *getMemoryBank(const Value &V) const;
};
}

#endif
