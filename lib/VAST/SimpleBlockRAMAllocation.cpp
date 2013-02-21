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
  ValueMap<const Value*, FuncUnitId> BlockRAMBinding;
  typedef ValueMap<const Function*, SmallVector<const GlobalVariable*, 4> >
  BRAMMap;
  BRAMMap AllocatedBRAMs;

  SimpleBlockRAMAllocation() : ModulePass(ID) {
    initializeSimpleBlockRAMAllocationPass(*PassRegistry::getPassRegistry());
  }

  template<typename T>
  FuncUnitId getMemoryPortImpl(const T &V) const {
    FuncUnitId ID = BlockRAMBinding.lookup(&V);

    if (!ID.isTrivial()) return ID;

    return HLSAllocation::getMemoryPort(V);
  }

  FuncUnitId getMemoryPort(const LoadInst &I) const {
    return getMemoryPortImpl(I);
  }

  FuncUnitId getMemoryPort(const StoreInst &I) const {
    return getMemoryPortImpl(I);
  }

  FuncUnitId getMemoryPort(const GlobalVariable &GV) const {
    return getMemoryPortImpl(GV);
  }

  ArrayRef<const GlobalVariable*> getBRAMAllocation(const Function *F) const {
    BRAMMap::const_iterator I = AllocatedBRAMs.find(F);
    return I == AllocatedBRAMs.end() ? ArrayRef<const GlobalVariable*>()
                                     : I->second;
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
  SmallVector<Value*, 8> Uses;
  // Modify information, Is the GV written in a function?
  DenseSet<const Function*> WrittenFunctions;
  DenseSet<const Function*> VisitedFunctions;
  typedef DenseSet<const Function*>::const_iterator fn_iterator;
  fn_iterator fn_begin() const { return VisitedFunctions.begin(); }
  fn_iterator fn_end() const { return VisitedFunctions.end(); }

  bool operator()(Value *ValUser, const Value *V)  {
    if (Instruction *I = dyn_cast<Instruction>(ValUser)) {
      switch (I->getOpcode()) {
      case Instruction::GetElementPtr:
        return true;
        // The pointer must use as pointer operand in load/store.
      case Instruction::Load:
        Uses.push_back(ValUser);
        VisitedFunctions.insert(I->getParent()->getParent());
        return cast<LoadInst>(I)->getPointerOperand() == V;
      case Instruction::Store:
        Uses.push_back(ValUser);
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

  if (!isa<ArrayType>(GV->getType()->getElementType()) && NoSingleElementBRAM) {
    for (unsigned i = 0, e = Collector.Uses.size(); i != e; ++i) {
      Instruction *I = dyn_cast<Instruction>(Collector.Uses[i]);

      if (I == 0 || !isa<StoreInst>(I)) continue;

      // It is ok if the GV is only written in the return block, so that we do not
      // need to Worry about SSA form at all.
      if (!isa<ReturnInst>(I->getParent()->getTerminator()))
        return;
    }
  }

  // Allocate the FUID for the GV.
  FuncUnitId ID = FuncUnitId(VFUs::BRam, BlockRAMBinding.size());
  BlockRAMBinding[GV] = ID;

  // Remember the assignment for the load/stores.
  while (!Collector.Uses.empty()) {
    Value *V = Collector.Uses.pop_back_val();
    BlockRAMBinding[V] = ID;
  }

  // Remember the allocation for each function.
  typedef GVUseCollector::fn_iterator fn_iterator;
  for (fn_iterator I = Collector.fn_begin(), E = Collector.fn_end();
       I != E; ++I)
    AllocatedBRAMs[*I].push_back(GV);

  ++NumLocalizedGV;
}
