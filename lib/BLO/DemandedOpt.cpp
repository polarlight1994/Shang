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
#include "BitlevelOpt.h"

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

  VASTValPtr replaceKnownBitsFromMask(VASTValPtr V, VASTBitMask Mask,
                                      bool FineGrain);

  // Shrink V and replace V on all its user.
  bool shrinkAndReplace(VASTValPtr V, VASTBitMask Mask, bool FineGrain);
  // Shrink the value used by U, and replace it on U only.
  bool fineGrainShrinkAndReplace(VASTUse &U, VASTBitMask Mask);
};
}

//===----------------------------------------------------------------------===//
/// Stole from LLVM's MathExtras.h
/// This function returns true if the argument is a sequence of ones starting
/// at the least significant bit with the remainder zero.
static bool isMask(APInt Value) {
  //  Value && ((Value + 1) & Value) == 0;
  return Value.getBoolValue() && !(((Value + 1) & Value)).getBoolValue();
}

/// This function returns true if the argument contains a sequence of ones with
/// the remainder zero Ex. isShiftedMask_32(0x0000FF00U) == true.
static bool isShiftedMask(APInt Value) {
  return isMask((Value - 1) | Value);
}

static bool hasEnoughKnownbits(APInt KnownBits, bool FineGrain) {
  return isMask(KnownBits) || isShiftedMask(KnownBits) ||
         isMask(~KnownBits) || isShiftedMask(~KnownBits) ||
         FineGrain;
}

VASTValPtr DemandedBitOptimizer::replaceKnownBitsFromMask(VASTValPtr V,
                                                          VASTBitMask Mask,
                                                          bool FineGrain) {
  APInt KnownBits = Mask.getKnownBits();

  if (KnownBits.isAllOnesValue())
    return BLO->getConstant(Mask.getKnownValues());

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
      Bits[NumSegments - i - 1] = BLO->getConstant(Mask.getKnownValues(UB, LB));
    else
      Bits[NumSegments - i - 1] = BLO.optimizeBitExtract(V, UB, LB);

    LB = UB;
  }

  return BLO.optimizedpBitCat<VASTValPtr>(Bits, Bitwidth);
}

bool
DemandedBitOptimizer::fineGrainShrinkAndReplace(VASTUse &U, VASTBitMask Mask) {
  VASTValPtr V = U;
  // Also use the mask of V it self.
  Mask.mergeAnyKnown(V);

  VASTValPtr NewV = replaceKnownBitsFromMask(V, Mask, true);
  if (NewV == V)
    return false;

#ifdef XDEBUG
  if (VASTMaskedValue *MV = dyn_cast<VASTMaskedValue>(NewV.get()))
    MV->evaluateMask();
  VASTBitMask NewMask = NewV;
  assert(NewMask == Mask && "Bit Mask not match!");
#endif

  U.replaceUseBy(NewV);
  return true;
}

bool DatapathBLO::shrink(VASTModule &VM) {
  bool Changed = false;
  DemandedBitOptimizer DBO(*this);

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
      VASTValPtr NewAnn = DBO.replaceKnownBitsFromMask(Ann, Ann, FineGrainOpt);
      // Replace the old keep expression by the new one.
      replaceIfNotEqual(Keep, optimizeAnnotation(Keep->getOpcode(), NewAnn));
    }
  }

  Visited.clear();
  return Changed;
}
