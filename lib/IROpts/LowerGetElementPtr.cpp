//===- LowerGetElementPtr.cpp - Lower GEP to integer arithmetic -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the LowerGetElementPtr pass. This pass translate geps
// to integer arithmetic operation while not confusing the LLVM's Alias
// Analysis
//
//===----------------------------------------------------------------------===//
#include "vast/Passes.h"

#include "llvm/Pass.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"

#define DEBUG_TYPE "vast-lower-gep"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct LowerGetElementPtr : public BasicBlockPass {
  static char ID;
  LowerGetElementPtr() : BasicBlockPass(ID) {
    initializeLowerGetElementPtrPass(*PassRegistry::getPassRegistry());
  }

  bool runOnBasicBlock(BasicBlock &BB);
  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<DataLayout>();
  }

  bool lowerGetElementPtr(GetElementPtrInst *Inst, DataLayout &TD);
};
}

char LowerGetElementPtr::ID = 0;
INITIALIZE_PASS_BEGIN(LowerGetElementPtr, "vast-lower-gep",
                      "Lower GetElementPtr to integer arithmetic", false, false)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(LowerGetElementPtr, "vast-lower-gep",
                    "Lower GetElementPtr to integer arithmetic", false, false)

Pass *llvm::createLowerGetElementPtrPass() {
  return new LowerGetElementPtr();
}

bool LowerGetElementPtr::runOnBasicBlock(BasicBlock &BB) {
  bool AnyChange = false;

  DataLayout &TD = getAnalysis<DataLayout>();

  typedef BasicBlock::iterator iterator;
  for (iterator I = BB.begin(), E = BB.end(); I != E; /*++I*/)
    if (GetElementPtrInst *Inst = dyn_cast<GetElementPtrInst>(I++))
      AnyChange |= lowerGetElementPtr(Inst, TD);
  
  return AnyChange;  
}

// A GetElementPtrInst is trivial if it has more than 1 non-constant indices.
static bool isTrivial(GEPOperator *O) {
  bool HasNonconst = false;

  typedef GEPOperator::op_iterator op_iterator;
  for (op_iterator OI = O->idx_begin(), E = O->op_end(); OI != E; ++OI) {
    const Value *Idx = *OI;
    if (!isa<ConstantInt>(Idx))  {
      if (HasNonconst)
        return false;

      HasNonconst = true;
    }
    
  }

  return true;
}

bool
LowerGetElementPtr::lowerGetElementPtr(GetElementPtrInst *Inst, DataLayout &TD) {
  GEPOperator *O = cast<GEPOperator>(Inst);
  if (isTrivial(O))
    return false;

  Value *Ptr = O->getPointerOperand();
  SmallVector<Value*, 4> NewIndices;

  // Note that the pointer operand may be a vector of pointers. Take the scalar
  // element which holds a pointer.
  Type *Ty = O->getPointerOperandType()->getScalarType();
  IntegerType *IntPtrTy
    = TD.getIntPtrType(Ty->getContext(), O->getPointerAddressSpace());

  Value *Offset = ConstantInt::getNullValue(IntPtrTy);
  IRBuilder<> Builder(Inst);

  typedef GEPOperator::op_iterator op_iterator;
  for (op_iterator OI = O->idx_begin(), E = O->op_end(); OI != E; ++OI) {
    Value *Idx = *OI;
    // Replace the index by zero, we will put the offset at the last index.
    NewIndices.push_back(ConstantInt::getNullValue(IntPtrTy));

    if (StructType *StTy = dyn_cast<StructType>(Ty)) {
      unsigned Field = cast<ConstantInt>(Idx)->getZExtValue();
      if (Field) {
        // N = N + Offset
        uint64_t FieldOffset = TD.getStructLayout(StTy)->getElementOffset(Field);
        Offset
          = Builder.CreateAdd(Offset, ConstantInt::get(IntPtrTy, FieldOffset));
      }

      Ty = StTy->getElementType(Field);
    } else {
      Ty = cast<SequentialType>(Ty)->getElementType();

      // If this is a constant subscript, handle it quickly.
      if (const ConstantInt *CI = dyn_cast<ConstantInt>(Idx)) {
        if (CI->isZero()) continue;
        uint64_t SubScriptOffset
          = TD.getTypeAllocSize(Ty) * cast<ConstantInt>(CI)->getSExtValue();

        Offset
          = Builder.CreateAdd(Offset, ConstantInt::get(IntPtrTy, SubScriptOffset));
        continue;
      }

      // N = N + Idx * ElementSize;
      APInt ElementSize(IntPtrTy->getBitWidth(), TD.getTypeAllocSize(Ty));

      // If this is a multiply by a power of two, turn it into a shl
      // immediately.  This is a very common case.
      if (ElementSize != 1) {
        if (ElementSize.isPowerOf2()) {
          unsigned Amt = ElementSize.logBase2();
          Idx = Builder.CreateShl(Idx, ConstantInt::get(IntPtrTy, Amt));
        } else
          Idx = Builder.CreateMul(Idx, ConstantInt::get(IntPtrTy, ElementSize));
      }

      Offset = Builder.CreateAdd(Offset, Idx);
    }
  }

  // We need to translate the byte address back to the "element subscript"
  // according to the definition of GetElementPtrInst.
  APInt ElementSize(IntPtrTy->getBitWidth(), TD.getTypeAllocSize(Ty));
  assert(ElementSize.isPowerOf2() && "Unexpected ElementSize!");
  Offset = Builder.CreateAShr(Offset, ElementSize.logBase2());

  // Replace the last indices by the offset computed by integer arithmetic.
  NewIndices.back() = Offset;

  Value *NewGEP = Builder.CreateGEP(Ptr, NewIndices);
  Inst->replaceAllUsesWith(NewGEP);

  return true;
}
