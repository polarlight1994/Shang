//===--- CarryChainOpt.cpp - CarryChain Related Optimization  ---*- C++ -*-===//
//
//                      The VAST HLS framework                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Carry Chain related optimization for addition,
// multiplication and comparisions
//
//===----------------------------------------------------------------------===//

#include "vast/BitlevelOpt.h"
#include "vast/VASTModule.h"
#include "vast/VASTHandle.h"

#define DEBUG_TYPE "vast-carry-chain-opt"
#include "llvm/Support/Debug.h"

using namespace vast;

VASTValPtr DatapathBLO::optimizeAddImpl(MutableArrayRef<VASTValPtr> Ops,
                                        unsigned BitWidth) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1)
    return Ops[0];

  std::sort(Ops.begin(), Ops.end(), VASTValPtr::type_less);

  APInt C = APInt::getNullValue(BitWidth);

  VASTValPtr LastVal = None;
  unsigned ActualPos = 0;
  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    VASTValPtr CurVal = Ops[i];
    if (CurVal == LastVal) {
      // A + A = A << 1
      VASTConstant *ShiftAmt = getConstant(1, Log2_32_Ceil(BitWidth));
      CurVal = optimizeShift(VASTExpr::dpShl, CurVal, ShiftAmt, BitWidth);
      // Replace the last value.
      Ops[ActualPos - 1] = CurVal;
      LastVal = CurVal;
      continue;
    }

    if (Builder.buildNotExpr(CurVal) == LastVal) {
      // A + ~A => ~0, verified by
      // (declare-fun a () (_ BitVec 32))
      // (assert (not (= (bvadd a (bvnot a)) #xffffffff)))
      // (check-sat)
      // -> unsat
      CurVal = getConstant(APInt::getAllOnesValue(BitWidth));
      // Replace the last value.
      Ops[ActualPos - 1] = CurVal;
      LastVal = CurVal;
      continue;
    }
    // Accumulate the constants.
    if (VASTConstPtr CurC = dyn_cast<VASTConstPtr>(CurVal)) {
      assert((CurC->getBitWidth() == BitWidth || CurC->getBitWidth() == 1)
             && "Unexpected bitwidth!");
      C += CurC.getAPInt().zextOrTrunc(BitWidth);
      continue;
    }

    Ops[ActualPos++] = CurVal;
    LastVal = CurVal;
  }

  // Add the accumulated constant to operand list.
  if (C.getBoolValue())
    Ops[ActualPos++] = getConstant(C);

  Ops = Ops.slice(0, ActualPos);

  // If there is only 1 operand left, simply return the operand.
  if (ActualPos == 1)
    return Ops[0];

  return optimizeCarryChain(VASTExpr::dpAdd, Ops, BitWidth);
}

VASTValPtr DatapathBLO::optimizeMulImpl(MutableArrayRef<VASTValPtr> Ops,
                                        unsigned BitWidth) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1)
    return Ops[0];

  std::sort(Ops.begin(), Ops.end(), VASTValPtr::type_less);

  APInt C = APInt(BitWidth, 1);

  VASTValPtr LastVal = None;
  unsigned ActualPos = 0;
  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    VASTValPtr CurVal = Ops[i];
    // Accumulate the constants.
    if (VASTConstPtr CurC = dyn_cast<VASTConstPtr>(CurVal)) {
      C *= CurC.getAPInt();
      continue;
    }

    Ops[ActualPos++] = CurVal;
  }

  // A * 0 => 0
  if (!C.getBoolValue())
    return getConstant(C);

  // A * 1 => A, we can Ignore the constant.
  if (C == APInt(BitWidth, 1))
    return Builder.buildMulExpr(Ops.slice(0, ActualPos), BitWidth);

  // Reserve a place for the constant operand.
  Ops[ActualPos++] = None;

  return optimizeMulWithConst(Ops, C, BitWidth);
}

VASTValPtr DatapathBLO::optimizeMulWithConst(MutableArrayRef<VASTValPtr> Ops,
                                             APInt C, unsigned BitWidth) {
  assert(Ops.back() == None &&
         "Expect place holder for the constant operand at the end!");

  if (C.isPowerOf2()) {
    // Remove the place holder for the constant operand.
    Ops = Ops.slice(0, Ops.size() - 1);

    unsigned lg2 = C.countTrailingZeros();
    VASTValPtr SubExpr = Builder.buildMulExpr(Ops, BitWidth);
    // Implement the multiplication by shift.
    return optimizeShift(VASTExpr::dpShl,
                          SubExpr, getConstant(lg2, BitWidth),
                          BitWidth);
  }

  // Implement the multiplication by shift and addition if the immediate is
  // bit mask, i.e. A * <N lower bits set> = (A << N) - A.
  if (isMask(C)) {
    // Remove the place holder for the constant operand.
    Ops = Ops.slice(0, Ops.size() - 1);
    unsigned lg2 = C.countTrailingOnes();

    VASTValPtr SubMul = Ops.size() == 1 ? Ops[0] :
                        Builder.buildMulExpr(Ops, BitWidth);
    // Lower A * -1 = A.
    if (lg2 == BitWidth) {
      VASTValPtr NegOps[] = { optimizeNot(SubMul), VASTConstant::True };
      return optimizeAddImpl(NegOps, BitWidth);
    }

    VASTValPtr ShiftedSubExpr
      = optimizeShift(VASTExpr::dpShl, SubMul, getConstant(lg2, BitWidth),
                      BitWidth);

    // Construct the subtraction: A - B = A + ~B + 1.
    VASTValPtr AddOps[] = {
      ShiftedSubExpr, optimizeNot(SubMul), VASTConstant::True
    };

    return optimizeAddImpl(AddOps, BitWidth);
  }

  // Else we have to build the multiplication with the constant.
  Ops.back() = getConstant(C);

  return Builder.buildMulExpr(Ops, BitWidth);
}

VASTValPtr DatapathBLO::optimizeCarryChain(VASTExpr::Opcode Opcode,
                                           MutableArrayRef<VASTValPtr>  Ops,
                                           unsigned BitWidth) {
  return Builder.buildExpr(Opcode, Ops, BitWidth);
}
