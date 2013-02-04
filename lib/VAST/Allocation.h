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

#include "llvm/Pass.h"

namespace llvm {
class VASTModule;
class StoreInst;
class LoadInst;
class FuncUnitId;

/// TODO: Implement the HLS allocation as analysis group.
/// HLSAllocation - The resource allocation interface.
class HLSAllocation : public ImmutablePass {
public:
  static char ID;

  HLSAllocation();

  FuncUnitId getMemoryPort(LoadInst &I) const;
  FuncUnitId getMemoryPort(StoreInst &I) const;
};

}

#endif
