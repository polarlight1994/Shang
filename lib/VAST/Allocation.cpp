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

#include "vast/Passes.h"
#include "vast/FUInfo.h"
#include "vast/VASTModule.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Pass.h"

using namespace llvm;

VASTMemoryBank *HLSAllocation::getMemoryBank(const StoreInst &I) const {
  assert(Allocation
         && "Allocation didn't call InitializeHLSAllocation in its run method!");
  return Allocation->getMemoryBank(I);
}

VASTMemoryBank *HLSAllocation::getMemoryBank(const LoadInst &I) const {
  assert(Allocation
         && "Allocation didn't call InitializeHLSAllocation in its run method!");
  return Allocation->getMemoryBank(I);
}

VASTMemoryBank *HLSAllocation::getMemoryBank(const GlobalVariable &GV) const {
  assert(Allocation
         && "Allocation didn't call InitializeHLSAllocation in its run method!");
  return Allocation->getMemoryBank(GV);
}

VASTMemoryBank *HLSAllocation::getMemoryBank(const Value &V) const {
  if (const LoadInst *L = dyn_cast<LoadInst>(&V))
    return getMemoryBank(*L);

  if (const StoreInst *S = dyn_cast<StoreInst>(&V))
    return getMemoryBank(*S);

  if (const GlobalVariable *G = dyn_cast<GlobalVariable>(&V))
    return getMemoryBank(*G);

  return NULL;
}

void HLSAllocation::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<HLSAllocation>();         // All HLSAllocation's chain
  AU.addRequired<DataLayout>();
}

void HLSAllocation::InitializeHLSAllocation(Pass *P) {
  TD = &P->getAnalysis<DataLayout>();
  Allocation = &P->getAnalysis<HLSAllocation>();
  M = Allocation->M;
}

HLSAllocation::~HLSAllocation() {
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

  VASTMemoryBank *getMemoryBank(const GlobalVariable &GV) const { return NULL; }
  VASTMemoryBank *getMemoryBankNum(const LoadInst &I) const { return NULL; }
  VASTMemoryBank *getMemoryBankNum(const StoreInst &I) const { return NULL; }

  ArrayRef<const GlobalVariable*> getBlockRAMAllocation(const Function *F) const {
    return ArrayRef<const GlobalVariable*>();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<DataLayout>();
  }

  void addCommonPorts() {
     M->addInputPort("clk", 1, VASTModule::Clk);
     M->addInputPort("rstN", 1, VASTModule::RST);
     M->addInputPort("start", 1, VASTModule::Start);
     M->addOutputPort("fin", 1, VASTModule::Finish);
  }

  void initializePass() {
    // Note: BasicAllocation does not call InitializeHLSAllocation because it's
    // special and does not support chaining.
    TD = &getAnalysis<DataLayout>();
    // Create the module.
    M = new VASTModule();
    // Create the common ports for a module.
    addCommonPorts();
  }

  ~BasicAllocation() {
    delete M;
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
