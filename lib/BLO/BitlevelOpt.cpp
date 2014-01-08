//==----------- BitlevelOpt.cpp - Bit-level Optimization ----------*- C++ -*-=//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the BitLevelOpt pass.
// The BitLevelOpt pass perform the bit-level optimizations iteratively until
// the bit-level optimization do not optimize the Module any further.
//
//===----------------------------------------------------------------------===//

#include "vast/BitlevelOpt.h"
#include "vast/Passes.h"
#include "vast/VASTModule.h"
#include "vast/Dataflow.h"
#include "vast/VASTModulePass.h"

#include "llvm/Analysis/Dominators.h"

#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-bit-level-opt"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(NumIterations, "Number of bit-level optimization iteration");
STATISTIC(NodesReplaced,
          "Number of Nodes are replaced during the bit-level optimization");
STATISTIC(NodesReplacedByKnownBits,
          "Number of Nodes whose bits are all known "
          "during the bit-level optimization");
STATISTIC(SlotsEliminated, "Number of unreachable states eliminated");

//===----------------------------------------------------------------------===//
/// Stole from LLVM's MathExtras.h
/// This function returns true if the argument is a sequence of ones starting
/// at the least significant bit with the remainder zero.
bool DatapathBLO::isMask(APInt Value) {
  //  Value && ((Value + 1) & Value) == 0;
  return Value.getBoolValue() && !(((Value + 1) & Value)).getBoolValue();
}

/// This function returns true if the argument contains a sequence of ones with
/// the remainder zero Ex. isShiftedMask_32(0x0000FF00U) == true.
bool DatapathBLO::isShiftedMask(APInt Value) {
  return isMask((Value - 1) | Value);
}

bool DatapathBLO::hasEnoughKnownbits(APInt KnownBits, bool FineGrain) {
  return DatapathBLO::isMask(KnownBits) || DatapathBLO::isShiftedMask(KnownBits) ||
         DatapathBLO::isMask(~KnownBits) || DatapathBLO::isShiftedMask(~KnownBits) ||
         FineGrain;
}

//===----------------------------------------------------------------------===//
DatapathBLO::DatapathBLO(DatapathContainer &Datapath)
  : MinimalExprBuilderContext(Datapath), Builder(*this) {}

DatapathBLO::~DatapathBLO() {}

void DatapathBLO::resetForNextIteration() {
  Visited.clear();
}

bool DatapathBLO::replaceIfNotEqual(VASTValPtr From, VASTValPtr To) {
  if (To == None || From == To)
    return false;

  replaceAllUseWith(From, To);
  ++NodesReplaced;

  // Now To is a optimized node, we will not optimize it again in the current
  // iteration.
  if (VASTExpr *Expr = dyn_cast<VASTExpr>(To.get()))
    Visited.insert(Expr);

  return true;
}

VASTValPtr DatapathBLO::eliminateInvertFlag(VASTValPtr V) {
  // There is not invert flag to fold.
  if (!V.isInverted())
    return V;

  VASTExpr *Expr = dyn_cast<VASTExpr>(V.get());

  if (Expr == NULL)
    return V;

  VASTExpr::Opcode Opcode = Expr->getOpcode();
  switch (Opcode) {
    // Only propagate the invert flag across these expressions:
  case VASTExpr::dpBitExtract:
  case VASTExpr::dpBitCat:
  case VASTExpr::dpBitRepeat:
  case VASTExpr::dpSAnn:
  case VASTExpr::dpHAnn:
    break;
    // Else stop propagating the invert flag here. In fact, the invert
    // flag cost nothing in LUT-based FPGA. What we worry about is the
    // invert flag may confuse the bit-level optimization.
  default:
    return V;
  }

  typedef VASTOperandList::op_iterator op_iterator;
  SmallVector<VASTValPtr, 8> InvertedOperands;
  // Collect the possible retimed operands.
  for (op_iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
    VASTValPtr Op = *I;
    InvertedOperands.push_back(Builder.buildNotExpr(Op));
  }

  return Builder.copyExpr(Expr, InvertedOperands);
}

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

  VASTValPtr And = Builder.buildAndExpr(Ops, BitWidth);

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

VASTValPtr DatapathBLO::optimizeAnnotation(VASTExpr::Opcode Opcode,
                                           VASTValPtr Op) {
  return Builder.buildAnnotation(Opcode, eliminateInvertFlag(Op));
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

VASTValPtr DatapathBLO::optimizeCmpWithConst(VASTExpr::Opcode Opcode,
                                             VASTValPtr X,
                                             const APInt &Const, bool VarAtLHS) {
  APInt Min = Opcode == VASTExpr::dpSGT ?
              APInt::getSignedMaxValue(Const.getBitWidth()) :
              APInt::getMaxValue(Const.getBitWidth());
  APInt Max = Opcode == VASTExpr::dpSGT ?
              APInt::getSignedMinValue(Const.getBitWidth()) :
              APInt::getMinValue(Const.getBitWidth());

  if (Const == Min) {
    // a > min <=> a != min.
    if (VarAtLHS)
      return Builder.buildNE(X, Builder.getConstant(Min));
    // min > a is always false,
    else
      return Builder.getConstant(false, 1);
  }

  if (Const == Max) {
    // a > max is always false.
    if (VarAtLHS)
      return Builder.getConstant(false, 1);
    // max > a <=> a != max
    else
      return Builder.buildNE(X, Builder.getConstant(Max));
  }

  if (Opcode == VASTExpr::dpSGT && Const.isMinValue()) {
    // a > 0 => signed bit == 0 && nonzero.
    if (VarAtLHS)
      return Builder.buildAndExpr(Builder.buildNotExpr(Builder.getSignBit(X)),
                                  Builder.buildROr(X),
                                  1);
    // 0 > a => signed bit == 1 && nonzero.
    else
      return Builder.buildAndExpr(Builder.getSignBit(X),
                                  Builder.buildROr(X),
                                  1);
  }

  if (Opcode == VASTExpr::dpSGT && Const.isAllOnesValue()) {
    // a > -1 <=> a >= 0 <=> signed bit == 0
    if (VarAtLHS)
      return Builder.buildNotExpr(Builder.getSignBit(X));
    // -1 > a <=> a < 0 && a != -1
    else {
      APInt MinusOne = APInt::getAllOnesValue(X->getBitWidth());
      VASTValPtr MinusOneImm = Builder.getConstant(MinusOne);
      return Builder.buildAndExpr(Builder.getSignBit(X),
                                  Builder.buildNE(X, MinusOneImm),
                                  1);
    }
  }

  VASTValPtr LHS = VarAtLHS ? X : getConstant(Const);
  VASTValPtr RHS = VarAtLHS ? getConstant(Const) : X;

  return Builder.buildICmpExpr(Opcode, LHS, RHS);
}

VASTValPtr DatapathBLO::optimizeSGT(VASTValPtr LHS, VASTValPtr RHS) {
  LHS = eliminateInvertFlag(LHS);
  RHS = eliminateInvertFlag(RHS);

  unsigned OperandWidth = LHS->getBitWidth();
  assert(OperandWidth == RHS->getBitWidth() && "Operand bitwidth doesn't match!");

  VASTConstPtr LHSC = dyn_cast<VASTConstPtr>(LHS),
               RHSC = dyn_cast<VASTConstPtr>(RHS);

  // Calculate the results of ICmp now.
  if (LHSC != None && RHSC != None)
    return getConstant(LHSC.getAPInt().sgt(RHSC.getAPInt()), 1);

  if (RHSC != None)
    return optimizeCmpWithConst(VASTExpr::dpSGT, LHS, RHSC.getAPInt(), true);

  if (LHSC != None)
    return optimizeCmpWithConst(VASTExpr::dpSGT, RHS, LHSC.getAPInt(), false);

  return Builder.buildICmpExpr(VASTExpr::dpSGT, LHS, RHS);
}

VASTValPtr DatapathBLO::optimizeUGT(VASTValPtr LHS, VASTValPtr RHS) {
  LHS = eliminateInvertFlag(LHS);
  RHS = eliminateInvertFlag(RHS);

  unsigned OperandWidth = LHS->getBitWidth();
  assert(OperandWidth == RHS->getBitWidth() && "Operand bitwidth doesn't match!");

  VASTConstPtr LHSC = dyn_cast<VASTConstPtr>(LHS),
               RHSC = dyn_cast<VASTConstPtr>(RHS);

  // Calculate the results of ICmp now.
  if (LHSC != None && RHSC != None)
    return getConstant(LHSC.getAPInt().ugt(RHSC.getAPInt()), 1);

  if (RHSC != None)
    return optimizeCmpWithConst(VASTExpr::dpUGT, LHS, RHSC.getAPInt(), true);

  if (LHSC != None)
    return optimizeCmpWithConst(VASTExpr::dpSGT, RHS, LHSC.getAPInt(), false);

  return Builder.buildICmpExpr(VASTExpr::dpUGT, LHS, RHS);
}

void DatapathBLO::eliminateInvertFlag(MutableArrayRef<VASTValPtr> Ops) {
  for (unsigned i = 0; i < Ops.size(); ++i)
    Ops[i] = eliminateInvertFlag(Ops[i]);
}

VASTValPtr DatapathBLO::optimizeExpr(VASTExpr *Expr) {
  // Replace the expr by known bits if possible.
  VASTValPtr KnownBits = replaceKnownBits(Expr);
  if (KnownBits != Expr)
    return KnownBits;

  VASTExpr::Opcode Opcode = Expr->getOpcode();
  switch (Opcode) {
  case VASTExpr::dpAnd:
    return optimizeNAryExpr<VASTExpr::dpAnd>(Expr->getOperands(),
                                             Expr->getBitWidth());
  case VASTExpr::dpBitExtract: {
    VASTValPtr Op = Expr->getOperand(0);
    return optimizeBitExtract(Op, Expr->getUB(), Expr->getLB());
  }
  case VASTExpr::dpBitCat:
    return optimizeNAryExpr<VASTExpr::dpBitCat>(Expr->getOperands(),
                                                Expr->getBitWidth());
  case VASTExpr::dpBitRepeat: {
    unsigned Times = Expr->getRepeatTimes();

    VASTValPtr Pattern = Expr->getOperand(0);
    return optimizeBitRepeat(Pattern, Times);
  }
  case VASTExpr::dpAdd:
    return optimizeNAryExpr<VASTExpr::dpAdd>(Expr->getOperands(),
                                             Expr->getBitWidth());
  case VASTExpr::dpMul:
    return optimizeNAryExpr<VASTExpr::dpMul>(Expr->getOperands(),
                                             Expr->getBitWidth());
  case VASTExpr::dpRAnd:
  case VASTExpr::dpRXor:
    return optimizeReduction(Opcode, Expr->getOperand(0));
  case VASTExpr::dpSAnn:
  case VASTExpr::dpHAnn:
    return optimizeAnnotation(Opcode, Expr->getOperand(0));
  case VASTExpr::dpShl:
  case VASTExpr::dpLshr:
  case VASTExpr::dpAshr:
    return optimizeShift(Opcode, Expr->getOperand(0), Expr->getOperand(1),
                         Expr->getBitWidth());
  case VASTExpr::dpSGT:
    return optimizeSGT(Expr->getOperand(0), Expr->getOperand(1));
  case VASTExpr::dpUGT:
    return optimizeUGT(Expr->getOperand(0), Expr->getOperand(1));
    // Yet to be implement:
  case VASTExpr::dpLUT:
    break;
  // Strange expressions that we cannot optimize.
  default: break;
  }

  return Expr;
}

VASTValPtr DatapathBLO::replaceKnownBitsFromMask(VASTValPtr V, VASTBitMask Mask,
                                                 bool FineGrain) {
  APInt KnownBits = Mask.getKnownBits();

  if (KnownBits.isAllOnesValue()) {
    ++NodesReplacedByKnownBits;
    return getConstant(Mask.getKnownValues());
  }

  // Do nothing if there is no bits known.
  if (!KnownBits.getBoolValue())
    return V;

  if (!hasEnoughKnownbits(KnownBits, FineGrain))
    return V;

  SmallVector<unsigned, 8> SplitPos;
  unsigned Bitwidth = Mask.getMaskWidth();

  // Calculate the split points to split the known and unknon bits.
  for (unsigned i = 1; i < Bitwidth; ++i) {
    if (KnownBits[i] != KnownBits[i - 1])
      SplitPos.push_back(i);
  }

  SplitPos.push_back(Bitwidth);

  assert(SplitPos.size() > 1 && "Too few split points!");

  unsigned NumSegments = SplitPos.size();
  SmallVector<VASTValPtr, 8> Bits(NumSegments, None);
  unsigned LB = 0;
  for (unsigned i = 0; i < NumSegments; ++i) {
    unsigned UB = SplitPos[i];

    // Put the segments from MSB to LSB, which is required by the BitCat
    // expression.
    // Also, Use the known bits whenever possible.
    if (Mask.isAllBitKnown(UB, LB))
      Bits[NumSegments - i - 1] = getConstant(Mask.getKnownValues(UB, LB));
    else
      Bits[NumSegments - i - 1] = optimizeBitExtract(V, UB, LB);

    LB = UB;
  }

  return optimizedpBitCat<VASTValPtr>(Bits, Bitwidth);
}

VASTValPtr DatapathBLO::replaceKnownBits(VASTValPtr V) {
  VASTMaskedValue *MV = dyn_cast<VASTMaskedValue>(V.get());
  if (MV == NULL)
    return V;

  // Update the bitmask before we perform the optimization.
  MV->evaluateMask();

  if (LLVM_LIKELY(!MV->isAllBitKnown()))
    return V;

  // If all bit is known, simply return the constant to replace the expr.
  ++NodesReplacedByKnownBits;
  VASTBitMask Mask(V);
  return getConstant(Mask.getKnownValues());
}

bool DatapathBLO::optimizeAndReplace(VASTValPtr V) {
  VASTExpr *Expr = dyn_cast<VASTExpr>(V.get());

  if (Expr == NULL)
    return replaceIfNotEqual(V, replaceKnownBits(V));

  // This expression had been optimized in the current iteration.
  if (Visited.count(Expr))
    return false;

  bool Replaced = false;

  std::set<VASTExpr*> LocalVisited;
  std::vector<std::pair<VASTHandle, unsigned> > VisitStack;

  VisitStack.push_back(std::make_pair(Expr, 0));

  while (!VisitStack.empty()) {
    VASTExpr *CurNode = VisitStack.back().first.getAsLValue<VASTExpr>();
    unsigned &Idx = VisitStack.back().second;

    // We have visited all children of current node.
    if (Idx == CurNode->size()) {
      VisitStack.pop_back();
      VASTValPtr NewVal = replaceKnownBits(optimizeExpr(CurNode));
      Replaced |= replaceIfNotEqual(CurNode, NewVal);
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTExpr *ChildExpr = CurNode->getOperand(Idx).getAsLValue<VASTExpr>();
    ++Idx;

    if (ChildExpr == NULL)
      continue;

    // No need to visit the same node twice
    if (!LocalVisited.insert(ChildExpr).second || Visited.count(ChildExpr))
      continue;

    VisitStack.push_back(std::make_pair(ChildExpr, 0));
  }

  return Replaced;
}

bool DatapathBLO::optimizeSelector(VASTSelector *Sel) {
  bool Changed = false;

  VASTBitMask MuxMask(Sel->getBitWidth());

  if (Sel->isSelectorSynthesized()) {
    // Only optimize the guard and fanin
    optimizeAndReplace(Sel->getGuard());
    optimizeAndReplace(Sel->getFanin());

    // Slot and enable are always assigned by 1, but timing is important for
    // them so we cannot simply replace the output of Slot and Enables by 1.
    if (!Sel->isSlot() && !Sel->isEnable())
      MuxMask = Sel->getFanin();
  }

  typedef VASTSelector::const_iterator const_iterator;
  for (const_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    const VASTLatch &L = *I;

    if (Sel->isTrivialFannin(L))
      continue;

    Changed |= optimizeAndReplace(L);
    Changed |= optimizeAndReplace(L.getGuard());

    assert(VASTBitMask(L.getFanin()).isCompatibleWith(MuxMask)
           && "Bad bitmask evaluation!");
  }

  typedef VASTSelector::def_iterator def_iterator;
  for (def_iterator I = Sel->def_begin(), E = Sel->def_end(); I != E; ++I) {
    VASTSeqValue *SV = *I;
    VASTBitMask OldMask = *SV;

    SV->mergeAnyKnown(MuxMask);

    Changed |= optimizeAndReplace(SV);
    Changed |= OldMask != *SV;
  }
    
  return Changed;
}

bool DatapathBLO::optimizeForward(VASTModule &VM) {
  bool Changed = false;
  typedef VASTModule::selector_iterator iterator;

  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;

    Changed |= optimizeSelector(Sel);
  }

  return Changed;
}

namespace {
struct BitlevelOpt : public VASTModulePass {
  static char ID;
  BitlevelOpt() : VASTModulePass(ID) {
    initializeBitlevelOptPass(*PassRegistry::getPassRegistry());
  }

  bool runSingleIteration(VASTModule &VM, DatapathBLO &BLO);

  bool runOnVASTModule(VASTModule &VM);

  void eliminateDeadSlot(VASTCtrlRgn &R, DominatorTree &DT, Dataflow& DF);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<DominatorTree>();
    AU.addRequired<Dataflow>();
    AU.addPreservedID(PreSchedBindingID);
    AU.addPreservedID(ControlLogicSynthesisID);
    AU.addPreservedID(TimingDrivenSelectorSynthesisID);
    AU.addPreservedID(SelectorSynthesisForAnnotationID);
    AU.addPreservedID(STGDistancesID);
  }
};
}

char BitlevelOpt::ID = 0;
char &vast::BitlevelOptID = BitlevelOpt::ID;
INITIALIZE_PASS(BitlevelOpt, "vast-bit-level-opt",
                "Perform the bit-level optimization.",
                false, true)

bool BitlevelOpt::runSingleIteration(VASTModule &VM, DatapathBLO &BLO) {
  bool Changed = false;

  Changed |= BLO.optimizeForward(VM);
  Changed |= BLO.performLUTMapping();
  Changed |= BLO.shrink(VM);

  ++NumIterations;
  return Changed;
}

bool BitlevelOpt::runOnVASTModule(VASTModule &VM) {
  DatapathBLO BLO(VM);
  DominatorTree &DT = getAnalysis<DominatorTree>();
  Dataflow &DF = getAnalysis<Dataflow>();

  do {
    BLO.resetForNextIteration();
    eliminateDeadSlot(VM, DT, DF);
    VM.gc();
  } while (runSingleIteration(VM, BLO));

  return true;
}

static void UnlinkFromPreds(VASTSlot *S) {
  SmallVector<VASTSlot*, 4> Preds(S->pred_begin(), S->pred_end());

  while (!Preds.empty()) {
    VASTSlot *PredSlot = Preds.pop_back_val();
    // Remove the edge from the STG
    PredSlot->unlinkSucc(S);

    typedef VASTSlot::op_iterator op_iterator;
    // Locate the corresponding branch operation and erase it.
    for (op_iterator I = PredSlot->op_begin(); I != PredSlot->op_end(); ++I) {
      VASTSlotCtrl *Br = dyn_cast<VASTSlotCtrl>(*I);
      if (Br == NULL || !Br->isBranch())
        continue;

      if (Br->getTargetSlot() == S) {
        Br->eraseFromParent();
        break;
      }
    }
  }
}

void
BitlevelOpt::eliminateDeadSlot(VASTCtrlRgn &R, DominatorTree &DT, Dataflow &DF) {
  bool Changed = false;

  typedef VASTModule::slot_iterator slot_iterator;
  // Build the signals corresponding to the slots.
  for (slot_iterator I = R.slot_begin(), E = R.slot_end(); I != E; /*++I*/) {
    VASTSlot *S = I++;

    // The slot is dead.
    if (S->pred_size() == 0) {
      if (!S->IsSubGrp) {
        DF.addUnreachableBlocks(S->getParent());
        ++SlotsEliminated;
      }

      S->unlinkSuccs();
      S->eraseFromParent();
      Changed = true;
      continue;
    }

    SmallVector<VASTSlotCtrl*, 4> DeadOps;
    typedef VASTSlot::op_iterator op_iterator;
    for (op_iterator I = S->op_begin(); I != S->op_end(); ++I) {
      VASTSlotCtrl *SeqOp = dyn_cast<VASTSlotCtrl>(*I);
      if (SeqOp == NULL || !SeqOp->isBranch())
        continue;

      if (SeqOp->getGuard() != VASTConstant::False)
        continue;

      DeadOps.push_back(SeqOp);
    }

    while (!DeadOps.empty()) {
      VASTSlotCtrl *SeqOp = DeadOps.pop_back_val();

      VASTSlot *Succ = SeqOp->getTargetSlot();

      // Eliminate the STG edge and corresponding branch operation (i.e. state
      // transfer operation).
      S->unlinkSucc(Succ);
      SeqOp->eraseFromParent();

      // We can prove the slot dead according to the dominator tree: If the
      // dominating CFG edge will never be taken, the target BB of the edge
      // is dead.
      if (DT.dominates(S->getParent(), Succ->getParent())) {
        UnlinkFromPreds(Succ);
        Changed = true;
      }
    }
  }

  if (!Changed)
    return;

  // Start over if any slot is deleted, this may expose more dead blocks.
  return eliminateDeadSlot(R, DT, DF);
}

Pass *vast::createBitlevelOptPass() {
  return new BitlevelOpt();
}
