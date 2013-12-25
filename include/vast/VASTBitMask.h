//===-------------- VASTBitMask.h - BitMask of VASTValues -------*- C++ -*-===//
//
//                       The VAST HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the VASTBitMask class, it represent the bitmask of a
// VASTValue.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_BITMASK_H
#define VAST_BITMASK_H

#include "llvm/ADT/APInt.h"

namespace llvm {
class Value;
class DataLayout;
class raw_ostream;
class ScalarEvolution;
}

namespace vast {
using namespace llvm;
class VASTMaskedValue;
class VASTExpr;
class VASTSeqValue;

class VASTBitMask {
  APInt KnownZeros, KnownOnes;
protected:
  void evaluateMask(VASTMaskedValue *V);
  void evaluateMask(VASTExpr *E);
  void evaluateMask(VASTSeqValue *V);

  void printMaskVerification(raw_ostream &OS, const VASTMaskedValue *V) const;
  void printMaskVerification(raw_ostream &OS, const VASTExpr *E) const;
public:
  explicit VASTBitMask(unsigned Size)
    : KnownZeros(APInt::getNullValue(Size)),
      KnownOnes(APInt::getNullValue(Size))
  {}

  VASTBitMask(APInt KnownZeros, APInt KnownOnes)
    : KnownZeros(KnownZeros), KnownOnes(KnownOnes) {
    assert(KnownOnes.getBitWidth() == KnownZeros.getBitWidth()
           && "Bitwidths are not agreed!");
    verify();
  }

  APInt getKnownZeros() const { return KnownZeros; }
  APInt getKnownOnes() const { return KnownOnes; }

  bool isAllKnownZero() const { return KnownZeros.isAllOnesValue(); }

  bool isKnownZeroAt(unsigned N) const {
    return (KnownZeros & APInt::getOneBitSet(getMaskWidth(), N)).getBoolValue();
  }

  bool isKnownOneAt(unsigned N) const {
    return (KnownOnes & APInt::getOneBitSet(getMaskWidth(), N)).getBoolValue();
  }

  void setKnwonZeroAt(unsigned i) {
    KnownOnes.clearBit(i);
    KnownZeros.setBit(i);
  }

  /// \brief Zero extend or truncate to width
  ///
  /// Make this VASTBitMask have the bit width given by \p width. The value is
  /// zero extended, truncated, or left alone to make it that width.
  VASTBitMask zextOrTrunc(unsigned Width) const {
    APInt NewKnownZeros = KnownZeros.zextOrTrunc(Width);

    // Set the Zero extended bits to zeros, if there is any.
    int ExtraBits = int(Width) - getMaskWidth();
    if (ExtraBits > 0)
      NewKnownZeros |= APInt::getHighBitsSet(Width, ExtraBits);

    return VASTBitMask(NewKnownZeros, KnownOnes.zextOrTrunc(Width));
  }

  VASTBitMask shl(unsigned i) {
    // Shift left and fill the lower bit with zeros.
    return VASTBitMask(KnownZeros.shl(i)|APInt::getLowBitsSet(getMaskWidth(),i),
                       KnownOnes.shl(i));
  }

  unsigned getMaskWidth() const;

  // Functions for the known bits of the BitMask
  APInt getKnownBits() const;

  APInt getKnownValue(unsigned UB, unsigned LB = 0) const;
  APInt getKnownValue() const {
    return getKnownValue(getMaskWidth(), 0);
  }

  bool isAllBitKnown(unsigned UB, unsigned LB = 0) const;

  bool isAllBitKnown() const {
    return isAllBitKnown(getMaskWidth(), 0);
  }

  bool anyBitKnown() const;
  bool anyKnownZero() const { return getKnownZeros().getBoolValue(); }
  bool anyKnownOne() const { return getKnownOnes().getBoolValue(); }

  VASTBitMask invert(bool invert = true) const {
    return invert ? VASTBitMask(KnownOnes, KnownZeros)
                  : VASTBitMask(KnownZeros, KnownOnes);
  }

  bool operator==(const VASTBitMask &RHS) const {
    return KnownZeros == RHS.KnownZeros && KnownOnes == RHS.KnownOnes;
  }

  // Compute the bit mask for a LLVM Value, by calling ComputeMaskedBits.
  void mergeAnyKnown(Value *V, ScalarEvolution &SE, const DataLayout &TD,
                     bool Inverted = false);
  // Merge any known bits from other to the current bit mask
  void mergeAnyKnown(const VASTBitMask &Other);
  void mergeAllKnown(const VASTBitMask &Other);

  void printMask(raw_ostream &OS) const;
  void printMaskIfAnyKnown(raw_ostream &OS) const;
  void dumpMask() const;

  // Mask Evaluation function.
  static VASTBitMask EvaluateAnd(ArrayRef<VASTBitMask> Masks, unsigned BitWidth);
  static VASTBitMask EvaluateOr(VASTBitMask LHS, VASTBitMask RHS,
                                unsigned BitWidth);
  static VASTBitMask EvaluateAnd(VASTBitMask LHS, VASTBitMask RHS,
                                 unsigned BitWidth);
  static VASTBitMask EvaluateXor(VASTBitMask LHS, VASTBitMask RHS,
                                 unsigned BitWidth);
  static VASTBitMask EvaluateLUT(ArrayRef<VASTBitMask> Masks, unsigned BitWidth,
                                 const char *SOP);

  static VASTBitMask EvaluateBitCat(ArrayRef<VASTBitMask> Masks,
                                    unsigned BitWidth);
  static VASTBitMask EvaluateBitExtract(VASTBitMask Mask,
                                        unsigned UB, unsigned LB);
  
  static VASTBitMask EvaluateAdd(VASTBitMask LHS, VASTBitMask RHS,
                                 unsigned BitWidth);
  static VASTBitMask EvaluateMul(VASTBitMask LHS, VASTBitMask RHS,
                                 unsigned BitWidth);

  static VASTBitMask EvaluateShl(VASTBitMask LHS, VASTBitMask RHS,
                                 unsigned BitWidth);
  static VASTBitMask EvaluateLshr(VASTBitMask LHS, VASTBitMask RHS,
                                  unsigned BitWidth);

  void verify() const;
};
}

#endif
