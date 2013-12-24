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

class VASTBitMask {
  APInt KnownZeros, KnownOnes;
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

  unsigned getMaskWidth() const;

  APInt getKnownBits() const;
  bool anyBitKnown() const;
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

  void printMask(raw_ostream &OS) const;
  void printMaskIfAnyKnown(raw_ostream &OS) const;
  void dumpMask() const;

  void verify() const;
};
}

#endif
