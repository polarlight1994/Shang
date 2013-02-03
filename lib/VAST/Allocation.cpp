//===- Allocation.cpp - High-level Synthesis Resource Allocation -*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the resource allocation interface in the Shang high-lvel
// synthesis framework.
//
//===----------------------------------------------------------------------===//

#include "Allocation.h"

#include "shang/Passes.h"
#include "shang/FUInfo.h"

using namespace llvm;

HLSAllocation::HLSAllocation() : ImmutablePass(ID) {
  initializeHLSAllocationPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS(HLSAllocation, "shang-resource-allocation",
                "Resource Allocation in High-level Synthesis",
                false, true)

char HLSAllocation::ID = 0;

FuncUnitId HLSAllocation::getMemoryPort(StoreInst &I) const {
  return FuncUnitId(VFUs::MemoryBus, 0);
}

FuncUnitId HLSAllocation::getMemoryPort(LoadInst &I) const {
  return FuncUnitId(VFUs::MemoryBus, 0);
}
