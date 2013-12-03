//===- VASTExprOptCmp.cpp - Optimize the Integer Comparisons -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement optimizations on the Integer Comparisons.
//
//===----------------------------------------------------------------------===//

#include "vast/VASTExprBuilder.h"

#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-expr-opt-cmp"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;
STATISTIC(ConstICmpLowered, "Number of Constant ICmp lowered");
STATISTIC(OneBitICmpLowered, "Number of 1 bit ICmp lowered");
STATISTIC(ICmpSplited, "Number of ICmp splited");
STATISTIC(CmpEquOptimized, "Number of >= optimized");
//===----------------------------------------------------------------------===//
// FIXME: Commit these function to llvm mainstream.
static bool isMask(APInt Value) {
  return Value.getBoolValue() && ((Value + 1) & Value).isMinValue();
}

inline bool isShiftedMask(APInt Value) {
  return isMask((Value - 1) | Value);
}

static bool isSigned(VASTExpr::Opcode Opc) {
  switch (Opc) {
  case VASTExpr::dpUGT: return false;
  case VASTExpr::dpSGT: return true;
  default:
    llvm_unreachable("Unexpected opcode!");
  }

  return false;
}

// Get the Max/Min value according to the signedness of the opcode.
static APInt GetMax(VASTExpr::Opcode Opc, unsigned SizeInBits) {
  return isSigned(Opc) ? APInt::getSignedMaxValue(SizeInBits)
                       : APInt::getMaxValue(SizeInBits);
}

static APInt GetMin(VASTExpr::Opcode Opc, unsigned SizeInBits) {
  return isSigned(Opc) ? APInt::getSignedMinValue(SizeInBits)
                       : APInt::getMinValue(SizeInBits);
}

//===----------------------------------------------------------------------===//
static VASTValPtr BuildSplitedICmp(VASTExprBuilder &Builder, VASTExpr::Opcode Opc,
                                   VASTValPtr LHS, VASTValPtr RHS,
                                   unsigned SplitAt) {
  unsigned SizeInBits = LHS->getBitWidth();
  assert(SplitAt != SizeInBits && SplitAt != 0 && "Bad split point!");
  ++ICmpSplited;

  // Force split at the signed bit for signed comparison.
  if (isSigned(Opc)) SplitAt = SizeInBits - 1;

  VASTValPtr Ops[] = { LHS, RHS };
  VASTValPtr Hi = Builder.buildExprByOpBitSlice(Opc, Ops, SizeInBits, SplitAt);
  // Force unsigned comparison on the lower part.
  VASTValPtr Lo = Builder.buildExprByOpBitSlice(VASTExpr::dpUGT, Ops, SplitAt, 0);

  VASTValPtr HiEQ
    = Builder.buildEQ(Builder.buildBitSliceExpr(LHS, SizeInBits, SplitAt),
                      Builder.buildBitSliceExpr(RHS, SizeInBits, SplitAt));

  // Use the Lo result if the operands higher part are equal.
  return Builder.buildOrExpr(Hi, Builder.buildAndExpr(HiEQ, Lo, 1), 1);
}

static VASTValPtr BuildSplitedICmp(VASTExprBuilder &Builder, VASTExpr::Opcode Opc,
                                   VASTValPtr LHS, VASTValPtr RHS,
                                   const APInt &Mask) {
  unsigned SizeInBits = LHS->getBitWidth();
  unsigned HiPt, LoPt;

  if (Builder.GetMaskSplitPoints(Mask, HiPt, LoPt)) {
    if (SizeInBits - HiPt > LoPt)
      return BuildSplitedICmp(Builder, Opc, LHS, RHS, HiPt);
    else
      return BuildSplitedICmp(Builder, Opc, LHS, RHS, LoPt);
  }

  return VASTValPtr();
}

//===----------------------------------------------------------------------===//
static VASTValPtr Lower1BitICmp(VASTExprBuilder &Builder, VASTExpr::Opcode Opc,
                                VASTValPtr LHS, VASTValPtr RHS) {
  assert(LHS->getBitWidth() == 1 && LHS->getBitWidth() == 1
         && "Unexpected bitwidth in lower1BitICmp!");
  switch (Opc) {
  // A > B <=> A == 1 && B == 0.
  case VASTExpr::dpUGT:
    return Builder.buildAndExpr(LHS, Builder.buildNotExpr(RHS), 1);
  // A > B <=>  A == 0 && B == 1.
  case VASTExpr::dpSGT:
    return Builder.buildAndExpr(Builder.buildNotExpr(LHS), RHS, 1);
  default:
    llvm_unreachable("Unexpected opcode!");
  }

  return VASTValPtr();
}

//===----------------------------------------------------------------------===//
static VASTValPtr
FoldPatialConstICmp(VASTExprBuilder &Builder, bool IsSigned, VASTValPtr Var,
                    const APInt &Const, bool VarAtLHS,
                    const APInt &Max, const APInt &Min) {
  if (Const == Min) {
    // a > min <=> a != min.
    if (VarAtLHS) return Builder.buildNE(Var, Builder.getImmediate(Min));
    // min > a is always false,
    else          return Builder.getBoolImmediate(false);
  }

  if (Const == Max) {
    // a > max is always false.
    if (VarAtLHS) return Builder.getBoolImmediate(false);
    // max > a <=> a != max
    else          return Builder.buildNE(Var, Builder.getImmediate(Max));
  }

  if (IsSigned && Const.isMinValue()) {
    // a > 0 => signed bit == 0 && nonzero.
    if (VarAtLHS)
      return Builder.buildAndExpr(Builder.buildNotExpr(Builder.getSignBit(Var)),
                                  Builder.buildROr(Var),
                                  1);
    // 0 > a => signed bit == 1 && nonzero..
    else
      return Builder.buildAndExpr(Builder.getSignBit(Var),
                                  Builder.buildROr(Var),
                                  1);
  }

  if (IsSigned && Const.isAllOnesValue()) {
    // a > -1 <=> a >= 0 <=> signed bit == 0
    if (VarAtLHS) return Builder.buildNotExpr(Builder.getSignBit(Var));
    // -1 > a <=> a < 0 && a != -1
    else {
      APInt MinusOne = APInt::getAllOnesValue(Var->getBitWidth());
      VASTValPtr MinusOneImm = Builder.getImmediate(MinusOne);
      return Builder.buildAndExpr(Builder.getSignBit(Var),
                                  Builder.buildNE(Var, MinusOneImm),
                                  1);
    }
  }

  return VASTValPtr();
}


static
VASTValPtr FoldPatialConstICmp(VASTExprBuilder &Builder, VASTExpr::Opcode Opc,
                               VASTValPtr Var, const APInt &Const, bool VarAtLHS) {
  if (VASTValPtr V = FoldPatialConstICmp(Builder, isSigned(Opc), Var, Const,
                                         VarAtLHS,
                                         GetMax(Opc, Var->getBitWidth()),
                                         GetMin(Opc, Var->getBitWidth())))
    return V;

  if (!Const.isAllOnesValue() && Const.getBoolValue()) {
    // Try to split the operand according to the sequence of ones/zeros
    VASTValPtr LHS = VarAtLHS ? Var : Builder.getImmediate(Const);
    VASTValPtr RHS = VarAtLHS ? Builder.getImmediate(Const) : Var;

    unsigned SizeInBits = Var->getBitWidth();
    unsigned LoPt = 0, HiPt = SizeInBits;

    // Try to get the maximal sequence of ones/zeros.
    if (isMask(Const))
      LoPt = std::max(LoPt, Const.countTrailingOnes());
    else if (isShiftedMask(Const)) {
      LoPt = std::max(LoPt, Const.countTrailingZeros());
      HiPt = std::min(HiPt, SizeInBits - Const.countLeadingZeros());
    }

    if (isMask(~Const))
      LoPt = std::max(LoPt, Const.countTrailingZeros());
    else if (isShiftedMask(~Const)) {
      LoPt = std::max(LoPt, Const.countTrailingOnes());
      HiPt = std::min(HiPt, SizeInBits - Const.countLeadingOnes());
    }

    unsigned SplitPt = (SizeInBits - HiPt > LoPt) ? HiPt : LoPt;
    if (SplitPt) return BuildSplitedICmp(Builder, Opc, LHS, RHS, SplitPt);
  }

  return VASTValPtr();
}

static VASTValPtr FoldConstICmp(VASTExprBuilder &Builder, VASTExpr::Opcode Opc,
                                VASTValPtr LHS, VASTValPtr RHS) {
  VASTImmPtr LHSC = dyn_cast<VASTImmPtr>(LHS),
             RHSC = dyn_cast<VASTImmPtr>(RHS);

  // Calculate the results of ICmp now.
  if (LHSC && RHSC) {
    switch (Opc) {
    case VASTExpr::dpUGT:
      return Builder.getImmediate(LHSC.getAPInt().ugt(RHSC.getAPInt()), 1);
    case VASTExpr::dpSGT:
      return Builder.getImmediate(LHSC.getAPInt().sgt(RHSC.getAPInt()), 1);
    default:
      llvm_unreachable("Unexpected opcode!");
    }

    return VASTValPtr();
  }

  if (LHSC)
    return FoldPatialConstICmp(Builder, Opc, RHS, LHSC.getAPInt(), false);

  if (RHSC)
    return FoldPatialConstICmp(Builder, Opc, LHS, RHSC.getAPInt(), true);

  return VASTValPtr();
}

VASTValPtr VASTExprBuilder::buildICmpExpr(VASTExpr::Opcode Opc,
                                          VASTValPtr LHS, VASTValPtr RHS) {
  assert(RHS->getBitWidth() == LHS->getBitWidth() && "Bad icmp bitwidth!");
  unsigned SizeInBits = LHS->getBitWidth();

  // Handle the trivial case trivially.
  if (SizeInBits == 1) {
    ++OneBitICmpLowered;
    return Lower1BitICmp(*this, Opc, LHS, RHS);
  }

  if (VASTValPtr V = FoldConstICmp(*this, Opc, LHS, RHS)) {
    ++ConstICmpLowered;
    return V;
  }

  BitMasks LHSMasks = calculateBitMask(LHS);
  BitMasks RHSMasks = calculateBitMask(RHS);
  APInt LHSKnownBits = LHSMasks.getKnownBits(),
        RHSKnownBits = RHSMasks.getKnownBits();
  APInt AllKnownBits = LHSKnownBits & RHSKnownBits;

  DEBUG(
  dbgs() << "Size of ICmp: " << LHS->getBitWidth() << "\n";
  LHS.printAsOperand(dbgs());
  dbgs().indent(2) << " LHSKnownBits " << LHSKnownBits.toString(2, false) << "\n";
  RHS.printAsOperand(dbgs());
  dbgs().indent(2) << " RHSKnownBits " << RHSKnownBits.toString(2, false) << "\n";
  dbgs().indent(2) << " AllKnownBits " << AllKnownBits.toString(2, false) << "\n";
  );

  // Split the ICmp according to the all known bits.
  if (VASTValPtr V = BuildSplitedICmp(*this, Opc, LHS, RHS, AllKnownBits))
    return V;

  VASTValPtr Ops[] = { LHS, RHS };
  return createExpr(Opc, Ops, 1, 0);
}

VASTValPtr VASTExprBuilder::buildICmpOrEqExpr(VASTExpr::Opcode Opc,
                                              VASTValPtr LHS, VASTValPtr RHS) {
  unsigned SizeInBits = LHS->getBitWidth();
  VASTImmPtr LHSC = dyn_cast<VASTImmPtr>(LHS),
             RHSC = dyn_cast<VASTImmPtr>(RHS);
  if (LHSC) {
    ++CmpEquOptimized;

    APInt LHSInt = LHSC.getAPInt();
    // Replace C >= X by C + 1 > X if C is not the Max value.
    if (LHSInt != GetMax(Opc, SizeInBits)) {
      ++LHSInt;
      return buildICmpExpr(Opc, getImmediate(LHSInt), RHS);
    }

    // Else, Max >= X is always true!
    return VASTImmediate::True;
  }

  if (RHSC) {
    ++CmpEquOptimized;

    APInt RHSInt = RHSC.getAPInt();
    // Replace X >= C by X > C - 1 if C is not the Min value.
    if (RHSInt != GetMin(Opc, SizeInBits)) {
      --RHSInt;
      return buildICmpExpr(Opc, LHS, getImmediate(RHSInt));
    }

    // Else X >= Min is always true!
    return VASTImmediate::True;
  }

  return buildOrExpr(buildICmpExpr(Opc, LHS, RHS), buildEQ(LHS, RHS), 1);
}
