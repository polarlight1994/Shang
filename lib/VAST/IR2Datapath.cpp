//===------------ IR2Datapath.cpp - LLVM IR <-> VAST ------------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the classes which convert LLVM IR to VASTExprs.
//
//===----------------------------------------------------------------------===//

#include "IR2Datapath.h"

#include "llvm/IR/DataLayout.h"

using namespace llvm;

VASTValPtr DatapathBuilderContext::lookupExpr(Value *Val) const {
  ValueMapTy::const_iterator at = Value2Expr.find(Val);
  return at == Value2Expr.end() ? 0 : at->second;
}

VASTValPtr DatapathBuilderContext::indexVASTExpr(Value *Val, VASTValPtr V) {
  bool inserted = Value2Expr.insert(std::make_pair(Val, V)).second;
  assert(inserted && "RegNum already indexed some value!");

  return V;
}

unsigned DatapathBuilderContext::getValueSizeInBits(const Value *V) const {
  unsigned SizeInBits = TD->getTypeSizeInBits(V->getType());
  assert(SizeInBits && "Size of V is unknown!");
  return SizeInBits;
}

VASTValPtr DatapathBuilder::visitTruncInst(TruncInst &I) {
  // Truncate the value by bitslice expression.
  return buildBitSliceExpr(getAsOperand(I.getOperand(0)),
                           getValueSizeInBits(I), 0);
}

VASTValPtr DatapathBuilder::visitZExtInst(ZExtInst &I) {
  unsigned NumBits = getValueSizeInBits(I);

  // Do not mess up with the big Value generated by ScalarRelAggregates pass.
  if (NumBits > 64) return VASTValPtr();

  VASTValPtr Operand = getAsOperand(I.getOperand(0));
  return buildZExtExpr(Operand, NumBits);
}

VASTValPtr DatapathBuilder::visitSExtInst(SExtInst &I) {
  unsigned NumBits = getValueSizeInBits(I);

  // Do not mess up with the big Value generated by ScalarRelAggregates pass.
  if (NumBits > 64) return VASTValPtr();

  VASTValPtr Operand = getAsOperand(I.getOperand(0));
  return buildSExtExpr(Operand, NumBits);
}

VASTValPtr DatapathBuilder::visitBitCastInst(BitCastInst &I) {
  VASTValPtr Operand = getAsOperand(I.getOperand(0));

  assert((!Operand || getValueSizeInBits(I) == Operand->getBitWidth())
         && "Cast between types with different size found!");
  return Operand;
}

VASTValPtr DatapathBuilder::visitSelectInst(SelectInst &I) {
  return buildExpr(VASTExpr::dpSel,
                   getAsOperand(I.getOperand(0)),
                   getAsOperand(I.getOperand(1)),
                   getAsOperand(I.getOperand(2)),
                   getValueSizeInBits(I));
}

VASTValPtr DatapathBuilder::visitICmpInst(ICmpInst &I) {
  VASTValPtr LHS = getAsOperand(I.getOperand(0)),
             RHS = getAsOperand(I.getOperand(1));

  switch (I.getPredicate()) {
  case CmpInst::ICMP_NE:  return buildNE(LHS, RHS);
  case CmpInst::ICMP_EQ:  return buildEQ(LHS, RHS);

  case CmpInst::ICMP_SLT:
    std::swap(LHS, RHS);
    // Fall though.
  case CmpInst::ICMP_SGT:
    return buildExpr(VASTExpr::dpSGT, LHS, RHS, 1);
  case CmpInst::ICMP_ULT:
    std::swap(LHS, RHS);
    // Fall though.
  case CmpInst::ICMP_UGT:
    return buildExpr(VASTExpr::dpUGT, LHS, RHS, 1);

  case CmpInst::ICMP_SLE:
    std::swap(LHS, RHS);
    // Fall though.
  case CmpInst::ICMP_SGE:
    return buildExpr(VASTExpr::dpUGT, LHS, RHS, 1);

  case CmpInst::ICMP_ULE:
    std::swap(LHS, RHS);
    // Fall though.
  case CmpInst::ICMP_UGE:
    return buildExpr(VASTExpr::dpUGE, LHS, RHS, 1);

  default: llvm_unreachable("Unexpected ICmp predicate!"); break;
  }

  return 0;
}

VASTValPtr DatapathBuilder::visitBinaryOperator(BinaryOperator &I) {
  unsigned NumBits = getValueSizeInBits(I);

  // Do not mess up with the big Value generated by ScalarRelAggregates pass.
  if (NumBits > 64) return VASTValPtr();

  VASTValPtr Ops[] = { getAsOperand(I.getOperand(0)),
                       getAsOperand(I.getOperand(1))};

  // FIXME: Do we need to care about NSW and NUW?
  switch (I.getOpcode()) {
  case Instruction::Add: return buildAddExpr(Ops, NumBits);
  case Instruction::Sub: {
    // A - B is equivalent to A + ~(B) + 1
    VASTValPtr SubOps[] = { Ops[0],
                            buildNotExpr(Ops[1]),
                            getOrCreateImmediate(1, 1) };
    return buildAddExpr(SubOps, NumBits);
  }
  case Instruction::Mul: return buildMulExpr(Ops, NumBits);

  case Instruction::Shl: return buildExpr(VASTExpr::dpShl, Ops, NumBits);
  case Instruction::AShr: return buildExpr(VASTExpr::dpSRA, Ops, NumBits);
  case Instruction::LShr: return buildExpr(VASTExpr::dpSRL, Ops, NumBits);

  // Div is implemented as submodule.
  case Instruction::SRem:
  case Instruction::URem:
  case Instruction::UDiv:
  case Instruction::SDiv: return VASTValPtr();

  case Instruction::And:  return buildAndExpr(Ops, NumBits);
  case Instruction::Or:   return buildOrExpr(Ops, NumBits);
  case Instruction::Xor:  return buildXorExpr(Ops, NumBits);
  default: llvm_unreachable("Unexpected opcode!"); break;
  }

  return VASTValPtr();
}

VASTValPtr DatapathBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  VASTValPtr Ptr = getAsOperand(I.getOperand(0));
  // FIXME: All the pointer arithmetic are perform under the precision of
  // PtrSize, do we need to perform the arithmetic at the max avilable integer
  // width and truncate the resunt?
  unsigned PtrSize = Ptr->getBitWidth();
  // Note that the pointer operand may be a vector of pointers. Take the scalar
  // element which holds a pointer.
  Type *Ty = I.getOperand(0)->getType()->getScalarType();

  for (GetElementPtrInst::const_op_iterator OI = I.op_begin()+1, E = I.op_end();
       OI != E; ++OI) {
    const Value *Idx = *OI;
    if (StructType *StTy = dyn_cast<StructType>(Ty)) {
      unsigned Field = cast<ConstantInt>(Idx)->getZExtValue();
      if (Field) {
        // N = N + Offset
        uint64_t Offset
          = getDataLayout()->getStructLayout(StTy)->getElementOffset(Field);
        Ptr = buildExpr(VASTExpr::dpAdd,
                        Ptr, getOrCreateImmediate(Offset, PtrSize),
                        PtrSize);
      }

      Ty = StTy->getElementType(Field);
    } else {
      Ty = cast<SequentialType>(Ty)->getElementType();

      // If this is a constant subscript, handle it quickly.
      if (const ConstantInt *CI = dyn_cast<ConstantInt>(Idx)) {
        if (CI->isZero()) continue;
        uint64_t Offs = getDataLayout()->getTypeAllocSize(Ty)
                        * cast<ConstantInt>(CI)->getSExtValue();
        
        Ptr = buildExpr(VASTExpr::dpAdd,
                        Ptr, getOrCreateImmediate(Offs, PtrSize),
                        PtrSize);
        continue;
      }

      // N = N + Idx * ElementSize;
      APInt ElementSize = APInt(PtrSize, getDataLayout()->getTypeAllocSize(Ty));
      VASTValPtr IdxN = getAsOperand(const_cast<Value*>(Idx));

      // If the index is smaller or larger than intptr_t, truncate or extend
      // it.
      IdxN = buildBitSliceExpr(IdxN, PtrSize, 0);

      // If this is a multiply by a power of two, turn it into a shl
      // immediately.  This is a very common case.
      if (ElementSize != 1) {
        if (ElementSize.isPowerOf2()) {
          unsigned Amt = ElementSize.logBase2();
          IdxN = buildShiftExpr(VASTExpr::dpShl, IdxN,
                                getOrCreateImmediate(Amt, PtrSize),
                                PtrSize);
        } else {
          VASTValPtr Scale = getOrCreateImmediate(ElementSize.getSExtValue(),
                                                  PtrSize);
          IdxN = buildExpr(VASTExpr::dpMul, IdxN, Scale, PtrSize);
        }
      }
      
      Ptr = buildExpr(VASTExpr::dpAdd, Ptr, IdxN, PtrSize);
    }
  }

  return Ptr;
}
