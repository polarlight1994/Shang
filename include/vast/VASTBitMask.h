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

namespace vast {
using namespace llvm;

struct BitMasks {
  APInt KnownZeros, KnownOnes;
  explicit BitMasks(unsigned Size)
    : KnownZeros(APInt::getNullValue(Size)),
      KnownOnes(APInt::getNullValue(Size))
  {}

  BitMasks(APInt KnownZeros = APInt(), APInt KnownOnes = APInt())
    : KnownZeros(KnownZeros), KnownOnes(KnownOnes) {
    assert(KnownOnes.getBitWidth() == KnownZeros.getBitWidth()
      && "Bitwidths are not agreed!");
  }

  APInt getKnownBits() const;
  // Return true if the known bits in the current mask is a subset of the known
  // bits in RHS.
  bool isSubSetOf(const BitMasks &RHS) const;

  void dump() const;
};
}

#endif
