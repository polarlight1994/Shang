//===- DemandedOpt.cpp - Eliminate the bits are not used --------*- C++ -*-===//
//
//                      The VAST HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement DemandedBitsOpt class, it try to eliminate all known bits
// in the datapath.
//
//===----------------------------------------------------------------------===//
#include "vast/BitlevelOpt.h"
#include "vast/VASTModule.h"
#include "vast/PatternMatch.h"

#define DEBUG_TYPE "vast-demanded-opt"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace vast;
using namespace PatternMatch;

namespace {
struct DemandedBitOptimizer {
  DatapathBLO &BLO;

  explicit DemandedBitOptimizer(DatapathBLO &BLO) : BLO(BLO) {}

  APInt getUsedBits(VASTValPtr V) {
    unsigned BitWidth = V->getBitWidth();

    // Do not worry about the 1 bit value ...
    if (BitWidth == 1 || V->use_empty())
      return APInt::getAllOnesValue(BitWidth);

    APInt AnyUsed = APInt::getNullValue(BitWidth);

    typedef VASTValue::use_iterator iterator;
    for (iterator I = V->use_begin(), E = V->use_end(); I != E; ++I) {
      VASTExpr *Expr = dyn_cast<VASTExpr>(*I);

      // Any expr user is not a bitextract use all bits.
      if (Expr == NULL || Expr->getOpcode() != VASTExpr::dpBitExtract)
        // Bits are used only if they are unknown.
        return ~VASTBitMask(V).getKnownBits();

      APInt CurUsed = APInt::getBitsSet(BitWidth, Expr->getLB(), Expr->getUB());
      AnyUsed |= CurUsed;
    }

    // Do not messup with the Exprs with strange user...
    if (!AnyUsed)
      return APInt::getAllOnesValue(BitWidth);

    return AnyUsed;
  }

  VASTValPtr shrink(VASTExpr *Expr) {
    VASTExpr::Opcode Opcode = Expr->getOpcode();

    switch (Opcode) {
    default: break;
    case VASTExpr::dpLUT:
      break;
    case VASTExpr::dpAnd:
      return shrinkParallel<VASTExpr::dpAnd>(Expr);
    case VASTExpr::dpAdd:
      return shrinkCarryChain<VASTExpr::dpAdd>(Expr);
    case VASTExpr::dpMul:
      return shrinkCarryChain<VASTExpr::dpMul>(Expr);
    case VASTExpr::dpShl:
      return shrinkShiftLeft(Expr);
    case VASTExpr::dpAshr:
      return shrinkShiftRight<VASTExpr::dpAshr>(Expr);
    case VASTExpr::dpLshr:
      return shrinkShiftRight<VASTExpr::dpLshr>(Expr);
    }

    return Expr;
  }

  template<VASTExpr::Opcode Opcode>
  VASTValPtr shrinkParallel(VASTExpr *Expr);

  template<VASTExpr::Opcode Opcode>
  VASTValPtr shrinkCarryChain(VASTExpr *Expr);

  VASTValPtr shrinkShiftLeft(VASTExpr *Expr);

  template<VASTExpr::Opcode Opcode>
  VASTValPtr shrinkShiftRight(VASTExpr *Expr);

  // Shrink V and replace V on all its user.
  bool shrinkAndReplace(VASTValPtr V, VASTBitMask Mask, bool FineGrain);
  // Shrink the value used by U, and replace it on U only.
  bool fineGrainShrinkAndReplace(VASTUse &U, VASTBitMask Mask);
};
}

//===----------------------------------------------------------------------===//
template<VASTExpr::Opcode Opcode>
VASTValPtr DemandedBitOptimizer::shrinkParallel(VASTExpr *Expr) {
  APInt KnownBits = Expr->getKnownBits();
  unsigned BitWidth = KnownBits.getBitWidth();
  SmallVector<unsigned, 8> SplitPos;

  if (BLO.hasEnoughKnownbits(KnownBits, false))
    DatapathBLO::extractSplitPositions(KnownBits, SplitPos);
  else {
    APInt KnownBits = ~getUsedBits(Expr);

    if (!BLO.hasEnoughKnownbits(KnownBits, false))
      return Expr;

    DatapathBLO::extractSplitPositions(KnownBits, SplitPos);
  }

  return BLO.splitAndConCat<Opcode>(Expr->getOperands(), BitWidth, SplitPos);
}

template<VASTExpr::Opcode Opcode>
VASTValPtr DemandedBitOptimizer::shrinkCarryChain(VASTExpr *Expr) {
  APInt KnownBits = Expr->getKnownBits();

  // Only trim the leading bits, it is the carry chaings optimizations to
  // trim the tailing bits.
  unsigned Leadings = KnownBits.countLeadingOnes();
  unsigned Bitwidth = Expr->getBitWidth();

  if (Leadings == 0) {
    // Get the leading unused bits.
    APInt UsedBits = getUsedBits(Expr);
    Leadings = UsedBits.countLeadingZeros();

    if (Leadings == 0)
      return Expr;
  }

  unsigned UB = Bitwidth - Leadings;

  unsigned SplitPos[] = { 0, UB };
  VASTValPtr Lo = BLO.splitAndConCat<Opcode>(Expr->getOperands(), UB, SplitPos);
  VASTValPtr Hi = BLO->getConstant(Expr->getKnownValues(Bitwidth, UB));
  VASTValPtr Segments[] = { Hi, Lo };
  return BLO.optimizedpBitCat<VASTValPtr>(Segments, Bitwidth);
}

VASTValPtr DemandedBitOptimizer::shrinkShiftLeft(VASTExpr *Expr) {
  APInt KnownBits = Expr->getKnownBits();

  // Only trim the leading bits
  unsigned Leadings = KnownBits.countLeadingOnes();
  unsigned Bitwidth = Expr->getBitWidth();

  if (Leadings == 0) {
    // Get the leading unused bits.
    APInt UsedBits = getUsedBits(Expr);
    Leadings = UsedBits.countLeadingZeros();

    if (Leadings == 0)
      return Expr;
  }

  unsigned UB = Bitwidth - Leadings;
  VASTValPtr LHS = BLO.optimizeBitExtract(Expr->getOperand(0), UB, 0);
  VASTValPtr Lo = BLO.optimizeShift(VASTExpr::dpShl, LHS, Expr->getOperand(1),
                                    UB);
  VASTValPtr Hi = BLO->getConstant(Expr->getKnownValues(Bitwidth, UB));
  VASTValPtr Segments[] = { Hi, Lo };
  return BLO.optimizedpBitCat<VASTValPtr>(Segments, Bitwidth);
}

template<VASTExpr::Opcode Opcode>
VASTValPtr DemandedBitOptimizer::shrinkShiftRight(VASTExpr *Expr) {
  APInt KnownBits = Expr->getKnownBits();

  // Only trim the tailing bits.
  unsigned Trailing = KnownBits.countTrailingOnes();
  unsigned Bitwidth = Expr->getBitWidth();

  if (Trailing == 0) {
    // Get the tailing unused bits.
    APInt UsedBits = getUsedBits(Expr);
    Trailing = UsedBits.countTrailingZeros();

    if (Trailing == 0)
      return Expr;
  }

  unsigned LB = Trailing;
  VASTValPtr LHS = BLO.optimizeBitExtract(Expr->getOperand(0), Bitwidth, Trailing);
  VASTValPtr Hi = BLO.optimizeShift(Opcode, LHS, Expr->getOperand(1),
                                    Bitwidth - Trailing);
  VASTValPtr Lo = BLO->getConstant(Expr->getKnownValues(Trailing, 0));
  VASTValPtr Segments[] = { Hi, Lo };
  return BLO.optimizedpBitCat<VASTValPtr>(Segments, Bitwidth);
}

bool
DemandedBitOptimizer::fineGrainShrinkAndReplace(VASTUse &U, VASTBitMask Mask) {
  VASTValPtr V = U;
  // Also use the mask of V it self.
  Mask.mergeAnyKnown(V);

  VASTValPtr NewV = BLO.replaceKnownBitsFromMask(V, Mask, true);
  if (NewV == V)
    return false;

#ifdef XDEBUG
  if (VASTMaskedValue *MV = dyn_cast<VASTMaskedValue>(NewV.get()))
    MV->evaluateMask();
  VASTBitMask NewMask = NewV;
  assert(NewMask == Mask && "Bit Mask not match!");
#endif

  BLO.replaceUseOf(NewV, U);
  return true;
}

bool DatapathBLO::shrink(VASTModule &VM) {
  bool Changed = false;
  DemandedBitOptimizer DBO(*this);

  bool DatapathChanged = true;

  while (DatapathChanged) {
    DatapathChanged = false;
    typedef DatapathContainer::expr_iterator iterator;
    for (iterator I = Datapath.expr_begin(); I != Datapath.expr_end(); /*++I*/) {
      VASTExpr *Expr = I;

      // Use Handle to trace the potantial replacement.
      VASTHandle VH(++I);

      // Do not need to worry about the dead expressions.
      if (Expr->use_empty())
        continue;

      VASTValPtr NewVal = DBO.shrink(Expr);
      if (replaceIfNotEqual(Expr, NewVal)) {
        Changed = DatapathChanged = true;
        // Recover the iterator from the replacement.
        I = VH.getAsLValue<VASTExpr>();
      }
    }
  }

  typedef VASTModule::selector_iterator selector_iterator;
  for (selector_iterator I = VM.selector_begin(), E = VM.selector_end();
       I != E; ++I) {
    VASTSelector *Sel = I;

    typedef VASTSelector::def_iterator def_iterator;
    for (def_iterator I = Sel->def_begin(), E = Sel->def_end(); I != E; ++I) {
      VASTSeqValue *SV = *I;

      typedef VASTSeqValue::fanin_iterator fainin_iterator;
      for (fainin_iterator I = SV->fanin_begin(), E = SV->fanin_end();
           I != E; ++I) {
        VASTLatch L = *I;

        if (Sel->isTrivialFannin(L))
          continue;

        // Preform fine grain shrinking on fanin of register assignment, avoid
        // *any* known bits!
        DBO.fineGrainShrinkAndReplace(L, *SV);
      }
    }

    SmallVector<VASTExpr*, 8> Worklist;
    typedef VASTSelector::ann_iterator ann_iterator;
    for (ann_iterator I = Sel->ann_begin(), E = Sel->ann_end(); I != E; ++I)
      Worklist.push_back(I->first);

    while (!Worklist.empty()) {
      VASTExpr *Keep = Worklist.pop_back_val();
      VASTValPtr Ann = matchUnderlying(Keep, extract_annotation());
      assert(Ann && "Unexpected expression type for annotation!");
      // Perform fine grain optimization on the hard annotation since we do not
      // allow the logic synthesis tool to optimize it.
      bool FineGrainOpt = Keep->isHardAnnotation();
      VASTValPtr NewAnn = replaceKnownBitsFromMask(Ann, Ann, FineGrainOpt);
      // Replace the old keep expression by the new one.
      replaceIfNotEqual(Keep, optimizeAnnotation(Keep->getOpcode(), NewAnn));
    }
  }

  Visited.clear();
  return Changed;
}
