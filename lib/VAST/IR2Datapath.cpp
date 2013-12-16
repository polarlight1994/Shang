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
#include "llvm/IR/Operator.h"

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

  VASTValPtr Operand = getAsOperand(I.getOperand(0));
  return buildZExtExpr(Operand, NumBits);
}

VASTValPtr DatapathBuilder::visitSExtInst(SExtInst &I) {
  unsigned NumBits = getValueSizeInBits(I);

  VASTValPtr Operand = getAsOperand(I.getOperand(0));
  return buildSExtExpr(Operand, NumBits);
}

VASTValPtr DatapathBuilder::visitBitCastInst(BitCastInst &I) {
  VASTValPtr Operand = getAsOperand(I.getOperand(0));

  assert((!Operand || getValueSizeInBits(I) == Operand->getBitWidth())
         && "Cast between types with different size found!");
  return Operand;
}

VASTValPtr DatapathBuilder::visitPtrToIntInst(PtrToIntInst &I) {
  VASTValPtr Operand = getAsOperand(I.getPointerOperand());

  assert((!Operand || getValueSizeInBits(I) == Operand->getBitWidth())
         && "Cast between types with different size found!");
  return Operand;
}

VASTValPtr DatapathBuilder::visitIntToPtrInst(IntToPtrInst &I) {
  VASTValPtr Operand = getAsOperand(I.getOperand(0));

  assert((!Operand || getValueSizeInBits(I) == Operand->getBitWidth())
         && "Cast between types with different size found!");
  return Operand;
}

VASTValPtr DatapathBuilder::visitSelectInst(SelectInst &I) {
  return buildSelExpr(getAsOperand(I.getOperand(0)),
                      getAsOperand(I.getOperand(1)),
                      getAsOperand(I.getOperand(2)),
                      getValueSizeInBits(I));
}

VASTValPtr DatapathBuilder::buildICmpExpr(ICmpInst::Predicate Predicate,
                                          VASTValPtr LHS, VASTValPtr RHS) {
  switch (Predicate) {
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
    return buildICmpOrEqExpr(VASTExpr::dpSGT, LHS, RHS);

  case CmpInst::ICMP_ULE:
    std::swap(LHS, RHS);
    // Fall though.
  case CmpInst::ICMP_UGE:
    return buildICmpOrEqExpr(VASTExpr::dpUGT, LHS, RHS);

  default: break;
  }

  llvm_unreachable("Unexpected ICmp predicate!");
  return 0;
}

VASTValPtr DatapathBuilder::visitICmpInst(ICmpInst &I) {
  return buildICmpExpr(I.getPredicate(),
                       getAsOperand(I.getOperand(0)),
                       getAsOperand(I.getOperand(1)));
}

VASTValPtr DatapathBuilder::visitBinaryOperator(BinaryOperator &I) {
  unsigned NumBits = getValueSizeInBits(I);

  VASTValPtr Ops[] = { getAsOperand(I.getOperand(0)),
                       getAsOperand(I.getOperand(1))};

  // FIXME: Do we need to care about NSW and NUW?
  switch (I.getOpcode()) {
  case Instruction::Add: return buildAddExpr(Ops, NumBits);
  case Instruction::Sub: {
    // A - B is equivalent to A + ~(B) + 1
    VASTValPtr SubOps[] = { Ops[0],
                            buildNotExpr(Ops[1]),
                            getImmediate(1, 1) };
    return buildAddExpr(SubOps, NumBits);
  }
  case Instruction::Mul: return buildMulExpr(Ops, NumBits);

  case Instruction::Shl: return buildExpr(VASTExpr::dpShl, Ops, NumBits);
  case Instruction::AShr: return buildExpr(VASTExpr::dpSRA, Ops, NumBits);
  case Instruction::LShr: return buildExpr(VASTExpr::dpSRL, Ops, NumBits);

  // Div is implemented as submodule.
  case Instruction::UDiv: return lowerUDiv(I);
  case Instruction::SDiv: return lowerSDiv(I);
  case Instruction::SRem: return lowerSRem(I);
  case Instruction::URem: return lowerURem(I);

  case Instruction::And:  return buildAndExpr(Ops, NumBits);
  case Instruction::Or:   return buildOrExpr(Ops, NumBits);
  case Instruction::Xor:  return buildXorExpr(Ops, NumBits);
  default: llvm_unreachable("Unexpected opcode!"); break;
  }

  return None;
}

VASTValPtr DatapathBuilder::visitIntrinsicInst(IntrinsicInst &I) {
  switch (I.getIntrinsicID()) {
  default: break;
  case Intrinsic::uadd_with_overflow: {
    VASTValPtr Ops[] = { getAsOperand(I.getOperand(0)),
                         getAsOperand(I.getOperand(1)) };
    // The result of uadd_with_overflow is 1 bit bigger than the operand size.
    unsigned ResultSize = Ops[0]->getBitWidth() + 1;
    // For unsigned addition, the overflow bit is just the carry bit, i.e. the
    // extra bit of the addition.
    return buildAddExpr(Ops, ResultSize);
  }
  case Intrinsic::bswap: {
    unsigned NumBits = getValueSizeInBits(I);
    VASTValPtr V = getAsOperand(I.getOperand(0));
    assert(NumBits % 16 == 0 && "Bad bitwidth for byteswap!");
    unsigned NumBytes = NumBits / 8;
    SmallVector<VASTValPtr, 8> Bytes(NumBytes, None);
    for (unsigned i = 0; i != NumBytes; ++i)
      Bytes[i] = buildBitSliceExpr(V, i * 8 + 8, i * 8);

    return buildBitCatExpr(Bytes, NumBits);
  }
  }

  return None;
}

VASTValPtr DatapathBuilder::visitExtractValueInst(ExtractValueInst &I) {
  Value *Operand = I.getAggregateOperand();

  if (IntrinsicInst *Intri = dyn_cast<IntrinsicInst>(Operand)) {
    VASTValPtr V = getAsOperand(Operand);
    switch (Intri->getIntrinsicID()) {
    default: break;
    case Intrinsic::uadd_with_overflow: {
      assert(I.getNumIndices() == 1 && "Unexpected number of indices!");
      // Return the overflow bit.
      if (I.getIndices()[0] == 1)
        return buildBitSliceExpr(V, V->getBitWidth(), V->getBitWidth() - 1);
      // Else return the addition result.
      assert(I.getIndices()[0] == 0 && "Bad index!");
      return buildBitSliceExpr(V, V->getBitWidth() - 1, 0);
    }
    }
  }

  return None;
}

VASTValPtr DatapathBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  return visitGEPOperator(cast<GEPOperator>(I));
}

VASTValPtr DatapathBuilder::visitGEPOperator(GEPOperator &O) {
  VASTValPtr Ptr = getAsOperand(O.getPointerOperand());
  // FIXME: All the pointer arithmetic are perform under the precision of
  // PtrSize, do we need to perform the arithmetic at the max available integer
  // width and truncate the result?
  unsigned PtrSize = Ptr->getBitWidth();
  // Note that the pointer operand may be a vector of pointers. Take the scalar
  // element which holds a pointer.
  Type *Ty = O.getPointerOperandType()->getScalarType();

  typedef GEPOperator::op_iterator op_iterator;
  for (op_iterator OI = O.idx_begin(), E = O.op_end(); OI != E; ++OI) {
    const Value *Idx = *OI;
    if (StructType *StTy = dyn_cast<StructType>(Ty)) {
      unsigned Field = cast<ConstantInt>(Idx)->getZExtValue();
      if (Field) {
        // N = N + Offset
        uint64_t Offset
          = getDataLayout()->getStructLayout(StTy)->getElementOffset(Field);
        Ptr = buildExpr(VASTExpr::dpAdd,
                        Ptr, getImmediate(Offset, PtrSize),
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
                        Ptr, getImmediate(Offs, PtrSize),
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
                                getImmediate(Amt, PtrSize),
                                PtrSize);
        } else {
          VASTValPtr Scale = getImmediate(ElementSize.getSExtValue(),
                                                  PtrSize);
          IdxN = buildExpr(VASTExpr::dpMul, IdxN, Scale, PtrSize);
        }
      }
      
      Ptr = buildExpr(VASTExpr::dpAdd, Ptr, IdxN, PtrSize);
    }
  }

  return Ptr;
}
