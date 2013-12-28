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
class VASTValue;
template<typename T> struct PtrInvPair;

class VASTBitMask {
  APInt KnownZeros, KnownOnes;
protected:
  void evaluateMask(VASTMaskedValue *V);
  void evaluateMask(VASTExpr *E);
  void evaluateMask(VASTSeqValue *V);

  void evaluateFUOutputMask(VASTSeqValue *V);

  APInt getBitSliceImpl(const APInt &Int, unsigned UB, unsigned LB) const;
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

  /*implicit*/ VASTBitMask(PtrInvPair<VASTValue> V);

#define CREATEBITACCESSORS(WHAT, VALUE) \
  APInt getKnown##WHAT##s() const { return VALUE; } \
  APInt getKnown##WHAT##s(unsigned UB, unsigned LB = 0) const { \
  return getBitSliceImpl(getKnown##WHAT##s(), UB, LB); \
  } \
  bool is##WHAT##KnownAt(unsigned N) const { \
    return get##Known##WHAT##s()[N]; \
  } \
  bool isAll##WHAT##Known() const { \
    return get##Known##WHAT##s().isAllOnesValue(); \
  } \
  bool isAll##WHAT##Known(unsigned UB, unsigned LB = 0) const { \
    return getBitSliceImpl(get##Known##WHAT##s(), UB, LB).isAllOnesValue(); \
  } \
  bool hasAny##WHAT##Known() const { \
    return get##Known##WHAT##s().getBoolValue(); \
  } \
  bool hasAny##WHAT##Known(unsigned UB, unsigned LB = 0) const { \
    return getBitSliceImpl(get##Known##WHAT##s(), UB, LB).getBoolValue(); \
  }

  CREATEBITACCESSORS(One, KnownOnes)
  CREATEBITACCESSORS(Zero, KnownZeros)
  CREATEBITACCESSORS(Bit, KnownOnes | KnownZeros)
  CREATEBITACCESSORS(Value, (KnownOnes & ~KnownZeros))

  void setKnwonZeroAt(unsigned i) {
    KnownOnes.clearBit(i);
    KnownZeros.setBit(i);
  }

  void setKnwonOneAt(unsigned i) {
    KnownZeros.clearBit(i);
    KnownOnes.setBit(i);
  }

  // Calculate the Hamming Distance between bitmasks, the unknown bits are
  // considered always different.
  unsigned calculateDistanceFrom(const VASTBitMask &RHS) const;

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
    APInt LowZeros = APInt::getLowBitsSet(getMaskWidth(), i);
    return VASTBitMask(KnownZeros.shl(i) | LowZeros, KnownOnes.shl(i));
  }

  VASTBitMask lshr(unsigned i) {
    // Shift right and fill the higher bit with zeros.
    APInt HighZeros = APInt::getHighBitsSet(getMaskWidth(), i);
    return VASTBitMask(KnownZeros.lshr(i) | HighZeros, KnownOnes.lshr(i));
  }

  VASTBitMask ashr(unsigned i) {
    // Shift right and fill the higher bits only they are known.
    return VASTBitMask(KnownZeros.ashr(i), KnownOnes.ashr(i));
  }

  unsigned getMaskWidth() const;

  VASTBitMask invert(bool invert = true) const {
    return invert ? VASTBitMask(KnownOnes, KnownZeros)
                  : VASTBitMask(KnownZeros, KnownOnes);
  }

  bool operator==(const VASTBitMask &RHS) const {
    return KnownZeros == RHS.KnownZeros && KnownOnes == RHS.KnownOnes;
  }

  bool operator!=(const VASTBitMask &RHS) const {
    return !operator==(RHS);
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
  static VASTBitMask EvaluateAshr(VASTBitMask LHS, VASTBitMask RHS,
                                  unsigned BitWidth);

  void verify() const;

  void printMaskVerification(raw_ostream &OS, PtrInvPair<VASTValue> V) const;
};
}

#endif
