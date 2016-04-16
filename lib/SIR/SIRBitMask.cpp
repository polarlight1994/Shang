#include "sir/SIR.h"

using namespace llvm;

SIRBitMask SIRBitMask::evaluateAnd(SIRBitMask LHS, SIRBitMask RHS) {
  return SIRBitMask(
    // If any zero in some bits of one of operands, then these bits of result will be zero
    LHS.KnownZeros | RHS.KnownZeros,
    // If any one in some bits of both two operands, then these bits of result will be one
    LHS.KnownOnes & RHS.KnownOnes);
}

SIRBitMask SIRBitMask::evaluateOr(SIRBitMask LHS, SIRBitMask RHS) {
  return SIRBitMask(
    // If any zero in some bits of both two operands, then these bits of result will be zero
    LHS.KnownZeros & RHS.KnownZeros,
    // If any one in some bits of one of operands, then these bits of result will be one
    LHS.KnownOnes | RHS.KnownOnes);
}

SIRBitMask SIRBitMask::evaluateNot(SIRBitMask Mask) {
  return SIRBitMask(KnownOnes, KnownZeros);
}

SIRBitMask SIRBitMask::evaluateXor(SIRBitMask LHS, SIRBitMask RHS) {
  return SIRBitMask(
    // If some bits of both two operands are known to be same, then these bits of result will be zero
    (LHS.KnownZeros & RHS.KnownZeros) | (LHS.KnownOnes & RHS.KnownOnes),
    // If some bits of both two operands are known to be different, then these bits of result will be one
    (LHS.KnownZeros & RHS.KnownOnes) | (LHS.KnownOnes & RHS.KnownZeros));
}

SIRBitMask SIRBitMask::evaluateRand(SIRBitMask Mask) {
  return Mask.isAllOneKnown() ? SIRBitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1)) :
                                (Mask.hasAnyZeroKnown() ? SIRBitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1)) : 
                                                          SIRBitMask(APInt::getNullValue(1), APInt::getNullValue(1)));
}

SIRBitMask SIRBitMask::evaluateRxor(SIRBitMask Mask) {
 if (Mask.hasAnyOneKnown() && Mask.hasAnyZeroKnown())
   return SIRBitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1));
 else if (Mask.isAllOneKnown() || Mask.isAllZeroKnown())
   return SIRBitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1));
 else
   return SIRBitMask(APInt::getNullValue(1), APInt::getNullValue(1));
}

SIRBitMask SIRBitMask::evaluateBitCat(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned MaskWidth = LHS.getMaskWidth() + RHS.getMaskWidth();
  SIRBitMask Mask(MaskWidth);

  Mask.KnownZeros = LHS.KnownZeros.zextOrSelf(MaskWidth).shl(RHS.getMaskWidth()) | RHS.KnownZeros.zextOrSelf(MaskWidth);
  Mask.KnownOnes = LHS.KnownOnes.zextOrSelf(MaskWidth).shl(RHS.getMaskWidth()) | RHS.KnownOnes.zextOrSelf(MaskWidth);

  return Mask;
}

SIRBitMask SIRBitMask::evaluateBitExtract(SIRBitMask Mask, unsigned UB, unsigned LB) {
  return SIRBitMask(getBitExtraction(Mask.KnownZeros, UB, LB),
                    getBitExtraction(Mask.KnownOnes, UB, LB));
}

SIRBitMask SIRBitMask::evaluateBitRepeat(SIRBitMask Mask, unsigned RepeatTimes) {
  SIRBitMask NewMask = evaluateBitCat(Mask, Mask);

  for (unsigned i = 2; i < RepeatTimes; ++i) {
    NewMask = evaluateBitCat(NewMask, Mask);
  }

  return NewMask;
}

SIRBitMask SIRBitMask::evaluateAdd(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned BitWidth = LHS.getMaskWidth();
  assert(BitWidth == RHS.getMaskWidth() && "BitWidth not matches!");

  // Without consideration of cin, the known bits of sum will be
  // determined by s = a ^ b;
  SIRBitMask S = evaluateXor(LHS, RHS);

  // Without consideration of cin, the known bits of sum will be
  // determined by c = a & b;
  SIRBitMask C = evaluateAnd(LHS, RHS);

  for (unsigned i = 0; i < BitWidth; ++i) {
    // If there is not any known bits of S, then we will get nothing.
    // Also, if there is all known zero bits of C, then the result
    // will always be same with S.
    if (!S.hasAnyBitKnown() || C.isAllZeroKnown())
      break;

    // Shift the C since the cout of this bit will be the cin
    // of the next bit.
    SIRBitMask ShiftedC = C.shl(1);

    // Calculate the mask bit by bit considering the cin.
    S = evaluateXor(S, ShiftedC);
    C = evaluateAnd(S, ShiftedC);
  }

  return S;
}

SIRBitMask SIRBitMask::evaluateAddc(SIRBitMask LHS, SIRBitMask RHS, SIRBitMask Carry) {
  unsigned BitWidth = LHS.getMaskWidth();
  assert(BitWidth == RHS.getMaskWidth() && "BitWidth not matches!");
  assert(Carry.getMaskWidth() == 1 && "Unexpected Carry BitWidth!");

  // Without consideration of cin, the known bits of sum will be
  // determined by s = a ^ b;
  SIRBitMask S = evaluateXor(LHS, RHS);

  // Without consideration of cin, the known bits of sum will be
  // determined by c = a & b; To be noted that, the cin will
  // catenate with the result.
  SIRBitMask C = evaluateAnd(LHS, RHS);

  for (unsigned i = 0; i < BitWidth; ++i) {
    // If there is not any known bits of S, then we will get nothing.
    // Also, if there is all known zero bits of C, then the result
    // will always be same with S.
    if (!S.hasAnyBitKnown() || (C.isAllZeroKnown() && Carry.isAllZeroKnown()))
      break;

    // Shift the C since the cout of this bit will be the cin
    // of the next bit.
    SIRBitMask ShiftedC = C.shl(1);

    // Consider the Cin.
    if (i == 0)
      ShiftedC = evaluateAnd(ShiftedC, Carry.extend(BitWidth));

    // Calculate the mask bit by bit considering the cin.
    S = evaluateXor(S, ShiftedC);
    C = evaluateAnd(S, ShiftedC);
  }

  return S;
}

SIRBitMask SIRBitMask::evaluateMul(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned BitWidth = LHS.getMaskWidth() + RHS.getMaskWidth();

  SIRBitMask R(APInt::getAllOnesValue(BitWidth),
               APInt::getNullValue(BitWidth));

  for (unsigned i = 0; i < BitWidth; ++i) {
    // If any operand is all known zero bits or there is not any
    // known bits of R, then the iteration can be stopped.
    if (LHS.isAllZeroKnown() || RHS.isAllZeroKnown() || !R.hasAnyBitKnown())
      break;

    // If the i-th bit is known 1 in RHS, we always add the shifted LHS
    // to the result in this case.
    if (RHS.isOneKnownAt(i))
      R = evaluateAdd(R, SIRBitMask(LHS.getKnownZeros().zextOrSelf(BitWidth),
                                    LHS.getKnownOnes().zextOrSelf(BitWidth)));
    // If the current bit is unknown, then we must make sure the known
    // zero bits of LHS is passed to the partial product.
    else if (!RHS.isZeroKnownAt(i)) {
      SIRBitMask Mask(LHS.getKnownZeros().zextOrSelf(BitWidth), APInt::getNullValue(BitWidth));
      R = evaluateAdd(R, Mask);
    }

    // Shift the LHS and prepare for next add
    LHS = LHS.shl(1);

    // Clear the bit after evaluate so when all bits are zero means the
    // end of this shift & add process.
    RHS.setKnownZeroAt(i);
  }

  return R;
}

SIRBitMask SIRBitMask::evaluateShl(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned RHSMaxWidth = std::min(Log2_32_Ceil(LHS.getMaskWidth()),
                                  RHS.getMaskWidth());

  SIRBitMask R = LHS;
  for (unsigned i = 0; i < RHSMaxWidth && R.hasAnyBitKnown(); ++i) {
    // If the i-th bit is known one in RHS, we always add the shifted
    // LHS to the result in this case.
    if (RHS.isOneKnownAt(i))
      R = R.shl(1 << i);
    // Otherwise if the bit at RHS is unknown, then the result bits
    // are known only if the LHS bit is known no matter it is shifted
    // or not.
    else if (!RHS.isZeroKnownAt(i))
      R.mergeKnownByAnd(R.shl(1 << i));
  }

  return R;
}

SIRBitMask SIRBitMask::evaluateLshr(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned RHSMaxWidth = std::min(Log2_32_Ceil(LHS.getMaskWidth()),
                                  RHS.getMaskWidth());

  SIRBitMask R = LHS;
  for (unsigned i = 0; i < RHSMaxWidth && R.hasAnyBitKnown(); ++i) {
    // If the i-th bit is known one in RHS, we always add the shifted
    // LHS to the result in this case.
    if (RHS.isOneKnownAt(i))
      R = R.lshr(1 << i);
    // Otherwise if the bit at RHS is unknown, then the result bits
    // are known only if the LHS bit is known no matter it is shifted
    // or not.
    else if (!RHS.isZeroKnownAt(i))
      R.mergeKnownByAnd(R.lshr(1 << i));
  }

  return R;
}

SIRBitMask SIRBitMask::evaluateAshr(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned RHSMaxWidth = std::min(Log2_32_Ceil(LHS.getMaskWidth()),
                                  RHS.getMaskWidth());

  SIRBitMask R = LHS;
  for (unsigned i = 0; i < RHSMaxWidth && R.hasAnyBitKnown(); ++i) {
    // If the i-th bit is known one in RHS, we always add the shifted
    // LHS to the result in this case.
    if (RHS.isOneKnownAt(i))
      R = R.ashr(1 << i);
    // Otherwise if the bit at RHS is unknown, then the result bits
    // are known only if the LHS bit is known no matter it is shifted
    // or not.
    else if (!RHS.isZeroKnownAt(i))
      R.mergeKnownByAnd(R.ashr(1 << i));
  }

  return R;
}

SIRBitMask SIRBitMask::evaluateUgt(SIRBitMask LHS, SIRBitMask RHS) {
  return SIRBitMask(1);
}

SIRBitMask SIRBitMask::evaluateSgt(SIRBitMask LHS, SIRBitMask RHS) {
  return SIRBitMask(1);
}

void SIRBitMask::evaluateMask(Instruction *Inst, SIR *SM, DataLayout *TD) {
  assert(isa<IntrinsicInst>(Inst) && "Unexpected instruction type!");

  SmallVector<SIRBitMask, 4> Masks;
  for (unsigned i = 0; i < Inst->getNumOperands() - 1; ++i) {
    Value *Op = Inst->getOperand(i);

    // The ports of module will not have mask.
    if (!SM->hasBitMask(Op)) {
      assert(isa<Argument>(Op) || isa<ConstantInt>(Op) && "Unexpected Value Type!");

      if (ConstantInt *CI = dyn_cast<ConstantInt>(Op)) {
        Masks.push_back(SIRBitMask(~(CI->getValue()), CI->getValue()));
        continue;
      }

      unsigned BitWidth = TD->getTypeSizeInBits(Op->getType());
      Masks.push_back(SIRBitMask(BitWidth));
      continue;
    }

    Masks.push_back(SM->getBitMask(Op));
  }

  IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst);
  assert(II && "Unexpected instruction type!");

  switch (II->getIntrinsicID()) {
  default:  assert(false && "Unexpected intrinsic instruction type!");
  case Intrinsic::shang_bit_repeat: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    Value *RepeatTimesVal = II->getOperand(1);
    ConstantInt *RepeatTimesCI = dyn_cast<ConstantInt>(RepeatTimesVal);
    unsigned RepeatTimes = RepeatTimesCI->getValue().getZExtValue();

    return mergeKnownByOr(evaluateBitRepeat(Masks[0], RepeatTimes));
  }
  case Intrinsic::shang_bit_extract: {
    assert(Masks.size() == 3 && "Unexpected numbers of operands!");

    Value *UBVal = II->getOperand(1), *LBVal = II->getOperand(2);
    ConstantInt *UBCI = dyn_cast<ConstantInt>(UBVal), *LBCI = dyn_cast<ConstantInt>(LBVal);
    unsigned UB = UBCI->getValue().getZExtValue(), LB = LBCI->getValue().getZExtValue();

    return mergeKnownByOr(evaluateBitExtract(Masks[0], UB, LB));
  }
  case Intrinsic::shang_bit_cat: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateBitCat(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_and: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateAnd(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_or: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateOr(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_not: {
    assert(Masks.size() == 1 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateNot(Masks[0]));
  }
  case Intrinsic::shang_xor: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateXor(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_rand: {
    assert(Masks.size() == 1 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateRand(Masks[0]));
  }
  case Intrinsic::shang_rxor: {
    assert(Masks.size() == 1 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateRxor(Masks[0]));
  }
  case Intrinsic::shang_addc: {
    assert(Masks.size() == 3 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateAddc(Masks[0], Masks[1], Masks[2]));
  }
  case Intrinsic::shang_add: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateAdd(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_mul: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateMul(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_shl: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateShl(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_lshr: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateLshr(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_ashr: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateAshr(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_ugt: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateUgt(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_sgt: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(evaluateSgt(Masks[0], Masks[1]));
  }
  case Intrinsic::shang_reg_assign: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return mergeKnownByOr(Masks[0]);
  }
  }
}

void SIRBitMask::print(raw_ostream &Output) {
  Output << "[";
  for (unsigned i = 0; i < KnownZeros.getBitWidth(); ++i) {
    int Bit = KnownZeros[i];
    Output << Bit;
  }

  Output << ", ";

  for (unsigned i = 0; i < KnownOnes.getBitWidth(); ++i) {
    int Bit = KnownOnes[i];
    Output << Bit;
  }

  Output << "]";
}