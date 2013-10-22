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

  struct MemBank {
    uint8_t Number;
    uint8_t WordSizeInBytes;
    uint8_t AddrWidth;
    bool RequireByteEnable : 1;
    bool IsReadOnly        : 1;

    MemBank(unsigned Number = 0, unsigned WordSizeInBytes = 0,
            unsigned AddrWdith = 0, bool RequireByteEnable = true,
            bool IsReadOnly = false);
  };

  // Memory Bank allocation queries.
  virtual MemBank  getMemoryBank(const GlobalVariable &GV) const;
  virtual unsigned getMemoryBankNum(const LoadInst &I) const;
  virtual unsigned getMemoryBankNum(const StoreInst &I) const;
  unsigned getMemoryBankNum(const Value &V) const;
};
}

#endif
