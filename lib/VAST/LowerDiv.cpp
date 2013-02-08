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
STATISTIC(SREMLowered, "Number of SRem lowered");

VASTValPtr DatapathBuilder::lowerUDiv(BinaryOperator &I) {
  return VASTValPtr();
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

  // Well, our assumption about we can lower the SDiv is wrong.
  --SDIVLowered;
  return VASTValPtr();
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
  return VASTValPtr();
}

VASTValPtr DatapathBuilder::lowerURem(BinaryOperator &I) {
  return VASTValPtr();
}
