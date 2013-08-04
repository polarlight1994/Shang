//===- SimpleBlockRAMAllocation.cpp - Simple Block RAM Allocation -*-C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The SimpleBlockRAMAllocation try to allocate block RAM whenever possible.
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
#include "llvm/ADT/ValueMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-simple-bram-allocation"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(NumLocalizedGV, "Number of GlobalVariable localized as BRAM");

static cl::opt<bool> NoSingleElementBRAM(
"shang-no-single-element-bram",
cl::desc("Forbid the SimpleBlockRAMAllocation allocate the single element bram"),
cl::init(true));

namespace {
struct SimpleBlockRAMAllocation : public ModulePass, public HLSAllocation {
  static char ID;

  SimpleBlockRAMAllocation() : ModulePass(ID) {
    initializeSimpleBlockRAMAllocationPass(*PassRegistry::getPassRegistry());
  }

  ValueMap<const Value*, unsigned>  Binding;
  ValueMap<const GlobalVariable*, MemBank>  Banks;

  // Look up the memory port allocation if the pointers are not allocated
  // to the BlockRAM.
  virtual unsigned getMemoryBankNum(const LoadInst &I) const {
    return Binding.lookup(I.getPointerOperand());
  }

  virtual unsigned getMemoryBankNum(const StoreInst &I) const {
    return Binding.lookup(I.getPointerOperand());
  }

  virtual MemBank getMemoryBank(const GlobalVariable &GV) const {
    return Banks.lookup(&GV);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    HLSAllocation::getAnalysisUsage(AU);
    AU.setPreservesAll();
  }

  bool runOnModule(Module &M);

  void localizeGV(GlobalVariable *GV);

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

struct GVUseCollector {
  SmallVector<Instruction*, 8> Uses;
  // Modify information, Is the GV written in a function?
  DenseSet<const Function*> WrittenFunctions;
  DenseSet<const Function*> VisitedFunctions;

  bool operator()(Value *ValUser, const Value *V)  {
    if (Instruction *I = dyn_cast<Instruction>(ValUser)) {
      switch (I->getOpcode()) {
      case Instruction::GetElementPtr:
        return true;
        // The pointer must use as pointer operand in load/store.
      case Instruction::Load:
        Uses.push_back(I);
        VisitedFunctions.insert(I->getParent()->getParent());
        return cast<LoadInst>(I)->getPointerOperand() == V;
      case Instruction::Store:
        Uses.push_back(I);
        WrittenFunctions.insert(I->getParent()->getParent());
        VisitedFunctions.insert(I->getParent()->getParent());
        return cast<StoreInst>(I)->getPointerOperand() == V;
      }
    } else if (const ConstantExpr *C = dyn_cast<ConstantExpr>(ValUser))
      if (C->getOpcode() == Instruction::GetElementPtr)
        return true;

    return onlyUsedByLifetimeMarkers(V);
  }

  static bool isLocalizedCandidate(GlobalVariable *GV) {
    // Cannot localize if the GV may be modified by others module.
    if (!GV->hasInternalLinkage() && !GV->hasPrivateLinkage() &&
        (!GV->isConstant() || GV->isDeclaration()))
      return false;

    assert(GV->hasInitializer() && "Unexpected declaration!");

    return true;
  }

  bool canBeLocalized() const {
    return VisitedFunctions.size() == 1 || WrittenFunctions.empty();
  }
};
}
//===----------------------------------------------------------------------===//
template <typename VisitFunc>
static bool visitPtrUseTree(Value *BasePtr, VisitFunc &Visitor) {
  typedef Instruction::use_iterator ChildIt;
  typedef SmallVector<std::pair<Value*, ChildIt>, 16> StackTy;
  SmallPtrSet<Value*, 8> Visited;

  StackTy Stack;
  Stack.push_back(std::make_pair(BasePtr, BasePtr->use_begin()));

  while (!Stack.empty()) {
    Value *CurVal = Stack.back().first;
    ChildIt &CurChildIt = Stack.back().second;

    // All children of the current instruction visited, visit the current
    // instruction.
    if (CurChildIt == CurVal->use_end()) {
      Stack.pop_back();
      continue;
    }

    Value *Child = *CurChildIt;
    ++CurChildIt;
    // Had us visited this node yet?
    if (!Visited.insert(Child)) continue;

    if (!Visitor(Child, CurVal)) return false;

    // Don't trace the loaded value.
    if (isa<LoadInst>(Child)) continue;

    Stack.push_back(std::make_pair(Child, Child->use_begin()));
  }

  return true;
}

INITIALIZE_AG_PASS(SimpleBlockRAMAllocation, HLSAllocation,
                   "simple-block-ram-allocation", "Simple BlockRAM Allocation",
                   false, true, false)

char SimpleBlockRAMAllocation::ID = 0;

Pass *llvm::createSimpleBlockRAMAllocationPass() {
  return new SimpleBlockRAMAllocation();
}

bool SimpleBlockRAMAllocation::runOnModule(Module &M) {
  InitializeHLSAllocation(this);

  typedef Module::global_iterator global_iterator;
  for (global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I)
    localizeGV(I);

  return false;
}

void SimpleBlockRAMAllocation::localizeGV(GlobalVariable *GV) {
  if (!GVUseCollector::isLocalizedCandidate(GV)) return;

  GVUseCollector Collector;

  if (!visitPtrUseTree(GV, Collector)) return;

  if (!Collector.canBeLocalized()) return;

  Type *ElemTy = cast<PointerType>(GV->getType())->getElementType();

  if (!isa<ArrayType>(ElemTy) && NoSingleElementBRAM) {
    for (unsigned i = 0, e = Collector.Uses.size(); i != e; ++i) {
      Instruction *I = dyn_cast<Instruction>(Collector.Uses[i]);

      if (I == 0 || !isa<StoreInst>(I)) continue;

      // It is ok if the GV is only written in the return block, so that we do not
      // need to Worry about SSA form at all.
      if (!isa<ReturnInst>(I->getParent()->getTerminator()))
        return;
    }
  }

  unsigned ElementSizeInBytes = TD->getTypeStoreSize(ElemTy);
  // Calculate the size of the object.
  unsigned NumElem = 1;

  // Try to expand multi-dimension array to single dimension array.
  while (const ArrayType *AT = dyn_cast<ArrayType>(ElemTy)) {
    ElemTy = AT->getElementType();
    NumElem *= AT->getNumElements();
  }

  ElementSizeInBytes = TD->getTypeStoreSize(ElemTy);
  unsigned ObjectSizeInBytes = NumElem * ElementSizeInBytes;

  // Allocate the bank for GV.
  unsigned CurNum = Banks.size() + 1;
  MemBank Bank(CurNum, ElementSizeInBytes, Log2_32_Ceil(ObjectSizeInBytes),
               false);
  Banks[GV] = Bank;

  // Remember the assignment for the load/stores.
  while (!Collector.Uses.empty()) {
    Instruction *I = Collector.Uses.pop_back_val();
    Binding[getPointerOperand(I)] = CurNum;
  }

  ++NumLocalizedGV;
}
