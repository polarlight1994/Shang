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

APInt VASTBitMask::getKnownBits() const {
  return KnownZeros | KnownOnes;
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
  OS << "/*\n";
  SmallString<128> Str;
  KnownZeros.toString(Str, 2, false, true);
  OS << "Known Zeros\t" << Str << '\n';
  Str.clear();
  KnownOnes.toString(Str, 2, false, true);
  OS << "Known Ones\t" << Str << '\n';
  Str.clear();
  getKnownBits().toString(Str, 2, false, true);
  OS << "Known Bits\t" << Str << '\n';
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

  mergeAnyKnown(Mask.invert(Inverted));
}

void VASTBitMask::mergeAnyKnown(const VASTBitMask &Other) {
  assert(Other.getMaskWidth() == getMaskWidth() && "Size of V is unknown!");
  assert(!(KnownOnes & Other.KnownZeros) && !(KnownZeros & Other.KnownOnes) &&
         "Bit masks contradict!");

  KnownOnes |= Other.KnownOnes;
  KnownZeros |= Other.KnownZeros;
  verify();
}
