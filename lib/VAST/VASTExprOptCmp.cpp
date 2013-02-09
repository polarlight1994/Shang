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

#include "VASTExprBuilder.h"

#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-expr-opt-cmp"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;
STATISTIC(ConstICmpLowered, "Number of Constant ICmp lowered");
STATISTIC(OneBitICmpLowered, "Number of 1 bit ICmp lowered");

static bool isSigned(VASTExpr::Opcode Opc) {
  switch (Opc) {
  case VASTExpr::dpUGE:
  case VASTExpr::dpUGT: return false;
  case VASTExpr::dpSGE:
  case VASTExpr::dpSGT: return true;
  default:
    llvm_unreachable("Unexpected opcode!");
  }

  return false;
}

static bool includeEQ(VASTExpr::Opcode Opc) {
  switch (Opc) {
  case VASTExpr::dpUGE:
  case VASTExpr::dpSGE: return true;
  case VASTExpr::dpUGT:
  case VASTExpr::dpSGT: return false;
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

static VASTValPtr Lower1BitICmp(VASTExprBuilder &Builder, VASTExpr::Opcode Opc,
                                VASTValPtr LHS, VASTValPtr RHS) {
  assert(LHS->getBitWidth() == 1 && LHS->getBitWidth() == 1
         && "Unexpected bitwidth in lower1BitICmp!");
  switch (Opc) {
  // A >= B <=> A == 1.
  case VASTExpr::dpUGE: return LHS;
  // A >= B <=> A == 1 && B == 0.
  case VASTExpr::dpUGT:
    return Builder.buildAndExpr(LHS, Builder.buildNotExpr(RHS), 1);
  // A >= B <=>  A == 0, because what we got is a signed bit.
  case VASTExpr::dpSGE:
    return Builder.buildNotExpr(LHS);
  // A > B <=>  A == 0 && B == 1.
  case VASTExpr::dpSGT:
    return Builder.buildAndExpr(Builder.buildNotExpr(LHS), RHS, 1);
  default:
    llvm_unreachable("Unexpected opcode!");
  }

  return VASTValPtr();
}

static VASTValPtr
FoldPatialConstICmp(VASTExprBuilder &Builder, bool IncludeEQ, bool IsSigned,
                    VASTValPtr Var, const APInt &Const, bool VarAtLHS,
                    const APInt &Max, const APInt &Min) {
  if (Const == Min && !IncludeEQ) {
    // a > min <=> a != min.
    if (VarAtLHS) return Builder.buildNE(Var, Builder.getImmediate(Min));
    // min > a is always false,
    else          return Builder.getBoolImmediate(false);
  }

  if (Const == Max && !IncludeEQ) {
    // a > max is always false.
    if (VarAtLHS) return Builder.getBoolImmediate(false);
    // max > a <=> a != max
    else          return Builder.buildNE(Var, Builder.getImmediate(Max));
  }

  if (Const == Min && IncludeEQ) {
    // a >= min is always true.
    if (VarAtLHS) return Builder.getBoolImmediate(true);
    // min >= a <=> a == min.
    else          return Builder.buildEQ(Var, Builder.getImmediate(Min));
  }

  if (Const == Max && IncludeEQ) {
    // a >= max <=> a == max
    if (VarAtLHS) return Builder.buildEQ(Var, Builder.getImmediate(Max));
    // max >= a is is always true
    else          return Builder.getBoolImmediate(true);
  }

  if (IsSigned && IncludeEQ && Const.isMinValue()) {
    // a >= 0 => signed bit == 0
    if (VarAtLHS) return Builder.buildNotExpr(Builder.getSignBit(Var));
    // 0 >= a => signed bit == 1
    else          return Builder.getSignBit(Var);

  }

  if (IsSigned && !IncludeEQ && Const.isMinValue()) {
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

  return VASTValPtr();
}


static
VASTValPtr FoldPatialConstICmp(VASTExprBuilder &Builder, VASTExpr::Opcode Opc,
                               VASTValPtr Var, const APInt &Const, bool VarAtLHS) {
  return FoldPatialConstICmp(Builder, includeEQ(Opc), isSigned(Opc),
                             Var, Const, VarAtLHS,
                             GetMax(Opc, Var->getBitWidth()),
                             GetMin(Opc, Var->getBitWidth()));
}

static VASTValPtr FoldConstICmp(VASTExprBuilder &Builder, VASTExpr::Opcode Opc,
                                VASTValPtr LHS, VASTValPtr RHS) {
  VASTImmPtr LHSC = dyn_cast<VASTImmPtr>(LHS),
             RHSC = dyn_cast<VASTImmPtr>(RHS);

  // Calculate the results of ICmp now.
  if (LHSC && RHSC) {
    switch (Opc) {
    case VASTExpr::dpUGE:
      return Builder.getImmediate(LHSC.getAPInt().uge(RHSC.getAPInt()), 1);
    case VASTExpr::dpUGT:
      return Builder.getImmediate(LHSC.getAPInt().ugt(RHSC.getAPInt()), 1);
    case VASTExpr::dpSGE:
      return Builder.getImmediate(LHSC.getAPInt().sge(RHSC.getAPInt()), 1);
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

  // Handle the trivial case trivially.
  if (RHS->getBitWidth() == 1) {
    ++OneBitICmpLowered;
    return Lower1BitICmp(*this, Opc, LHS, RHS);
  }

  if (VASTValPtr V = FoldConstICmp(*this, Opc, LHS, RHS)) {
    ++ConstICmpLowered;
    return V;
  }

  VASTValPtr Ops[] = { LHS, RHS };
  return createExpr(Opc, Ops, 1, 0);
}
