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

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Pass.h"

using namespace llvm;

HLSAllocation::MemBank::MemBank(unsigned Number, unsigned WordSizeInBytes,
                                unsigned AddrWdith, bool RequireByteEnable,
                                bool IsReadOnly)
  : Number(Number), WordSizeInBytes(WordSizeInBytes), AddrWidth(AddrWdith),
    RequireByteEnable(RequireByteEnable), IsReadOnly(IsReadOnly) {}

unsigned HLSAllocation::getMemoryBankNum(const StoreInst &I) const {
  assert(Allocation
         && "Allocation didn't call InitializeHLSAllocation in its run method!");
  return Allocation->getMemoryBankNum(I);
}

unsigned HLSAllocation::getMemoryBankNum(const LoadInst &I) const {
  assert(Allocation
         && "Allocation didn't call InitializeHLSAllocation in its run method!");
  return Allocation->getMemoryBankNum(I);
}

HLSAllocation::MemBank
HLSAllocation::getMemoryBank(const GlobalVariable &GV) const {
  assert(Allocation
         && "Allocation didn't call InitializeHLSAllocation in its run method!");
  return Allocation->getMemoryBank(GV);
}

unsigned HLSAllocation::getMemoryBankNum(const Value &V) const {
  if (const LoadInst *L = dyn_cast<LoadInst>(&V))
    return getMemoryBankNum(*L);

  if (const StoreInst *S = dyn_cast<StoreInst>(&V))
    return getMemoryBankNum(*S);

  if (const GlobalVariable *G = dyn_cast<GlobalVariable>(&V))
    return getMemoryBank(*G).Number;

  return 0;
}

void HLSAllocation::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<HLSAllocation>();         // All HLSAllocation's chain
  AU.addRequired<DataLayout>();
}

void HLSAllocation::InitializeHLSAllocation(Pass *P) {
  TD = &P->getAnalysis<DataLayout>();
  Allocation = &P->getAnalysis<HLSAllocation>();
}

char HLSAllocation::ID = 0;

INITIALIZE_ANALYSIS_GROUP(HLSAllocation,
                          "High-level Synthesis Resource Allocation",
                          BasicAllocation)

namespace {
/// TODO: Implement the HLS allocation as analysis group.
/// HLSAllocation - The resource allocation interface.
struct BasicAllocation : public ImmutablePass, public HLSAllocation {
  static char ID;

  BasicAllocation();

  MemBank  getMemoryBank(const GlobalVariable &GV) const { return MemBank(); }
  unsigned getMemoryBankNum(const LoadInst &I) const { return 0; }
  unsigned getMemoryBankNum(const StoreInst &I) const { return 0; }

  ArrayRef<const GlobalVariable*> getBlockRAMAllocation(const Function *F) const {
    return ArrayRef<const GlobalVariable*>();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<DataLayout>();
  }

  void initializePass() {
    // Note: BasicAllocation does not call InitializeHLSAllocation because it's
    // special and does not support chaining.
    TD = &getAnalysis<DataLayout>();
  }

  /// getAdjustedAnalysisPointer - This method is used when a pass implements
  /// an analysis interface through multiple inheritance.  If needed, it
  /// should override this to adjust the this pointer as needed for the
  /// specified pass info.
  virtual void *getAdjustedAnalysisPointer(const void *ID) {
    if (ID == &HLSAllocation::ID)
      return (HLSAllocation*)this;
    return this;
  }
};
}

BasicAllocation::BasicAllocation() : ImmutablePass(ID) {
  initializeBasicAllocationPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_AG_PASS(BasicAllocation, HLSAllocation,
                   "shang-basic-resource-allocation",
                   "Basic Resource Allocation in High-level Synthesis",
                   false, true, true)

char BasicAllocation::ID = 0;
