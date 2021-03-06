//===--- SIRAllocation.h - High-level Synthesis Resource Allocation in SIR ---*- C++ -*-===//
//
//                                The SIR HLS framework                                    //
//
// This file is distributed under the University of Illinois Open Source License.
// See LICENSE.TXT for details.
//
//===-----------------------------------------------------------------------------------===//
//
// This file define the resource allocation interface in the SIR HLS framework.
//
//===-----------------------------------------------------------------------------------===//

#ifndef SIR_ALLOCATION_H
#define SIR_ALLOCATION_H

#include "sir/SIR.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"

namespace llvm {
class SIRAllocation : public ModulePass {
private:
  SIR *SM;
  DataLayout *TD;

public:
  static char ID;
  // Create a VirtualMB only to serve as a memory bank interface
  // between the hardware and software in HW/SW co-simulation.
  SIRMemoryBank *VirtualMB;

  SIRAllocation();

  SIR *getSIR() const { return SM; }
  DataLayout *getDataLayout() const { return TD; }

  // The map between value and memory bank
  ValueMap<const Value *, SIRMemoryBank *> Binding;

  // Look up the memory port allocation.
  SIRMemoryBank *getMemoryBank(const LoadInst &I) const {
    return Binding.lookup(I.getPointerOperand());
  }

  SIRMemoryBank *getMemoryBank(const StoreInst &I) const {
    return Binding.lookup(I.getPointerOperand());
  }

  SIRMemoryBank *getMemoryBank(const GlobalVariable &GV) const {
    return Binding.lookup(&GV);
  }

  SIRMemoryBank *getVirtualMemoryBank() const {
    assert(VirtualMB && "VirtualMB can only be called in HW/SW co-simulation!");
    return VirtualMB;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const;

  bool createSIRMemoryBank(AliasSet *AS, unsigned BankNum);

  bool runOnModule(Module &M);
  void runOnFunction(Function &F, AliasSetTracker &AST);

  operator SIR*() const { return SM; }
  SIR *operator->() const { return SM; }
};
}

#endif