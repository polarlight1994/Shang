//===- LowerDiv.cpp - Expand the [S|U]Divs and [S|U]Rems --------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the functions to Expand the [S|U]Divs and [S|U]Rems
//
//===----------------------------------------------------------------------===//

#include "IR2Datapath.h"

#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-lower-div-rem"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(SDIVLowered, "Number of SDiv lowered");
STATISTIC(UDIVLowered, "Number of UDiv lowered");
STATISTIC(SREMLowered, "Number of SRem lowered");
STATISTIC(UREMLowered, "Number of URem lowered");

VASTValPtr DatapathBuilder::lowerUDiv(BinaryOperator &I) {
  Value *LHS = I.getOperand(0), *RHS = I.getOperand(1);
  ConstantInt *LHSC = dyn_cast<ConstantInt>(LHS);
  ConstantInt *RHSC = dyn_cast<ConstantInt>(RHS);
  // Assume we can lower the UDiv.
  ++UDIVLowered;

  // fold (udiv c1, c2) -> c1/c2
  if (LHSC && RHSC && !RHSC->isNullValue())
    return getImmediate(LHSC->getValue().udiv(RHSC->getValue()));

  if (RHSC) {
    unsigned ResultSizeInBits = getValueSizeInBits(I);
    VASTValPtr N = getAsOperand(LHS);
    VASTValPtr Q = N;

    APInt d = RHSC->getValue();
    APInt::mu magics = d.magicu();

    // If the divisor is even, we can avoid using the expensive fixup by shifting
    // the divided value upfront.
    if (magics.a != 0 && d[0]) {
      unsigned Shift = d.countTrailingZeros();
      Q = buildShiftExpr(VASTExpr::dpSRL, Q,
                         getImmediate(Shift, ResultSizeInBits),
                         ResultSizeInBits);

    // Get magic number for the shifted divisor.
      magics = d.lshr(Shift).magicu(Shift);
      assert(magics.a == 0 && "Should use cheap fixup now");
    }

    // Multiply the numerator (operand 0) by the magic value
    Q = buildMulExpr(Q, getImmediate(magics.m), 2 * ResultSizeInBits);
    // Get the higher part of the multplication result.
    Q = buildBitSliceExpr(Q, 2 * ResultSizeInBits, ResultSizeInBits);

    if (magics.a == 0)
      return buildShiftExpr(VASTExpr::dpSRL, Q,
                            getImmediate(magics.s, ResultSizeInBits),
                            ResultSizeInBits);

    VASTValPtr Ops[] = { N, buildNotExpr(Q),  VASTImmediate::True };
    VASTValPtr NPQ = buildAddExpr(Ops, ResultSizeInBits);
    NPQ = buildShiftExpr(VASTExpr::dpSRL, NPQ,
                         getImmediate(1, ResultSizeInBits),
                         ResultSizeInBits);
    NPQ = buildAddExpr(NPQ, Q, ResultSizeInBits);
    return buildShiftExpr(VASTExpr::dpSRL, NPQ,
                          getImmediate(magics.s - 1, ResultSizeInBits),
                          ResultSizeInBits);
  }

  // Well, our assumption about we can lower the UDiv is wrong.
  --UDIVLowered;
  return None;
}

VASTValPtr DatapathBuilder::lowerSDiv(BinaryOperator &I) {
  Value *LHS = I.getOperand(0), *RHS = I.getOperand(1);
  ConstantInt *LHSC = dyn_cast<ConstantInt>(LHS);
  ConstantInt *RHSC = dyn_cast<ConstantInt>(RHS);
  // Assume we can lower the SDiv.
  ++SDIVLowered;

  // fold (sdiv c1, c2) -> c1/c2
  if (LHSC && RHSC && !RHSC->isNullValue())
    return getImmediate(LHSC->getValue().sdiv(RHSC->getValue()));
  // fold (sdiv X, 1) -> X
  if (RHSC && RHSC->getValue() == 1LL)
    return getAsOperand(LHS);
  // fold (sdiv X, -1) -> 0-X
  if (RHSC && RHSC->isAllOnesValue())
    return buildNegative(getAsOperand(LHS));

  // fold (sdiv X, pow2)
  if (RHSC && !RHSC->isNullValue()
      && (RHSC->getValue().isPowerOf2() || (-RHSC->getValue()).isPowerOf2())) {
    unsigned lg2 = RHSC->getValue().countTrailingZeros();
    VASTValPtr LHSVal = getAsOperand(LHS);
    unsigned SizeInBits = LHSVal->getBitWidth();

    // Splat the sign bit put the sign bit of LHS at the lower lg2 bits, and add
    // it to LHS.
    VASTValPtr BitCatOps[] = { getImmediate(0, SizeInBits - lg2),
                               buildBitRepeat(getSignBit(LHSVal), lg2),
                             };
    VASTValPtr SGN = buildBitCatExpr(BitCatOps, SizeInBits);

    // Add (LHS < 0) ? abs2 - 1 : 0;
    VASTValPtr ADD = buildAddExpr(LHSVal, SGN, SizeInBits);

    // Divide by pow2
    VASTValPtr SRA
      = buildShiftExpr(VASTExpr::dpSRA, ADD,
                       getImmediate(lg2, SizeInBits), SizeInBits);

    // If we're dividing by a positive value, we're done.  Otherwise, we must
    // negate the result.
    if (RHSC->getValue().isNonNegative()) return SRA;

    return buildNegative(SRA);
  }

  if (RHSC) {
    // Lower divide by constant just like what TargetLowering::BuildSDIV do.
    APInt d = RHSC->getValue();
    APInt::ms magics = d.magic();
    unsigned ResultSizeInBits = getValueSizeInBits(I);
    VASTValPtr N = getAsOperand(LHS);
    VASTValPtr Q = buildMulExpr(N, getImmediate(magics.m), 2 * ResultSizeInBits);
    // Get the higher part of the multplication result.
    Q = buildBitSliceExpr(Q, 2 * ResultSizeInBits, ResultSizeInBits);
    // If d > 0 and m < 0, add the numerator
    if (d.isStrictlyPositive() && magics.m.isNegative())
      Q = buildAddExpr(Q, N, ResultSizeInBits);
    // If d < 0 and m > 0, subtract the numerator
    if (d.isNegative() && magics.m.isStrictlyPositive()) {
      VASTValPtr Ops[] = { Q, buildNotExpr(N),  VASTImmediate::True };
      Q = buildAddExpr(Ops, ResultSizeInBits);
    }

    // Shift right algebraic if shift value is nonzero
    if (magics.s > 0)
      Q = buildShiftExpr(VASTExpr::dpSRA, Q,
                         getImmediate(magics.s, ResultSizeInBits),
                         ResultSizeInBits);
    // Extract the sign bit and add it to the quotient
    VASTValPtr T = getSignBit(Q);
    return buildAddExpr(Q, T, ResultSizeInBits);
  }

  // Well, our assumption about we can lower the SDiv is wrong.
  --SDIVLowered;
  return None;
}

VASTValPtr DatapathBuilder::lowerSRem(BinaryOperator &I) {
  Value *LHS = I.getOperand(0), *RHS = I.getOperand(1);
  ConstantInt *LHSC = dyn_cast<ConstantInt>(LHS);
  ConstantInt *RHSC = dyn_cast<ConstantInt>(RHS);
  // Assume we can lower the SRem.
  ++SREMLowered;

  // fold (srem c1, c2) -> c1%c2
  if (LHSC && RHSC && !RHSC->isNullValue())
    return getImmediate(LHSC->getValue().srem(RHSC->getValue()));

  // If X/C can be simplified by the division-by-constant logic, lower
  // X%C to the equivalent of X-X/C*C.
  if (RHSC && !RHSC->isNullValue())
    // If we can get the SDiv expression.
    if (VASTValPtr SDiv = lowerSDiv(I)) {
      VASTValPtr LHSVal = getAsOperand(LHS);
      unsigned SizeInBits = LHSVal->getBitWidth();
      VASTValPtr Mul = buildMulExpr(SDiv, getAsOperand(RHS), SizeInBits);
      VASTValPtr SubOps[] = { LHSVal,
                              buildNotExpr(Mul),
                              getImmediate(1,1)
                            };
      return buildAddExpr(SubOps, SizeInBits);
    }

  // Well, our assumption about we can lower the SRem is wrong.
  --SREMLowered;
  return None;
}

VASTValPtr DatapathBuilder::lowerURem(BinaryOperator &I) {
  Value *LHS = I.getOperand(0), *RHS = I.getOperand(1);
  ConstantInt *LHSC = dyn_cast<ConstantInt>(LHS);
  ConstantInt *RHSC = dyn_cast<ConstantInt>(RHS);
  // Assume we can lower the SRem.
  ++UREMLowered;

  // fold (srem c1, c2) -> c1%c2
  if (LHSC && RHSC && !RHSC->isNullValue())
    return getImmediate(LHSC->getValue().urem(RHSC->getValue()));

  // If X/C can be simplified by the division-by-constant logic, lower
  // X%C to the equivalent of X-X/C*C.
  if (RHSC && !RHSC->isNullValue())
    // If we can get the SDiv expression.
    if (VASTValPtr UDiv = lowerUDiv(I)) {
      VASTValPtr LHSVal = getAsOperand(LHS);
      unsigned SizeInBits = LHSVal->getBitWidth();
      VASTValPtr Mul = buildMulExpr(UDiv, getAsOperand(RHS), SizeInBits);
      VASTValPtr SubOps[] = { LHSVal,
                              buildNotExpr(Mul),
                              getImmediate(1,1)
                            };
      return buildAddExpr(SubOps, SizeInBits);
    }

  // Well, our assumption about we can lower the SRem is wrong.
  --UREMLowered;
  return None;
}
