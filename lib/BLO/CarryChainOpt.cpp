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


//===----------------------------------------------------------------------===//
namespace {
template<VASTExpr::Opcode Opcode>
struct CarryChainOpt {
  DatapathBLO &BLO;
  CarryChainOpt(DatapathBLO *BLO) : BLO(*BLO) {}

  VASTValPtr optimize(MutableArrayRef<VASTValPtr> Ops, unsigned BitWidth);
  VASTValPtr optimizeBinary(VASTValPtr LHS, VASTValPtr RHS, unsigned BitWidth);
};
}

//===----------------------------------------------------------------------===//
template<VASTExpr::Opcode Opcode>
VASTValPtr CarryChainOpt<Opcode>::optimizeBinary(VASTValPtr LHS, VASTValPtr RHS,
                                                 unsigned BitWidth) {
  if (Opcode == VASTExpr::dpAdd) {
    // Try to break the carry chain.
    VASTBitMask InitialCarries = VASTBitMask::EvaluateAnd(LHS, RHS, BitWidth);

    // If the initial carries are all zero, there will be no carry at all during
    // the addition. Lower the Addition to OR (or XOR).
    if (InitialCarries.isAllZeroKnown())
      return BLO->buildOrExpr(LHS, RHS, BitWidth);

    // Chop of the carry chain from LSB.
    unsigned TrailingZeros = InitialCarries.getKnownZeros().countTrailingOnes();
    if (TrailingZeros) {
      VASTValPtr LHSLo = BLO.optimizeBitExtract(LHS, TrailingZeros, 0),
                 RHSLo = BLO.optimizeBitExtract(RHS, TrailingZeros, 0);
      VASTValPtr Lo = BLO->buildOrExpr(LHSLo, RHSLo, TrailingZeros);

      VASTValPtr LHSHi = BLO.optimizeBitExtract(LHS, BitWidth, TrailingZeros),
                 RHSHi = BLO.optimizeBitExtract(RHS, BitWidth, TrailingZeros);
      VASTValPtr His[] = { LHSHi, RHSHi };
      VASTValPtr Hi = BLO.optimizeAdd<VASTValPtr>(His, BitWidth - TrailingZeros);
      VASTValPtr Segments[] = { Hi, Lo };
      return BLO.optimizedpBitCat<VASTValPtr>(Segments, BitWidth);
    }
  }

  // Evaluate the carry bits.
  return BLO->buildExpr(Opcode, LHS, RHS, BitWidth);
}

template<VASTExpr::Opcode Opcode>
VASTValPtr CarryChainOpt<Opcode>::optimize(MutableArrayRef<VASTValPtr> Ops,
                                           unsigned BitWidth) {
  // Early return for the trivial case.
  if (Ops.size() == 1)
    return Ops[0];

  // Short cut for most common cases
  if (Ops.size() == 2)
    return optimizeBinary(Ops[0], Ops[1], BitWidth);

  return BLO->buildExpr(Opcode, Ops, BitWidth);
}

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

  return CarryChainOpt<VASTExpr::dpAdd>(this).optimize(Ops, BitWidth);
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

  // Implement the Mul by addition if the population of the constant is low.
  if (C.countPopulation() <= 2u) {
    // Remove the place holder for the constant operand.
    Ops = Ops.slice(0, Ops.size() - 1);
    VASTValPtr SubExpr = Builder.buildMulExpr(Ops, BitWidth);
    
    SmallVector<VASTValPtr, 4> Addends;
    for (unsigned i = 0; i < BitWidth; ++i) {
      if (!C[i])
        continue;

      // Implement the multiplication by shift.
      Addends.push_back(optimizeShift(VASTExpr::dpShl,
                                      SubExpr, getConstant(i, BitWidth),
                                      BitWidth));
    }

    return optimizeAdd<VASTValPtr>(Addends, BitWidth);
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

    return optimizeAdd<VASTValPtr>(AddOps, BitWidth);
  }

  // If the constant is a invert bitmask like 0xff00, we can perform the following
  // transform:
  // A * 0xff00 = (A * (0x00ff << 8)) = (A * 0x00ff) << 8
  // Now we get a multiplication with bitmask, which can be handled by the
  // code above.
  if (isMask(~C)) {
    // Remove the place holder for the constant operand.
    Ops = Ops.slice(0, Ops.size() - 1);
    unsigned lg2 = C.countTrailingZeros();

    VASTValPtr SubMul = Ops.size() == 1 ? Ops[0] :
                        Builder.buildMulExpr(Ops, BitWidth);
    VASTValPtr NewOperands[] = { SubMul, None };
    SubMul = optimizeMulWithConst(NewOperands, C.lshr(lg2), BitWidth);
    return optimizeShift(VASTExpr::dpShl, SubMul, getConstant(lg2, BitWidth),
                         BitWidth);
  }

  // Else we have to build the multiplication with the constant.
  Ops.back() = getConstant(C);
  
  return CarryChainOpt<VASTExpr::dpMul>(this).optimize(Ops, BitWidth);
}
