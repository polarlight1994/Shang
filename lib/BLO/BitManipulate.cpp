//===----- BitManipulate.cpp - Perform BitManipulate in BLO  ----*- C++ -*-===//
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

#define DEBUG_TYPE "vast-bit-manipulate"
#include "llvm/Support/Debug.h"

using namespace vast;

VASTValPtr DatapathBLO::optimizeBitRepeat(VASTValPtr Pattern, unsigned Times) {
  Pattern = eliminateInvertFlag(Pattern);

  // This is not a repeat at all.
  if (Times == 1)
    return Pattern;

  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(Pattern)) {
    // Repeat the constant bit pattern.
    if (C->getBitWidth() == 1) {
      return C.getBoolValue() ?
             getConstant(APInt::getAllOnesValue(Times)) :
             getConstant(APInt::getNullValue(Times));
    }
  }

  return Builder.buildBitRepeat(Pattern, Times);
}

VASTValPtr
DatapathBLO::optimizeBitExtract(VASTValPtr V, unsigned UB, unsigned LB) {
  V = eliminateInvertFlag(V);
  unsigned OperandSize = V->getBitWidth();
  // Not a sub bitslice.
  if (UB == OperandSize && LB == 0)
    return V;

  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(V))
    return getConstant(C.getBitSlice(UB, LB));

  VASTExpr *Expr = dyn_cast<VASTExpr>(V.get());

  if (Expr == NULL)
    return Builder.buildBitExtractExpr(V, UB, LB);

  if (Expr->getOpcode() == VASTExpr::dpBitExtract){
    assert(!V.isInverted() &&
           "Invert flag of bitextract should had been eliminated!");
    unsigned Offset = Expr->getLB();
    UB += Offset;
    LB += Offset;
    return optimizeBitExtract(Expr->getOperand(0), UB, LB);
  }

  if (Expr->getOpcode() == VASTExpr::dpBitCat) {
    assert(!V.isInverted() &&
           "Invert flag of bitextract should had been eliminated!");
    // Collect the bitslices which fall into (UB, LB]
    SmallVector<VASTValPtr, 8> Ops;
    unsigned CurUB = Expr->getBitWidth(), CurLB = 0;
    unsigned LeadingBitsToLeft = 0, TailingBitsToTrim = 0;
    for (unsigned i = 0; i < Expr->size(); ++i) {
      VASTValPtr CurBitSlice = Expr->getOperand(i);
      CurLB = CurUB - CurBitSlice->getBitWidth();
      // Not fall into (UB, LB] yet.
      if (CurLB >= UB) {
        CurUB = CurLB;
        continue;
      }
      // The entire range is visited.
      if (CurUB <= LB)
        break;
      // Now we have CurLB < UB and CurUB > LB.
      // Compute LeadingBitsToLeft if UB fall into [CurUB, CurLB), which imply
      // CurUB >= UB >= CurLB.
      if (CurUB >= UB)
        LeadingBitsToLeft = UB - CurLB;
      // Compute TailingBitsToTrim if LB fall into (CurUB, CurLB], which imply
      // CurUB >= LB >= CurLB.
      if (LB >= CurLB)
        TailingBitsToTrim = LB - CurLB;

      Ops.push_back(CurBitSlice);
      CurUB = CurLB;
    }

    // Trivial case: Only 1 bitslice in range.
    if (Ops.size() == 1)
      return optimizeBitExtract(Ops.back(), LeadingBitsToLeft, TailingBitsToTrim);

    Ops.front() = optimizeBitExtract(Ops.front(), LeadingBitsToLeft, 0);
    Ops.back() = optimizeBitExtract(Ops.back(), Ops.back()->getBitWidth(),
                                    TailingBitsToTrim);

    return optimizeNAryExpr<VASTExpr::dpBitCat, VASTValPtr>(Ops, UB - LB);
  }

  if (Expr->getOpcode() == VASTExpr::dpBitRepeat) {
    assert(!V.isInverted() &&
           "Invert flag of bitextract should had been eliminated!");
    VASTValPtr Pattern = Expr->getOperand(0);
    // Simply repeat the pattern by the correct number.
    if (Pattern->getBitWidth() == 1)
      return optimizeBitRepeat(Pattern, UB - LB);
    // TODO: Build the correct pattern.
  }

  return Builder.buildBitExtractExpr(V, UB, LB);
}

static VASTExpr *GetAsBitExtractExpr(VASTValPtr V) {
  VASTExpr *Expr = dyn_cast<VASTExpr>(V);
  if (Expr == NULL || !Expr->isSubWord())
    return NULL;

  assert(!V.isInverted() &&
         "Invert flag of bitextract should had been eliminated!");
  return Expr;
}

VASTValPtr DatapathBLO::optimizeBitCatImpl(MutableArrayRef<VASTValPtr> Ops,
                                           unsigned BitWidth) {
  VASTConstPtr LastC = dyn_cast<VASTConstPtr>(Ops[0]);
  VASTExpr *LastBitSlice = GetAsBitExtractExpr(Ops[0]);

  unsigned ActualOpPos = 1;

  // Merge the constant sequence.
  for (unsigned i = 1, e = Ops.size(); i < e; ++i) {
    VASTValPtr V = Ops[i];
    if (VASTConstPtr CurC = dyn_cast<VASTConstPtr>(V)) {
      if (LastC != None) {
        // Merge the constants.
        APInt HiVal = LastC.getAPInt(), LoVal = CurC.getAPInt();
        unsigned HiSizeInBits = LastC->getBitWidth(),
                 LoSizeInBits = CurC->getBitWidth();
        unsigned SizeInBits = LoSizeInBits + HiSizeInBits;
        APInt Val = LoVal.zextOrSelf(SizeInBits);
        Val |= HiVal.zextOrSelf(SizeInBits).shl(LoSizeInBits);
        Ops[ActualOpPos - 1] = (LastC = getConstant(Val)); // Modify back.
        continue;
      } else {
        LastC = CurC;
        Ops[ActualOpPos++] = V; //push_back.
        continue;
      }
    } else // Reset LastImm, since the current value is not immediate.
      LastC = None;

    if (VASTExpr *CurBitSlice = GetAsBitExtractExpr(V)) {
      VASTValPtr CurBitSliceParent = CurBitSlice->getOperand(0);
      if (LastBitSlice && CurBitSliceParent == LastBitSlice->getOperand(0)
          && LastBitSlice->getLB() == CurBitSlice->getUB()) {
        VASTValPtr MergedBitSlice
          = optimizeBitExtract(CurBitSliceParent, LastBitSlice->getUB(),
                               CurBitSlice->getLB());
        Ops[ActualOpPos - 1] = MergedBitSlice; // Modify back.
        LastBitSlice = GetAsBitExtractExpr(MergedBitSlice);
        continue;
      } else {
        LastBitSlice = CurBitSlice;
        Ops[ActualOpPos++] = V; //push_back.
        continue;
      }
    } else
      LastBitSlice = 0;

    Ops[ActualOpPos++] = V; //push_back.
  }

  Ops = Ops.slice(0, ActualOpPos);
  if (Ops.size() == 1)
    return Ops.back();

#ifndef NDEBUG
  unsigned TotalBits = 0;
  for (unsigned i = 0, e = Ops.size(); i < e; ++i)
    TotalBits += Ops[i]->getBitWidth();
  if (TotalBits != BitWidth) {
    dbgs() << "Bad bitcat operands: \n";
    for (unsigned i = 0, e = Ops.size(); i < e; ++i)
      Ops[i]->dump();
    llvm_unreachable("Bitwidth not match!");
  }
#endif

  return Builder.buildBitCatExpr(Ops, BitWidth);
}

VASTValPtr DatapathBLO::optimizeAndPatialKnowns(MutableArrayRef<VASTValPtr> Ops,
                                                unsigned BitWidth) {
  if (BitWidth == 1)
    return Builder.buildAndExpr(Ops, BitWidth);

  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    APInt CurKnowns = VASTBitMask(Ops[i]).getKnownBits();

    if (!isMask(CurKnowns) && !isMask(~CurKnowns))
      return Builder.buildAndExpr(Ops, BitWidth);

    if (CurKnowns.countPopulation() < BitWidth / 2)
      return Builder.buildAndExpr(Ops, BitWidth);
  }

  // Split the expression in the middle.
  unsigned SplitPos[] = { BitWidth / 2, BitWidth };
  return splitAndConCat<VASTExpr::dpAnd>(Ops, BitWidth, SplitPos);
}

VASTValPtr DatapathBLO::optimizeAndImpl(MutableArrayRef<VASTValPtr> Ops,
                                        unsigned BitWidth) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1)
   return Ops[0];

  std::sort(Ops.begin(), Ops.end(), VASTValPtr::type_less);

  APInt C = APInt::getAllOnesValue(BitWidth);
  VASTValPtr LastVal = None;
  unsigned ActualPos = 0;

  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    VASTValPtr CurVal = Ops[i];
    if (CurVal == LastVal) {
      // A & A = A
      continue;
    } else if (Builder.buildNotExpr(CurVal) == LastVal)
      // A & ~A => 0
      return getConstant(APInt::getNullValue(BitWidth));

    // Ignore the 1s
    if (VASTConstPtr CurC = dyn_cast<VASTConstPtr>(CurVal)) {
      C &= CurC.getAPInt();
      continue;
    }

    Ops[ActualPos++] = CurVal;
    LastVal = CurVal;
  }

  // The result of and become all zero if the constant mask is zero.
  // Also return the Constant if all operand is folded into the constant.
  if (!C || ActualPos == 0)
    return getConstant(C);

  // Resize the operand vector so it only contains valid operands.
  Ops = Ops.slice(0, ActualPos);

  VASTValPtr And = optimizeAndPatialKnowns(Ops, BitWidth);

  // Build the bitmask expression if we get some mask.
  if (!C.isAllOnesValue()) {
    // Perform knwon bits replacement.
    if (hasEnoughKnownbits(C, false)) {
      // Create the mask with knwon zeros.
      VASTBitMask Mask(~C, APInt::getNullValue(BitWidth));
      return replaceKnownBitsFromMask(And, Mask, false);
    }

    And = Builder.buildBitMask(And, C);
  }

  return And;
}

VASTValPtr DatapathBLO::optimizeReduction(VASTExpr::Opcode Opc, VASTValPtr Op) {
  Op = eliminateInvertFlag(Op);

  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(Op)) {
    APInt Val = C.getAPInt();
    switch (Opc) {
    case VASTExpr::dpRAnd:
      // Only reduce to 1 if all bits are 1.
      if (Val.isAllOnesValue())
        return getConstant(true, 1);
      else
        return getConstant(false, 1);
    case VASTExpr::dpRXor:
      // Only reduce to 1 if there are odd 1s.
      if (Val.countPopulation() & 0x1)
        return getConstant(true, 1);
      else
        return getConstant(false, 1);
      break; // FIXME: Who knows how to evaluate this?
    default:  llvm_unreachable("Unexpected Reduction Node!");
    }
  }

  // Promote the reduction to the operands.
  if (VASTExpr *Expr = dyn_cast<VASTExpr>(Op)) {
    switch (Expr->getOpcode()) {
    default: break;
    case VASTExpr::dpBitCat: {
      SmallVector<VASTValPtr, 8> Ops;
      typedef VASTExpr::op_iterator op_iterator;
      for (op_iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I)
        Ops.push_back(optimizeReduction(Opc, *I));

      switch (Opc) {
      case VASTExpr::dpRAnd:
        return optimizeNAryExpr<VASTExpr::dpAnd, VASTValPtr>(Ops, 1);
      case VASTExpr::dpRXor:
        return Builder.buildXorExpr(Ops, 1);
      default:  llvm_unreachable("Unexpected Reduction Node!");
      }
    }
    }
  }

  return Builder.buildReduction(Opc, Op);
}

VASTValPtr DatapathBLO::optimizeShift(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS,
                                      unsigned BitWidth) {
  LHS = eliminateInvertFlag(LHS);
  RHS = eliminateInvertFlag(RHS);

  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(RHS)) {
    unsigned ShiftAmount = C.getZExtValue();

    // If we not shift at all, simply return the operand.
    if (ShiftAmount == 0)
     return LHS;

    switch(Opc) {
    case VASTExpr::dpShl:{
      VASTValPtr PaddingBits = getConstant(0, ShiftAmount);
      LHS = optimizeBitExtract(LHS, LHS->getBitWidth() - ShiftAmount, 0);
      VASTValPtr Ops[] = { LHS, PaddingBits };
      return optimizeNAryExpr<VASTExpr::dpBitCat, VASTValPtr>(Ops, BitWidth);
    }
    case VASTExpr::dpLshr:{
      VASTValPtr PaddingBits = getConstant(0, ShiftAmount);
      LHS = optimizeBitExtract(LHS, LHS->getBitWidth(), ShiftAmount);
      VASTValPtr Ops[] = { PaddingBits, LHS };
      return optimizeNAryExpr<VASTExpr::dpBitCat, VASTValPtr>(Ops, BitWidth);
    }
    case VASTExpr::dpAshr:{
      VASTValPtr SignBits = optimizeBitRepeat(optimizeSignBit(LHS), ShiftAmount);
      LHS = optimizeBitExtract(LHS, LHS->getBitWidth(), ShiftAmount);
      VASTValPtr Ops[] = { SignBits, LHS };
      return optimizeNAryExpr<VASTExpr::dpBitCat, VASTValPtr>(Ops, BitWidth);
    }
    default: llvm_unreachable("Unexpected opcode!"); break;
    }
  }

  return Builder.buildShiftExpr(Opc, LHS, RHS, BitWidth);
}
