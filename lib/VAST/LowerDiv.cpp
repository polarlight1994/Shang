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

#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<bool>
SDIVBYPOW2TOASHR("shang-sdiv-by-power-of-2-to-ashr",
  cl::desc("Replace the sdiv by power of 2 by ashr"),
  cl::init(false));

VASTValPtr DatapathBuilder::lowerUDiv(BinaryOperator &I) {
  return VASTValPtr();
}

VASTValPtr DatapathBuilder::lowerSDiv(BinaryOperator &I) {
  Value *Op0 = I.getOperand(0), *Op1 = I.getOperand(1);

  if (ConstantInt *RHS = dyn_cast<ConstantInt>(Op1)) {
    // Ignore the exact flag and round toward 0.
    // See http://bob.allegronetwork.com/prog/tricks.html
    // Section "Division by constant powers-of-2"
    if (SDIVBYPOW2TOASHR && RHS->getValue().isNonNegative()
        && RHS->getValue().isPowerOf2()) {
      VASTValPtr LHSVal = getAsOperand(Op0);
      VASTValPtr LHSSign = getSignBit(LHSVal);
      // Compensate for the shift error by adding the missing 1 if the original
      // number was negative
      VASTValPtr AddOps[] = { LHSVal, LHSSign };
      unsigned SizeInBits = LHSVal->getBitWidth();
      LHSVal = buildAddExpr(AddOps, SizeInBits);
      VASTValPtr ShiftAmt =
        getOrCreateImmediate(RHS->getValue().exactLogBase2(), SizeInBits);
      // Replace the sdiv by a shift.
      return buildShiftExpr(VASTExpr::dpSRA, LHSVal, ShiftAmt, SizeInBits);
    }
  }

  return VASTValPtr();
}

VASTValPtr DatapathBuilder::lowerSRem(BinaryOperator &I) {
  return VASTValPtr();
}

VASTValPtr DatapathBuilder::lowerURem(BinaryOperator &I) {
  return VASTValPtr();
}
