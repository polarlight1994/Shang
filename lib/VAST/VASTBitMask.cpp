//===------------ VASTBitMask.cpp - BitMask of VASTValues -------*- C++ -*-===//
//
//                       The VAST HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VASTBitMask class, it represent the bitmask of a
// VASTValue.
//
//===----------------------------------------------------------------------===//

#include "vast/VASTBitMask.h"
#include "vast/VASTDatapathNodes.h"
#include "vast/VASTSeqValue.h"

#include "llvm/IR/Value.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vast-bit-mask"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace vast;
//===--------------------------------------------------------------------===//
void VASTBitMask::verify() const {
  assert(!(KnownOnes & KnownZeros) && "Bit masks contradict!");
}

bool VASTBitMask::isAllBitKnown(unsigned UB, unsigned LB) const {
  APInt KnownBits = getKnownBits();

  if (UB != getMaskWidth() || LB != 0)
    KnownBits = VASTConstant::getBitSlice(KnownBits, UB, LB);

  return KnownBits.isAllOnesValue();
}

APInt VASTBitMask::getKnownBits() const {
  return KnownZeros | KnownOnes;
}

APInt VASTBitMask::getKnownValue(unsigned UB, unsigned LB) const {
  assert(isAllBitKnown(UB, LB) && "The value is unknown!");

  verify();

  if (UB != getMaskWidth() || LB != 0)
    return VASTConstant::getBitSlice(KnownOnes, UB, LB);

  return KnownOnes;
}

bool VASTBitMask::anyBitKnown() const {
  return KnownOnes.getBoolValue() || KnownZeros.getBoolValue();
}

unsigned VASTBitMask::getMaskWidth() const {
  assert(KnownOnes.getBitWidth() == KnownOnes.getBitWidth()
          && "Bitwidths are not agreed!");
  return KnownOnes.getBitWidth();
}

void VASTBitMask::printMask(raw_ostream &OS) const {
  // This function may called when we are generating the Verilog code,
  // make sure their are comments.
  OS << "/*";
  SmallString<128> Str;
  KnownZeros.toString(Str, 2, false, false);
  OS << 'Z' << Str << ' ';
  Str.clear();
  KnownOnes.toString(Str, 2, false, false);
  OS << 'O' << Str << ' ';
  Str.clear();
  getKnownBits().toString(Str, 2, false, false);
  OS << 'K' << Str << ' ';
  OS << "*/\n";
}

void VASTBitMask::printMaskIfAnyKnown(raw_ostream &OS) const {
  if (anyBitKnown())
    printMask(OS);
}

void VASTBitMask::dumpMask() const {
  printMask(dbgs());
}

void VASTBitMask::mergeAnyKnown(Value *V, ScalarEvolution &SE,
                                const DataLayout &TD,
                                bool Inverted) {
  // We cannot anything if V is not integer.
  if (!SE.isSCEVable(V->getType()))
    return;

  unsigned SizeInBits = TD.getTypeSizeInBits(V->getType());
  VASTBitMask Mask(SizeInBits);

  ComputeMaskedBits(V, Mask.KnownZeros, Mask.KnownOnes, &TD);

  ConstantRange R = SE.getSignedRange(SE.getSCEV(V));

  // Further trim the unknown bits if the Range is zero based.
  if (R.getLower().isMinValue()) {
    APInt UB = R.getUpper();
    // Note that the upper bound of ConstantRange is not included in the range.
    --UB;
    unsigned LeadingZeros = UB.countLeadingZeros();
    Mask.KnownZeros |= APInt::getHighBitsSet(SizeInBits, LeadingZeros);
  }

  return mergeAnyKnown(Mask.invert(Inverted));
}

void VASTBitMask::mergeAnyKnown(const VASTBitMask &Other) {
  assert(Other.getMaskWidth() == getMaskWidth() && "Size of V is unknown!");
  assert(!(KnownOnes & Other.KnownZeros) && !(KnownZeros & Other.KnownOnes) &&
         "Bit masks contradict!");

  KnownOnes |= Other.KnownOnes;
  KnownZeros |= Other.KnownZeros;
  verify();
}

void VASTBitMask::evaluateMask(VASTMaskedValue *V) {
  if (VASTExpr *E = dyn_cast<VASTExpr>(V))
    return evaluateMask(E);

  if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(V))
    return evaluateMask(SV);
}


//===--------------------------------------------------------------------===//
template<typename T>
static
void ExtractBitMasks(ArrayRef<T> Ops, SmallVectorImpl<VASTBitMask> &Masks) {
  for (unsigned i = 0; i < Ops.size(); ++i) {
    VASTValPtr V = Ops[i];

    if (VASTConstPtr C = dyn_cast<VASTConstPtr>(V)) {
      APInt Bits = C.getAPInt();
      // Directly construct the mask from constant.
      Masks.push_back(VASTBitMask(~Bits, Bits));
      continue;
    }

    if (VASTMaskedValue *MV = dyn_cast<VASTMaskedValue>(V.get())) {
      Masks.push_back(MV->invert(V.isInverted()));
      continue;
    }

    // Else just push the all unknown mask.
    Masks.push_back(VASTBitMask(V->getBitWidth()));
  }
}

VASTBitMask VASTBitMask::EvaluateAndMask(ArrayRef<VASTBitMask> Masks,
                                         unsigned BitWidth) {
  // Assume all bits are 1s.
  VASTBitMask Mask(APInt::getNullValue(BitWidth),
                   APInt::getAllOnesValue(BitWidth));

  for (unsigned i = 0; i < Masks.size(); ++i) {
    // The bit become zero if the same bit in any operand is zero.
    Mask.KnownZeros |= Masks[i].KnownZeros;
    // The bit is one only if the same bit in all operand are zeros.
    Mask.KnownOnes &= Masks[i].KnownOnes;
  }

  return Mask;
}

VASTBitMask VASTBitMask::EvaluateAddMask(VASTBitMask LHS, VASTBitMask RHS,
                                         unsigned BitWidth) {
  //if (!(LHS.getKnownBits() | RHS.getKnownBits())) {
  //  // If the known bits are not overlapped, the addition become an OR.
  //  // Build OR by ~(~A & ~B)
  //  VASTBitMask Masks[] = { LHS.invert(), RHS.invert() };
  //  return EvaluateAndMask(Masks, BitWidth).invert();
  //}

  // Assume all bits are unknown.
  VASTBitMask Mask(BitWidth);

  // Well, steal from llvm ComputeMaskedBitsAddSub:
  unsigned LHSKnownTrailingZeros = LHS.KnownZeros.countTrailingOnes();
  unsigned RHSKnownTrailingZeros = RHS.KnownZeros.countTrailingOnes();

  // Determine which operand has more trailing zeros, and use that
  // many bits from the other operand.
  if (LHSKnownTrailingZeros > RHSKnownTrailingZeros) {
    APInt ZeroMask = APInt::getLowBitsSet(BitWidth, LHSKnownTrailingZeros);
    Mask.KnownZeros |= RHS.KnownZeros & ZeroMask;
    Mask.KnownOnes |= RHS.KnownOnes & ZeroMask;
  } else if (RHSKnownTrailingZeros >= LHSKnownTrailingZeros) {
    APInt ZeroMask = APInt::getLowBitsSet(BitWidth, RHSKnownTrailingZeros);
    Mask.KnownZeros |= LHS.KnownZeros & ZeroMask;
    Mask.KnownOnes |= LHS.KnownOnes & ZeroMask;
  }

  // The Carry bit will stop propagate if LHS and RHS has zero bit at the same
  // position.
  unsigned LHSKnownLeadingZeros= LHS.KnownZeros.countLeadingOnes();
  unsigned RHSKnownLeadingZeros = RHS.KnownZeros.countLeadingOnes();
  unsigned ResultKnownLeadingZeros = std::min(LHSKnownLeadingZeros,
                                              RHSKnownLeadingZeros);
  // The carry bit will eat a leading zero, if there is any.
  if (ResultKnownLeadingZeros > 0)
    ResultKnownLeadingZeros -= 1;

  Mask.KnownZeros |= APInt::getHighBitsSet(BitWidth, ResultKnownLeadingZeros);

  // TODO: Find the opportunity to break the carray chain.

  return Mask;
}

VASTBitMask VASTBitMask::EvaluateMultMask(VASTBitMask LHS, VASTBitMask RHS,
                                         unsigned BitWidth) {
  // Assume all bits are unknown.
  VASTBitMask Mask(BitWidth);
  unsigned TrailZeros = LHS.KnownZeros.countTrailingOnes() +
                        RHS.KnownZeros.countTrailingOnes();
  unsigned LeadZeros = std::max(LHS.KnownZeros.countLeadingOnes() +
                                RHS.KnownZeros.countLeadingOnes(),
                                BitWidth) - BitWidth;

  // If low bits are zero in either operand, output low known-0 bits.
  // Also compute a conserative estimate for high known-0 bits.
  TrailZeros = std::min(TrailZeros, BitWidth);
  LeadZeros = std::min(LeadZeros, BitWidth);
  Mask.KnownZeros = APInt::getLowBitsSet(BitWidth, TrailZeros) |
                    APInt::getHighBitsSet(BitWidth, LeadZeros);
  return Mask;
}

VASTBitMask VASTBitMask::EvaluateBitExtractMask(VASTBitMask Mask,
                                                unsigned UB, unsigned LB) {
  return VASTBitMask(VASTConstant::getBitSlice(Mask.KnownZeros, UB, LB),
                     VASTConstant::getBitSlice(Mask.KnownOnes, UB, LB));
}

VASTBitMask VASTBitMask::EvaluateBitCatMask(ArrayRef<VASTBitMask> Masks,
                                            unsigned BitWidth) {
  unsigned CurUB = BitWidth;
  unsigned ExprSize = BitWidth;

  // Assume all bits are unknown.
  VASTBitMask Mask(BitWidth);

  // Concatenate the bit mask together.
  for (unsigned i = 0; i < Masks.size(); ++i) {
    VASTBitMask CurMask = Masks[i];
    unsigned CurSize = CurMask.getMaskWidth();
    unsigned CurLB = CurUB - CurSize;
    Mask.KnownZeros |= CurMask.KnownZeros.zextOrSelf(ExprSize).shl(CurLB);
    Mask.KnownOnes  |= CurMask.KnownOnes.zextOrSelf(ExprSize).shl(CurLB);

    CurUB = CurLB;
  }

  return Mask;
}

//===--------------------------------------------------------------------===//
void VASTBitMask::evaluateMask(VASTExpr *E) {
  SmallVector<VASTBitMask, 8> Masks;
  ExtractBitMasks(E->getOperands(), Masks);
  unsigned BitWidth = E->getBitWidth();

  switch (E->getOpcode()) {
  default: break;
  case VASTExpr::dpBitExtract:
    mergeAnyKnown(EvaluateBitExtractMask(Masks[0], E->getUB(), E->getLB()));
    break;
  case VASTExpr::dpBitCat:
    mergeAnyKnown(EvaluateBitCatMask(Masks, BitWidth));
    break;
  case VASTExpr::dpAnd:
    mergeAnyKnown(EvaluateAndMask(Masks, BitWidth));
    break;
  case VASTExpr::dpAdd: {
    // Evaluate the bitmask pairwise for the ADD for now.
    while (Masks.size() > 1) {
      VASTBitMask LHS = Masks.pop_back_val().zextOrTrunc(BitWidth);
      VASTBitMask RHS = Masks.pop_back_val().zextOrTrunc(BitWidth);
      Masks.push_back(EvaluateAddMask(LHS, RHS, BitWidth));
    }

    mergeAnyKnown(Masks[0]);
    break;
  }
  case VASTExpr::dpMul: {
    // Evaluate the bitmask pairwise for the ADD for now.
    while (Masks.size() > 1) {
      VASTBitMask LHS = Masks.pop_back_val().zextOrTrunc(BitWidth);
      VASTBitMask RHS = Masks.pop_back_val().zextOrTrunc(BitWidth);
      Masks.push_back(EvaluateMultMask(LHS, RHS, BitWidth));
    }

    mergeAnyKnown(Masks[0]);
    break;
  }
  case VASTExpr::dpKeep:
    // Simply propagate the masks from the RHS of the assignment.
    mergeAnyKnown(Masks[0]);
    break;
  }
}

void VASTBitMask::evaluateMask(VASTSeqValue *SV) {
}
