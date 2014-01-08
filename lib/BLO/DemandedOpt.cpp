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

  VASTValPtr shrink(VASTExpr *Expr) {
    VASTExpr::Opcode Opcode = Expr->getOpcode();

    switch (Opcode) {
    default:
      break;
    case vast::VASTExpr::dpLUT:
      break;
    case VASTExpr::dpAnd:
      return shrinkParallel<VASTExpr::dpAnd>(Expr);
    case vast::VASTExpr::dpAdd:
      break;
    case vast::VASTExpr::dpMul:
      break;
    case vast::VASTExpr::dpShl:
      break;
    case vast::VASTExpr::dpAshr:
      break;
    case vast::VASTExpr::dpLshr:
      break;
    case vast::VASTExpr::dpROMLookUp:
      break;
    }

    return Expr;
  }

  template<VASTExpr::Opcode Opcode>
  VASTValPtr shrinkParallel(VASTExpr *Expr);
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

  if (!BLO.hasEnoughKnownbits(Expr->getKnownBits(), false))
    return Expr;

  unsigned Bitwidth = KnownBits.getBitWidth();

  SmallVector<unsigned, 8> SplitPos;
  DatapathBLO::extractSplitPositions(KnownBits, SplitPos);

  unsigned NumSegments = SplitPos.size();
  SmallVector<VASTValPtr, 8> Bits(NumSegments, None);
  unsigned LB = 0;

  for (unsigned i = 0; i < NumSegments; ++i) {
    unsigned UB = SplitPos[i];

    // Use the knwon bits when ever possible.
    if (Expr->isAllBitKnown(UB, LB)) {
      Bits[NumSegments - i - 1]
        = BLO->getConstant(Expr->getKnownValues(UB, LB));
      LB = UB;
      continue;
    }

    SmallVector<VASTValPtr, 8> Operands;

    // Build the And for the current segment.
    for (unsigned j = 0, e = Expr->size(); j < e; ++j) {
      VASTValPtr V = Expr->getOperand(j);
      VASTBitMask CurMask = V;

      if (CurMask.isAllBitKnown(UB, LB))
        V = BLO->getConstant(CurMask.getKnownValues(UB, LB));
      else
        V = BLO.optimizeBitExtract(V, UB, LB);

      Operands.push_back(V);
    }

    // Put the segments from MSB to LSB, which is required by the BitCat
    // expression.
    Bits[NumSegments - i - 1]
      = BLO.optimizeNAryExpr<Opcode, VASTValPtr>(Operands, UB - LB);

    LB = UB;
  }

  return BLO.optimizedpBitCat<VASTValPtr>(Bits, Bitwidth);
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
