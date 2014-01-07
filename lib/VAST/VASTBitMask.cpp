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
#include "vast/VASTMemoryBank.h"

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
//===----------------------------------------------------------------------===//
VASTBitMask::VASTBitMask(VASTValPtr V)
  : KnownZeros(APInt::getNullValue(V->getBitWidth())),
    KnownOnes(APInt::getNullValue(V->getBitWidth())){
  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(V)) {
    APInt Bits = C.getAPInt();
    // Directly construct the mask from constant.
    KnownZeros = ~Bits;
    KnownOnes = Bits;
    return;
  }

  if (VASTMaskedValue *MV = dyn_cast<VASTMaskedValue>(V.get())) {
    VASTBitMask Mask = MV->invert(V.isInverted());
    KnownZeros = Mask.KnownZeros;
    KnownOnes = Mask.KnownOnes;
    return;
  }
}

void VASTBitMask::verify() const {
  assert(!(KnownOnes & KnownZeros) && "Bit masks contradict!");
}

APInt VASTBitMask::getBitSliceImpl(const APInt &Int,
                                   unsigned UB, unsigned LB) const {
  if (UB != Int.getBitWidth() || LB != 0)
    return VASTConstant::getBitSlice(Int, UB, LB);

  return Int;
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
  OS << "*/";
}

void VASTBitMask::printMaskIfAnyKnown(raw_ostream &OS) const {
  if (hasAnyBitKnown())
    printMask(OS);
}

void VASTBitMask::dumpMask() const {
  printMask(dbgs());
  dbgs() << '\n';
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

bool VASTBitMask::isCompatibleWith(const VASTBitMask &Other) const {
  return Other.getMaskWidth() == getMaskWidth() &&
         !(KnownOnes & Other.KnownZeros) && !(KnownZeros & Other.KnownOnes);
}

void VASTBitMask::mergeAnyKnown(const VASTBitMask &Other) {
  assert(isCompatibleWith(Other) && "Bit masks contradict!");

  KnownOnes |= Other.KnownOnes;
  KnownZeros |= Other.KnownZeros;
  verify();
}

void VASTBitMask::mergeAllKnown(const VASTBitMask &Other) {
  assert(Other.getMaskWidth() == getMaskWidth() && "Size of V is unknown!");

  KnownOnes &= Other.KnownOnes;
  KnownZeros &= Other.KnownZeros;
  verify();
}

unsigned VASTBitMask::calculateDistanceFrom(const VASTBitMask &RHS) const {
  APInt KnownBits = getKnownBits(), RHSKnownBits = getKnownBits();
  APInt AllKnownBits = KnownBits & RHSKnownBits;

  unsigned NumKnownBits = AllKnownBits.countPopulation();

  // If no bits are in common, we assume that they are all different.
  if (NumKnownBits == 0)
    return getMaskWidth();

  APInt KnownValue = getKnownValues() & AllKnownBits,
        RHSKnownValue = RHS.getKnownValues() & AllKnownBits;

  // Calculate the different between known bits.
  APInt Delta = KnownValue ^ RHSKnownValue;
  return Delta.countPopulation() + (getMaskWidth() - NumKnownBits);
}

void VASTBitMask::evaluateMask(VASTMaskedValue *V) {
  if (VASTExpr *E = dyn_cast<VASTExpr>(V))
    return evaluateMask(E);

  if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(V))
    return evaluateMask(SV);
}

//===----------------------------------------------------------------------===//
template<typename T>
static
void ExtractBitMasks(ArrayRef<T> Ops, SmallVectorImpl<VASTBitMask> &Masks) {
  for (unsigned i = 0; i < Ops.size(); ++i)
    Masks.push_back(VASTBitMask(Ops[i]));
}

//===----------------------------------------------------------------------===//
VASTBitMask
VASTBitMask::EvaluateAnd(ArrayRef<VASTBitMask> Masks, unsigned BitWidth) {
  // Assume all bits are 1s.
  VASTBitMask Mask(APInt::getNullValue(BitWidth),
                   APInt::getAllOnesValue(BitWidth));

  for (unsigned i = 0; i < Masks.size(); ++i) {
    // The bit become zero if the same bit in any operand is zero.
    Mask.KnownZeros |= Masks[i].KnownZeros;
    // The bit is one only if the same bit in all operand are ones.
    Mask.KnownOnes &= Masks[i].KnownOnes;
  }

  return Mask;
}

//===----------------------------------------------------------------------===//
VASTBitMask
VASTBitMask::EvaluateAnd(VASTBitMask LHS, VASTBitMask RHS, unsigned BitWidth) {
  return VASTBitMask(
    // The bit become zero if the same bit in any operand is zero.
    LHS.KnownZeros | RHS.KnownZeros,
    // The bit is one only if the same bit in all operand are ones.
    LHS.KnownOnes & RHS.KnownOnes);
}

//===----------------------------------------------------------------------===//
VASTBitMask
VASTBitMask::EvaluateOr(VASTBitMask LHS, VASTBitMask RHS, unsigned BitWidth) {
  return VASTBitMask(
    // The bit become zero if the same bit in all operand is zero.
    LHS.KnownZeros & RHS.KnownZeros,
    // The bit is one only if the same bit in any operand are ones.
    LHS.KnownOnes | RHS.KnownOnes);
}

//===----------------------------------------------------------------------===//
VASTBitMask
VASTBitMask::EvaluateXor(VASTBitMask LHS, VASTBitMask RHS, unsigned BitWidth) {
  return VASTBitMask(
    // Output known-0 bits are known if clear or set in both the LHS & RHS.
    (LHS.KnownZeros & RHS.KnownZeros) | (LHS.KnownOnes & RHS.KnownOnes),
    // Output known-1 are known to be set if set in only one of the LHS, RHS.
    (LHS.KnownZeros & RHS.KnownOnes) | (LHS.KnownOnes & RHS.KnownZeros));
}

//===----------------------------------------------------------------------===//
VASTBitMask
VASTBitMask::EvaluateLUT(ArrayRef<VASTBitMask> Masks, unsigned BitWidth,
                         const char *SOP) {
  bool IsComplement = false;
    // Interpret the sum of product table.
  const char *p = SOP;
  unsigned NumInputs = Masks.size();

  SmallVector<VASTBitMask, 6> ProductMasks;
  SmallVector<VASTBitMask, 6> SumMasks;

  while (*p) {
    ProductMasks.clear();
    // Interpret the product.
    for (unsigned i = 0; i < NumInputs; ++i) {
      char c = *p++;
      if (c == '-')
       continue;

      assert((c == '0' || c == '1') && "Unexpected SOP char!");
      // Put the operand into product masks vector, invert it if necessary.
      ProductMasks.push_back(Masks[i].invert(c == '0'));
    }

    // Inputs and outputs are seperated by blank space.
    assert(*p == ' ' && "Expect the blank space!");
    ++p;

    // Is the output inverted?
    char c = *p++;
    assert((c == '0' || c == '1') && "Unexpected SOP char!");
    VASTBitMask CurProduct = EvaluateAnd(ProductMasks, BitWidth);
    // We are going to evaluate A OR B by ~(~A AND ~B), so invert the mask
    // before we are putting it into the sum masks vector.
    SumMasks.push_back(CurProduct.invert());
    IsComplement |= (c == '0');

    // Products are separated by new line.
    assert(*p == '\n' && "Expect the new line!");
    ++p;
  }

  VASTBitMask Sum = EvaluateAnd(SumMasks, BitWidth).invert();

  // We need to invert the final result if the SOP is complemented.
  return Sum.invert(IsComplement);
}

//===----------------------------------------------------------------------===//
VASTBitMask VASTBitMask::EvaluateAdd(VASTBitMask LHS, VASTBitMask RHS,
                                     unsigned BitWidth) {
  // Perform bit level evaluation for addition.
  // unsigned int carry = a & b;
  // unsigned int result = a ^ b;
  // while (carry != 0) {
  //   unsigned int shiftedcarry = carry << 1;
  //   carry = result & shiftedcarry;
  //   result ^= shiftedcarry;
  // }
  // return result;

  VASTBitMask S = EvaluateXor(LHS, RHS, BitWidth),
              C = EvaluateAnd(LHS, RHS, BitWidth);

  for (unsigned i = 0; i < BitWidth; ++i) {
    if (!S.hasAnyBitKnown() || C.isAllZeroKnown())
      break;

    VASTBitMask ShiftedC = C.shl(1);
    C = EvaluateAnd(S, ShiftedC, BitWidth);
    S = EvaluateXor(S, ShiftedC, BitWidth);
  }

  return S;
}

//===----------------------------------------------------------------------===//
VASTBitMask VASTBitMask::EvaluateMul(VASTBitMask LHS, VASTBitMask RHS,
                                    unsigned BitWidth) {
  // Start from zero, and implement the multipication by "shift-and-add":
  // p = 0
  // for i = 0 to bitwidth
  //  p += (LHS << i) & Bitrepeat(RHS(i), bitwidth)
  //
  VASTBitMask P(APInt::getAllOnesValue(BitWidth),
                APInt::getNullValue(BitWidth));

  for (unsigned i = 0; i < BitWidth; ++i) {
    // Nothing to do if Both LHS and RHS is zero.
    if (LHS.isAllZeroKnown() || RHS.isAllZeroKnown() ||!P.hasAnyBitKnown())
      break;

    // If the i-th bit is known 1 in RHS, we always add the shifted LHS
    // to the result in this case.
    if (RHS.isOneKnownAt(i))
      P = EvaluateAdd(P, LHS, BitWidth);
    else if (!RHS.isZeroKnownAt(i)) {
      // If the current bit is unknown, we can only be sure about the zeros
      // that will be add to the product.
      VASTBitMask Mask(LHS.getKnownZeros(), APInt::getNullValue(BitWidth));
      P = EvaluateAdd(P, Mask, BitWidth);
    }

    LHS = LHS.shl(1);
    // Clear the bit after we had evaluated it.
    RHS.setKnwonZeroAt(i);
  }

  return P;
}

//===----------------------------------------------------------------------===//
VASTBitMask VASTBitMask::EvaluateShl(VASTBitMask LHS, VASTBitMask RHS,
                                     unsigned BitWidth) {
  unsigned RHSMaxSize = std::min(Log2_32_Ceil(LHS.getMaskWidth()),
                                 RHS.getMaskWidth());
  VASTBitMask M = LHS;
  for (unsigned i = 0; i < RHSMaxSize && M.hasAnyBitKnown(); ++i) {
    // If the i-th bit is known 1 in RHS, we always add the shifted LHS
    // to the result in this case.
    if (RHS.isOneKnownAt(i))
      M = M.shl(1 << i);
    // Otherwise if the bit at RHS is unknown, the result bits are known only
    // if the LHS bit is known no matter it is shifted or not.
    else if (!RHS.isZeroKnownAt(i))
      M.mergeAllKnown(M.shl(1 << i));
  }

  // TODO: Analyze RHS.
  return M;
}

//===----------------------------------------------------------------------===//
VASTBitMask VASTBitMask::EvaluateLshr(VASTBitMask LHS, VASTBitMask RHS,
                                      unsigned BitWidth) {
  unsigned RHSMaxSize = std::min(Log2_32_Ceil(LHS.getMaskWidth()),
                                 RHS.getMaskWidth());
  VASTBitMask M = LHS;
  for (unsigned i = 0; i < RHSMaxSize && M.hasAnyBitKnown(); ++i) {
    // If the i-th bit is known 1 in RHS, we always add the shifted LHS
    // to the result in this case.
    if (RHS.isOneKnownAt(i))
      M = M.lshr(1 << i);
    // Otherwise if the bit at RHS is unknown, the result bits are known only
    // if the LHS bit is known no matter it is shifted or not.
    else if (!RHS.isZeroKnownAt(i))
      M.mergeAllKnown(M.lshr(1 << i));
  }

  return M;
}

//===----------------------------------------------------------------------===//
VASTBitMask VASTBitMask::EvaluateAshr(VASTBitMask LHS, VASTBitMask RHS,
                                      unsigned BitWidth) {
  unsigned RHSMaxSize = std::min(Log2_32_Ceil(LHS.getMaskWidth()),
                                 RHS.getMaskWidth());
  VASTBitMask M = LHS;
  for (unsigned i = 0; i < RHSMaxSize && M.hasAnyBitKnown(); ++i) {
    // If the i-th bit is known 1 in RHS, we always add the shifted LHS
    // to the result in this case.
    if (RHS.isOneKnownAt(i))
      M = M.ashr(1 << i);
    // Otherwise if the bit at RHS is unknown, the result bits are known only
    // if the LHS bit is known no matter it is shifted or not.
    else if (!RHS.isZeroKnownAt(i))
      M.mergeAllKnown(M.ashr(1 << i));
  }

  return M;
}

//===----------------------------------------------------------------------===//
VASTBitMask VASTBitMask::EvaluateBitExtract(VASTBitMask Mask,
                                            unsigned UB, unsigned LB) {
  return VASTBitMask(VASTConstant::getBitSlice(Mask.KnownZeros, UB, LB),
                     VASTConstant::getBitSlice(Mask.KnownOnes, UB, LB));
}

VASTBitMask VASTBitMask::EvaluateBitCat(ArrayRef<VASTBitMask> Masks,
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

//===----------------------------------------------------------------------===//
void VASTBitMask::evaluateMask(VASTExpr *E) {
  SmallVector<VASTBitMask, 8> Masks;
  ExtractBitMasks(E->getOperands(), Masks);
  unsigned BitWidth = E->getBitWidth();

  switch (E->getOpcode()) {
  default: break;
  case VASTExpr::dpBitExtract:
    return mergeAnyKnown(EvaluateBitExtract(Masks[0], E->getUB(), E->getLB()));
  case VASTExpr::dpBitCat:
    return mergeAnyKnown(EvaluateBitCat(Masks, BitWidth));
  case VASTExpr::dpAnd:
  // BitMask expression is also an AND expression execpt that the RHS is
  // always constant.
  case VASTExpr::dpBitMask:
    return mergeAnyKnown(EvaluateAnd(Masks, BitWidth));
  case VASTExpr::dpLUT:
    return mergeAnyKnown(EvaluateLUT(Masks, BitWidth, E->getLUT()));
  case VASTExpr::dpAdd: {
    // Evaluate the bitmask pairwise for the ADD for now.
    while (Masks.size() > 1) {
      VASTBitMask LHS = Masks.pop_back_val().zextOrTrunc(BitWidth);
      VASTBitMask RHS = Masks.pop_back_val().zextOrTrunc(BitWidth);
      Masks.push_back(EvaluateAdd(LHS, RHS, BitWidth));
    }

    return mergeAnyKnown(Masks[0]);
  }
  case VASTExpr::dpMul: {
    // Evaluate the bitmask pairwise for the ADD for now.
    while (Masks.size() > 1) {
      VASTBitMask LHS = Masks.pop_back_val().zextOrTrunc(BitWidth);
      VASTBitMask RHS = Masks.pop_back_val().zextOrTrunc(BitWidth);
      Masks.push_back(EvaluateMul(LHS, RHS, BitWidth));
    }

    return mergeAnyKnown(Masks[0]);
  }
  case VASTExpr::dpShl:
    return mergeAnyKnown(EvaluateShl(Masks[0], Masks[1], BitWidth));
  case VASTExpr::dpLshr:
    return mergeAnyKnown(EvaluateLshr(Masks[0], Masks[1], BitWidth));
  case VASTExpr::dpAshr:
    return mergeAnyKnown(EvaluateAshr(Masks[0], Masks[1], BitWidth));
  case VASTExpr::dpSAnn:
  case VASTExpr::dpHAnn:
    // Simply propagate the masks from the RHS of the assignment.
    return mergeAnyKnown(Masks[0]);
  // Yet to be implement:
  case VASTExpr::dpSGT:
  case VASTExpr::dpUGT:
  case VASTExpr::dpROMLookUp:
    break;
  }
}

static void CollectAddrs(const LoadInst *LI, const VASTSelector *AddrPort,
                         SmallVectorImpl<VASTValPtr> &Addrs) {
  typedef VASTSelector::const_iterator const_iterator;
  for (const_iterator I = AddrPort->begin(), E = AddrPort->end(); I != E; ++I) {
    const VASTLatch &L = *I;
    if (L.Op->getValue() == LI)
      Addrs.push_back(L);
  }
}

void VASTBitMask::evaluateFUOutputMask(VASTSeqValue *SV) {
  VASTMemoryBank *Bank = dyn_cast<VASTMemoryBank>(SV->getParent());

  // Only evaluate the bitmask of memory bank output for now.
  if (Bank == NULL)
    return;

  // We are only going to evaluate the bitmask for the alignment of the data,
  // which is encoded in the higher part of the value.
  if (SV->getBitWidth() == Bank->getDataWidth())
    return;

  // Get the corresponding load instruction of this SV.
  LoadInst *LI = cast<LoadInst>(SV->getLLVMValue());

  SmallVector<VASTValPtr, 4> Addrs;
  // Now look for the same load instruction from the Address port. Since the
  // operation maybe duplicated, we need to collect *all* address assignments
  // derived from the same LoadInst.
  CollectAddrs(LI, Bank->getAddr(0), Addrs);
  if (Addrs.empty() && Bank->isDualPort())
    CollectAddrs(LI, Bank->getAddr(1), Addrs);

  assert(!Addrs.empty() && "Cannot find the corresponding address!");

  unsigned BytesPerWord = Bank->getDataWidth() / 8;
  unsigned ByteAddrWidth = Log2_32_Ceil(BytesPerWord);
  assert(ByteAddrWidth && "Should print as block RAM!");
  assert(ByteAddrWidth == SV->getBitWidth() - Bank->getDataWidth()
         && "Unexpected byte address width!");

  // Extract the bitmask from all address assignments.
  VASTBitMask ByteAddrMask(ByteAddrWidth);
  ByteAddrMask.KnownZeros = APInt::getAllOnesValue(ByteAddrWidth);
  ByteAddrMask.KnownOnes = APInt::getAllOnesValue(ByteAddrWidth);
  
  for (unsigned i = 0, e = Addrs.size(); i < e; ++i)
    ByteAddrMask.mergeAllKnown(EvaluateBitExtract(Addrs[i], ByteAddrWidth, 0));

  // And shift it to the correct place.
  ByteAddrMask = VASTBitMask(ByteAddrMask.KnownZeros.zextOrTrunc(SV->getBitWidth()),
                             ByteAddrMask.KnownOnes.zextOrTrunc(SV->getBitWidth()));
  ByteAddrMask = VASTBitMask(ByteAddrMask.KnownZeros.shl(Bank->getDataWidth()),
                             ByteAddrMask.KnownOnes.shl(Bank->getDataWidth()));
  mergeAnyKnown(ByteAddrMask);
}

void VASTBitMask::evaluateMask(VASTSeqValue *SV) {
  // Do not fail on some strange SeqValue.
  if (!SV->hasSelector())
    return;

  // Slot and enable are always assigned by 1, but timing is important for them
  // so we cannot simply replace the output of Slot and Enables by 1.
  if (SV->isSlot() || SV->isEnable())
    return;

  if (SV->isFUOutput())
    return evaluateFUOutputMask(SV);

  if (SV->fanin_empty())
    return;

  VASTBitMask NewMask(getMaskWidth());
  NewMask.KnownZeros = APInt::getAllOnesValue(getMaskWidth());
  NewMask.KnownOnes = APInt::getAllOnesValue(getMaskWidth());
  // Becareful of Non-SSA register that guarded by the enable signals that is
  // not slot registers. In this case, different SSA value from the same
  // register may be enabled by the same enable register at the different
  // slots. For example:
  // FI0 = En (Slot0 SSA) & Value (Slot0 SSA) with bitmask 0
  // FI1 = En (Slot1 SSA) & Value (Slot1 SSA) with bitmask 1
  // FI2 = En (Slot2 SSA) & Value (Slot2 SSA) with bitmask 2
  // MUXOut = FI0 | FI1 | FI2 ... Other Fanins ...
  // Here Value (Slot0 SSA) and Value (Slot1 SSA) may have imcompatible
  // bitmask. As a SSA value, we know at that monent the register should
  // have the bitmask of the bitmask of that SSA value. However, because
  // the enable signal is also not SSA, the other SSA values are also
  // enabled, and the bitmasks from other SSA value are introduced.
  // Hence we need to consider the bitmasks of all fanins fo the same
  // register.
  typedef VASTSelector::iterator iterator;
  VASTSelector *Selector = SV->getSelector();
  for (iterator I = Selector->begin(), E = Selector->end(); I != E; ++I)
    NewMask.mergeAllKnown(VASTValPtr(*I));

  mergeAnyKnown(NewMask);
}

void
VASTBitMask::printMaskVerification(raw_ostream &OS, VASTValPtr V) const {
  // There should not be 1s in the bits that are known zeros
  if (hasAnyZeroKnown()) {
    OS.indent(2) << "if ($time() > 0 && (";
    V.printAsOperand(OS);
    SmallString<128> Str;
    KnownZeros.toString(Str, 2, false, false);
    OS << " & " << getMaskWidth() << "'b" << Str << ")) begin\n";
    OS.indent(4) << "$display(\"At time %t, " << (void*)V
                 << " with unexpected ones: %b!\", $time(),";
    V.printAsOperand(OS);
    OS << ");\n";
    OS.indent(4) << "$finish(1);\n";
    OS.indent(2) << "end\n";
  }

  // There should not be 0s in the bits that are known ones
  if (hasAnyOneKnown()) {
    OS.indent(2) << "if ($time() > 0 && (~";
    V.printAsOperand(OS);
    SmallString<128> Str;
    KnownOnes.toString(Str, 2, false, false);
    OS << " & " << getMaskWidth() << "'b" << Str << ")) begin\n";
    OS.indent(4) << "$display(\"At time %t, " << (void*)V
                 << " with unexpected zeros: %b!\", $time(),";
    V.printAsOperand(OS);
    OS << ");\n";
    OS.indent(4) << "$finish(1);\n";
    OS.indent(2) << "end\n";
  }
}
