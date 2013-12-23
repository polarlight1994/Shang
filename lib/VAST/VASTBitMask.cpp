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

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_os_ostream.h"
#define DEBUG_TYPE "vast-bit-mask"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace vast;
//===--------------------------------------------------------------------===//
APInt BitMasks::getKnownBits() const {
  return KnownZeros | KnownOnes;
}

bool BitMasks::isSubSetOf(const BitMasks &RHS) const {
  assert(!(KnownOnes & RHS.KnownZeros)
    && !(KnownZeros & RHS.KnownOnes)
    && "Bit masks contradict!");

  APInt KnownBits = getKnownBits(), RHSKnownBits = RHS.getKnownBits();
  if (KnownBits == RHSKnownBits) return false;

  return (KnownBits | RHSKnownBits) == RHSKnownBits;
}

void BitMasks::dump() const {
  SmallString<128> Str;
  KnownZeros.toString(Str, 2, false, true);
  dbgs() << "Known Zeros\t" << Str << '\n';
  Str.clear();
  KnownOnes.toString(Str, 2, false, true);
  dbgs() << "Known Ones\t" << Str << '\n';
  Str.clear();
  getKnownBits().toString(Str, 2, false, true);
  dbgs() << "Known Bits\t" << Str << '\n';
}


