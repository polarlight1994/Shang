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

static bool IsPointerOrInt(Value *V) {
  return V->getType()->isIntOrIntVectorTy() ||
         V->getType()->getScalarType()->isPointerTy();
}

void VASTBitMask::init(Value *V, const DataLayout *TD, bool Inverted) {
  // We cannot anything if V is not integer.
  if (!IsPointerOrInt(V))
    return;

  unsigned SizeInBits = TD->getTypeSizeInBits(V->getType());
  VASTBitMask Mask(SizeInBits);

  ComputeMaskedBits(V, Mask.KnownZeros, Mask.KnownOnes, TD);

  init(Mask, Inverted);
}

void VASTBitMask::init(const VASTBitMask &Other, bool Inverted) {
  assert(Other.getMaskWidth() == getMaskWidth() && "Size of V is unknown!");
  assert((!anyBitKnown() || operator==(Other.invert(Inverted))) &&
         "Cannot initialize nonzero!");

  KnownOnes = Inverted ? Other.KnownZeros : Other.KnownOnes;
  KnownZeros = Inverted ? Other.KnownOnes : Other.KnownZeros;
}
