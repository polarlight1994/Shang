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
  // We only care about the known zeros because we assume all unknown bits are
  // 1s.
  APInt KnownZeros, KnownOnes;

  VASTExprOpInfo(VASTExprBuilder &Builder, unsigned OperandWidth)
    : Builder(Builder), OperandWidth(OperandWidth), KnownZeros(OperandWidth, 0),
      KnownOnes(APInt::getAllOnesValue(OperandWidth))
  {}

  VASTValPtr analyzeOperand(VASTValPtr V) {
    assert(OperandWidth == V->getBitWidth() && "Bitwidth not match!");

    if (VASTImmPtr Imm = dyn_cast<VASTImmPtr>(V)) {
      // The bit is known zero if the bit of any operand are zero.
      KnownZeros |= ~Imm.getAPInt();
      KnownOnes  &= Imm.getAPInt();
      return 0;
    }

    APInt OpKnownZeros, OpKnownOnes;
    Builder.calculateBitMask(V, OpKnownZeros, OpKnownOnes);
    KnownZeros |= OpKnownZeros;
    KnownOnes &= OpKnownOnes;
    assert(!KnownZeros.intersects(KnownOnes) && "Got bad bitmask!");
    // Do nothing by default.
    return V;
  }

  // Functions about constant mask.
  bool isAllZeros() const { return KnownZeros.isAllOnesValue(); }
  bool hasAnyZero() const  { return KnownZeros.getBoolValue(); }
  bool isAllOnes() const { return KnownOnes.isAllOnesValue(); }
  APInt getKnownBits() const { return KnownZeros | KnownOnes; }
  bool isAllBitsKnown() const {
    return getKnownBits().isAllOnesValue();
  }

  // For the and expression, only zero is known, and we assume all other bits
  // are 1s.
  APInt getZeros() const { return ~KnownZeros; }
};
}

static VASTValPtr splitByMask(VASTExprBuilder &Builder, APInt Mask,
                              ArrayRef<VASTValPtr> NewOps) {
  // Split the word according to known bits.
  unsigned HiPt, LoPt;
  unsigned BitWidth = Mask.getBitWidth();

  if (VASTExprBuilder::GetMaskSplitPoints(Mask, HiPt, LoPt)) {
    assert(BitWidth >= HiPt && HiPt > LoPt && "Bad split point!");
    SmallVector<VASTValPtr, 4> Ops;

    if (HiPt != BitWidth)
      Ops.push_back(Builder.buildExprByOpBitSlice(VASTExpr::dpAnd, NewOps,
                                                  BitWidth, HiPt));

    Ops.push_back(Builder.buildExprByOpBitSlice(VASTExpr::dpAnd, NewOps,
                                                HiPt, LoPt));

    if (LoPt != 0)
      Ops.push_back(Builder.buildExprByOpBitSlice(VASTExpr::dpAnd, NewOps,
                                                  LoPt, 0));

    return Builder.buildBitCatExpr(Ops, BitWidth);
  }

  return VASTValPtr();
}

VASTValPtr VASTExprBuilder::buildAndExpr(ArrayRef<VASTValPtr> Ops,
                                         unsigned BitWidth) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1) return Ops[0];

  DEBUG(dbgs() << "Going to and these Operands together:\n";
  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    Ops[i].printAsOperand(dbgs().indent(2));
    dbgs() << '\n';
  });

  SmallVector<VASTValPtr, 8> NewOps;
  typedef const VASTUse *op_iterator;
  VASTExprOpInfo<VASTExpr::dpAnd> OpInfo(*this, BitWidth);
  flattenExpr<VASTExpr::dpAnd>(Ops.begin(), Ops.end(),
                               op_filler<VASTExpr::dpAnd>(NewOps, OpInfo));

  // Check the immediate mask. Return the constant value if all bits are known.
  if (OpInfo.isAllBitsKnown())
    return getImmediate(OpInfo.getZeros());

  DEBUG(
    dbgs() << "KnownZeros: " << OpInfo.KnownZeros.toString(16, false) << '\n'
           << "KnownOnes: " << OpInfo.KnownOnes.toString(16, false) << '\n';
  );

  // Add the Constant back to the Operands.
  if (OpInfo.hasAnyZero())
    NewOps.push_back(getImmediate(OpInfo.getZeros()));

  // Split the word according to known zero bits.
  if (VASTValPtr V = splitByMask(*this, OpInfo.getKnownBits(), NewOps))
    return V;

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
      return getImmediate(APInt::getNullValue(BitWidth));

    // Ignore the 1s
    if (VASTImmPtr Imm = dyn_cast<VASTImmPtr>(CurVal))
      if (Imm.isAllOnes()) {
        DEBUG(dbgs().indent(2) << "Discard the all ones value: " << Imm << '\n');
        continue;
      }

    NewOps[ActualPos++] = CurVal;
    LastVal = CurVal;
  }
  // If there is only 1 operand left, simply return the operand.
  if (ActualPos == 1) return LastVal;

  // Resize the operand vector so it only contains valid operands.
  NewOps.resize(ActualPos);

  return createExpr(VASTExpr::dpAnd, NewOps, BitWidth, 0);
}
