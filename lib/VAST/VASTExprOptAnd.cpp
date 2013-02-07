//===- VASTExprOptAnd.cpp - Optimize the Bitwise And Expression -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement optimizations on the Bitwise And Expression.
//
//===----------------------------------------------------------------------===//

#include "VASTExprBuilder.h"

#include "shang/Utilities.h"

#include "llvm/Support/ErrorHandling.h"
#define DEBUG_TYPE "vast-expr-opt-and"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace llvm {
template<>
struct VASTExprOpInfo<VASTExpr::dpAnd> {
  VASTExprBuilder &Builder;
  unsigned OperandWidth;
  APInt KnownZeros, KnownOnes;

  VASTExprOpInfo(VASTExprBuilder &Builder, unsigned OperandWidth)
    : Builder(Builder), OperandWidth(OperandWidth), KnownZeros(OperandWidth, 0),
      KnownOnes(APInt::getAllOnesValue(OperandWidth))/*Assume all bits are ones*/
  {}

  VASTValPtr analyzeOperand(VASTValPtr V) {
    assert(OperandWidth == V->getBitWidth() && "Bitwidth not match!");

    if (VASTImmPtr Imm = dyn_cast<VASTImmPtr>(V)) {
      // The bit is known one only if the bit of all operand are one.
      KnownOnes &= Imm.getAPInt();
      // The bit is known zero if the bit of any operand are zero.
      KnownZeros |= ~Imm.getAPInt();
      return 0;
    }

    APInt OpKnownZeros, OpKnownOnes;
    Builder.calculateBitMask(V, OpKnownZeros, OpKnownOnes);
    KnownOnes &= OpKnownOnes;
    KnownZeros |= OpKnownZeros;

    // Do nothing by default.
    return V;
  }

  // Functions about constant mask.
  bool isAllZeros() const { return KnownZeros.isAllOnesValue(); }
  bool hasAnyZero() const  { return KnownZeros.getBoolValue(); }
  // For the and expression, only zero is known.
  APInt getZeros() const { return ~KnownZeros; }
  APInt getConstMask() const {
    APInt Mask = KnownZeros;
    Mask |= KnownOnes;
    return Mask;
  }

  bool hasFullConstMask() const { return getConstMask().isAllOnesValue(); }

  bool getZeroMaskSplitPoints(unsigned &HiPt, unsigned &LoPt) const {
    HiPt = OperandWidth;
    LoPt = 0;

    if (!KnownZeros.getBoolValue()) return false;

    if (APIntOps::isShiftedMask(OperandWidth, KnownZeros)
        || APIntOps::isMask(OperandWidth, KnownZeros)) {
      LoPt = KnownZeros.countTrailingZeros();
      HiPt = OperandWidth - KnownZeros.countLeadingZeros();
      assert(HiPt > LoPt && "Bad split point!");
      return true;
    }

    APInt NotKnownZeros = ~KnownZeros;
    if (APIntOps::isShiftedMask(OperandWidth, NotKnownZeros)
        || APIntOps::isMask(OperandWidth, NotKnownZeros)) {
      LoPt = NotKnownZeros.countTrailingZeros();
      HiPt = OperandWidth - NotKnownZeros.countLeadingZeros();
      assert(HiPt > LoPt && "Bad split point!");
      return true;
    }

    return false;
  }
};
}

VASTValPtr VASTExprBuilder::buildAndExpr(ArrayRef<VASTValPtr> Ops,
                                         unsigned BitWidth) {
  SmallVector<VASTValPtr, 8> NewOps;
  typedef const VASTUse *op_iterator;
  VASTExprOpInfo<VASTExpr::dpAnd> OpInfo(*this, BitWidth);
  flattenExpr<VASTExpr::dpAnd>(Ops.begin(), Ops.end(),
                               op_filler<VASTExpr::dpAnd>(NewOps, OpInfo));

  // Check the immediate mask.
  if (OpInfo.isAllZeros())
    return getOrCreateImmediate(APInt::getNullValue(BitWidth));

  if (NewOps.empty()) {
    assert(OpInfo.hasFullConstMask() && "Unexpected empty NewOps!");
    // All the operands in Ops are constant, directly return the constant result.
    return Context.getOrCreateImmediate(OpInfo.getZeros());
  }

  if (NewOps.size() == 1) {
  }

  if (OpInfo.hasAnyZero()) {
    NewOps.push_back(Context.getOrCreateImmediate(OpInfo.getZeros()));

    // Split the word according to known zeros.
    unsigned HiPt, LoPt;
    if (OpInfo.getZeroMaskSplitPoints(HiPt, LoPt)) {
      assert(BitWidth >= HiPt && HiPt > LoPt && "Bad split point!");
      SmallVector<VASTValPtr, 4> Ops;

      if (HiPt != BitWidth)
        Ops.push_back(buildExprByOpBitSlice(VASTExpr::dpAnd, NewOps, BitWidth,
                                            HiPt));

      Ops.push_back(buildExprByOpBitSlice(VASTExpr::dpAnd, NewOps, HiPt, LoPt));

      if (LoPt != 0)
        Ops.push_back(buildExprByOpBitSlice(VASTExpr::dpAnd, NewOps, LoPt, 0));

      return buildBitCatExpr(Ops, BitWidth);
    }
  }

  std::sort(NewOps.begin(), NewOps.end(), VASTValPtr::type_less);
  typedef SmallVectorImpl<VASTValPtr>::iterator it;
  VASTValPtr LastVal;
  unsigned ActualPos = 0;
  for (unsigned i = 0, e = NewOps.size(); i != e; ++i) {
    VASTValPtr CurVal = NewOps[i];
    if (CurVal == LastVal) {
      // A & A = A
      continue;
    } else if (CurVal.invert() == LastVal)
      // A & ~A => 0
      return getBoolImmediate(false);

    NewOps[ActualPos++] = CurVal;
    LastVal = CurVal;
  }
  // If there is only 1 operand left, simply return the operand.
  if (ActualPos == 1) return LastVal;

  // Resize the operand vector so it only contains valid operands.
  NewOps.resize(ActualPos);

  return createExpr(VASTExpr::dpAnd, NewOps, BitWidth, 0);
}
