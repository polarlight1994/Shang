//===-------------- MemoryPartition.cpp - MemoryPartition ---------*-C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The MemoryPartition try to allocate the memory ports.
//
//===----------------------------------------------------------------------===//

#include "Allocation.h"

#include "shang/Utilities.h"
#include "shang/Passes.h"
#include "shang/FUInfo.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/ADT/ValueMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-memory-partition"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumMemoryAccess, "Number of memory accesses");
STATISTIC(NumLoad, "Number of Load");
STATISTIC(NumStore, "Number of Store");
STATISTIC(NumMemBanks, "Number of Local Memory Bank Allocated");

namespace {
struct MemoryPartition : public FunctionPass, public HLSAllocation {
  static char ID;
  
  ValueMap<const Value*, FuncUnitId>  Allocation;

  // Look up the memory port allocation if the pointers are not allocated
  // to the BlockRAM.
  virtual FuncUnitId getMemoryPort(const LoadInst &I) const {
    FuncUnitId ID = HLSAllocation::getMemoryPort(I);

    if (ID.getFUType() == VFUs::BRam) return ID;

    return Allocation.lookup(I.getPointerOperand());
  }

  virtual FuncUnitId getMemoryPort(const StoreInst &I) const {
    FuncUnitId ID = HLSAllocation::getMemoryPort(I);

    if (ID.getFUType() == VFUs::BRam) return ID;

    return Allocation.lookup(I.getPointerOperand());
  }

  virtual FuncUnitId getMemoryPort(const GlobalVariable &GV) const {
    FuncUnitId ID = HLSAllocation::getMemoryPort(GV);

    if (ID.getFUType() == VFUs::BRam) return ID;

    return Allocation.lookup(&GV);
  }

  MemoryPartition() : FunctionPass(ID) {
    initializeMemoryPartitionPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    HLSAllocation::getAnalysisUsage(AU);
    AU.addRequired<AliasAnalysis>();
    AU.setPreservesAll();
  }

  bool runOnFunction(Function &F);

  void releaseMemory() { Allocation.clear(); }

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
//===----------------------------------------------------------------------===//

INITIALIZE_AG_PASS_BEGIN(MemoryPartition, HLSAllocation,
                         "memory-partition", "Memory Partition",
                         false, true, false)
  INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
INITIALIZE_AG_PASS_END(MemoryPartition, HLSAllocation,
                       "memory-partition", "Memory Partition",
                       false, true, false)

char MemoryPartition::ID = 0;

Pass *llvm::createMemoryPartitionPass() {
  return new MemoryPartition();
}

bool MemoryPartition::runOnFunction(Function &F) {
  InitializeHLSAllocation(this);

  Module *M = F.getParent();

  // Make sure we have only 1 function.
#ifndef NDEBUG
  {
    Function *TopFunction = 0;
    for (Module::iterator I = M->begin(), E = M->end(); I != E; ++I) {
      Function *F = I;

      if (!F->use_empty() || F->isDeclaration())  continue;

      // Ignore the dead functions.
      if (!F->hasExternalLinkage()) continue;

      assert(TopFunction == 0 && "More than 1 top functions!");
      TopFunction = F;
    }
  }
#endif
  
  AliasSetTracker AST(getAnalysis<AliasAnalysis>());

  typedef Module::global_iterator iterator;
  for (iterator I = M->global_begin(), E = M->global_end(); I != E; ++I) {
    GlobalVariable *GV = I;
    // Ignore the block rams that is already assign to block RAM.
    if (HLSAllocation::getMemoryPort(*GV).getFUType() == VFUs::BRam)
      continue;

    AST.add(GV, AliasAnalysis::UnknownSize, 0);
  }

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    Instruction *Inst = &*I;
    assert(!isCall(Inst) && "MemoryPartition can only run after goto-expansion!");

    if (!isLoadStore(Inst)) continue;

    // Ignore the block RAM accesses.
    VFUs::FUTypes T = HLSAllocation::getMemoryPort(*Inst).getFUType();

    if (T == VFUs::BRam) continue;

    ++NumMemoryAccess;

    if (Inst->mayWriteToMemory()) ++NumStore;
    else                          ++NumLoad;

    AST.add(Inst);
  }

  unsigned CurPortNum = 1;

  for (AliasSetTracker::iterator I = AST.begin(), E = AST.end(); I != E; ++I) {
    AliasSet *AS = I;

    // Ignore the set that does not contain any load/store.
    if (AS->isForwardingAliasSet() || !(AS->isMod() || AS->isRef()))
      continue;

    bool AllocateNewPort = true;
    SmallVector<Value*, 8> Pointers;

    for (AliasSet::iterator AI = AS->begin(), AE = AS->end(); AI != AE; ++AI) {
      Value *V = AI.getPointer();
      Pointers.push_back(V);
      // Do not allocate local memory port if the pointers alias with external
      // global variables.
      if (GlobalVariable *GV = dyn_cast<GlobalVariable>(V))
        AllocateNewPort &= GV->hasInternalLinkage() || GV->hasPrivateLinkage();
    }

    unsigned Num = F.getName() != "main" ? 0 : 1;//AllocateNewPort ? CurPortNum++ : 0;

    // Create the allocation.
    while (!Pointers.empty()) {
      FuncUnitId Id = FuncUnitId(VFUs::MemoryBus, Num);
      Value *Ptr = Pointers.pop_back_val();

      if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Ptr))
        dbgs() << "Assign " << *GV << " to Memory #" << Num << "\n";

      bool inserted = Allocation.insert(std::make_pair(Ptr, Id)).second;
      assert(inserted && "Allocation not inserted!");
      (void) inserted;
    }
  }

  NumMemBanks += (CurPortNum - 1);

  return false;
}

