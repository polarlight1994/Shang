#include "sir/BitMaskAnalysis.h"

using namespace llvm;
using namespace vast;

char BitMaskAnalysis::ID = 0;
char &llvm::BitMaskAnalysisID = BitMaskAnalysis::ID;
INITIALIZE_PASS_BEGIN(BitMaskAnalysis, "bit-mask-analysis",
                      "Perform the bit-level analysis",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRRegisterSynthesisForCodeGen)
INITIALIZE_PASS_END(BitMaskAnalysis, "bit-mask-analysis",
                    "Perform the bit-level analysis",
                    false, true)

BitMask BitMaskAnalysis::computeAnd(BitMask LHS, BitMask RHS) {
  return BitMask(
    // If any zero in some bits of one of operands, then these bits of result will be zero
    LHS.getKnownZeros() | RHS.getKnownZeros(),
    // If any one in some bits of both two operands, then these bits of result will be one
    LHS.getKnownOnes() & RHS.getKnownOnes(),
    // Sign bits will be And,
    LHS.getKnownSames() & RHS.getKnownSames());
}

BitMask BitMaskAnalysis::computeOr(BitMask LHS, BitMask RHS) {
  return BitMask(
    // If any zero in some bits of both two operands, then these bits of result will be zero
    LHS.getKnownZeros() & RHS.getKnownZeros(),
    // If any one in some bits of one of operands, then these bits of result will be one
    LHS.getKnownOnes() | RHS.getKnownOnes(),
    // Sign bits will be And,
    LHS.getKnownSames() & RHS.getKnownSames());
}

BitMask BitMaskAnalysis::computeNot(BitMask Mask) {
  return BitMask(Mask.getKnownOnes(), Mask.getKnownZeros(), Mask.getKnownSames());
}

BitMask BitMaskAnalysis::computeXor(BitMask LHS, BitMask RHS) {
  return BitMask(
    // If some bits of both two operands are known to be same, then these bits of result will be zero
    (LHS.getKnownZeros() & RHS.getKnownZeros()) | (LHS.getKnownOnes() & RHS.getKnownOnes()),
    // If some bits of both two operands are known to be different, then these bits of result will be one
    (LHS.getKnownZeros() & RHS.getKnownOnes()) | (LHS.getKnownOnes() & RHS.getKnownZeros()),
    // Sign bits will be And,
    LHS.getKnownSames() & RHS.getKnownSames());
}

BitMask BitMaskAnalysis::computeRand(BitMask Mask) {
  return Mask.isAllOneKnown() ? BitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1)) :
                                (Mask.hasAnyZeroKnown() ? BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1)) : BitMask(APInt::getNullValue(1), APInt::getNullValue(1), APInt::getNullValue(1)));
}

BitMask BitMaskAnalysis::computeRxor(BitMask Mask) {
  if (Mask.hasAnyOneKnown() && Mask.hasAnyZeroKnown())
    return BitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
  else if (Mask.isAllOneKnown() || Mask.isAllZeroKnown())
    return BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
  else
    return BitMask(APInt::getNullValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
}

BitMask BitMaskAnalysis::computeBitCat(BitMask LHS, BitMask RHS) {
  unsigned MaskWidth = LHS.getMaskWidth() + RHS.getMaskWidth();

  APInt KnownZeros = LHS.getKnownZeros().zextOrSelf(MaskWidth).shl(RHS.getMaskWidth()) | RHS.getKnownZeros().zextOrSelf(MaskWidth);
  APInt KnownOnes = LHS.getKnownOnes().zextOrSelf(MaskWidth).shl(RHS.getMaskWidth()) | RHS.getKnownOnes().zextOrSelf(MaskWidth);

  return BitMask(KnownZeros, KnownOnes, LHS.getKnownSames().zextOrSelf(MaskWidth).shl(RHS.getMaskWidth()));;
}

BitMask BitMaskAnalysis::computeBitExtract(BitMask Mask, unsigned UB, unsigned LB) {
  return BitMask(getBitExtraction(Mask.getKnownZeros(), UB, LB),
                 getBitExtraction(Mask.getKnownOnes(), UB, LB),
                 getBitExtraction(Mask.getKnownSames(), UB, LB));
}

BitMask BitMaskAnalysis::computeBitRepeat(BitMask Mask, unsigned RepeatTimes) {
  BitMask NewMask = computeBitCat(Mask, Mask);

  for (unsigned i = 2; i < RepeatTimes; ++i) {
    NewMask = computeBitCat(NewMask, Mask);
  }

  if (!Mask.isAllBitKnown())
    NewMask = BitMask(NewMask.getKnownZeros(), NewMask.getKnownOnes(), APInt::getAllOnesValue(RepeatTimes));

  return NewMask;
}

BitMask BitMaskAnalysis::computeAdd(BitMask LHS, BitMask RHS, unsigned ResultBitWidth) {
  unsigned BitWidth = LHS.getMaskWidth();
  assert(BitWidth == RHS.getMaskWidth() && "BitWidth not matches!");

  // Initialize a empty mask.
  BitMask ResultBitMask(ResultBitWidth);

  // Calculate each bit of result mask by analyzing the adder in carry-propagate form.
  BitMask Carry
    = BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
  for (unsigned i = 0; i < ResultBitWidth; ++i) {
    if (i < BitWidth) {
      BitMask LHSBit = computeBitExtract(LHS, i + 1, i);
      BitMask RHSBit = computeBitExtract(RHS, i + 1, i);

      BitMask S = computeXor(computeXor(LHSBit, RHSBit), Carry);

      BitMask C0 = computeAnd(LHSBit, RHSBit);
      BitMask C1 = computeAnd(LHSBit, Carry);
      BitMask C2 = computeAnd(RHSBit, Carry);
      Carry = computeOr(computeOr(C0, C1), C2);

      if (S.isZeroKnownAt(0))
        ResultBitMask.setKnownZeroAt(i);
      else if (S.isOneKnownAt(0))
        ResultBitMask.setKnownOneAt(i);
    }
    else {
      if (Carry.isZeroKnownAt(0))
        ResultBitMask.setKnownZeroAt(i);
      else if (Carry.isOneKnownAt(0))
        ResultBitMask.setKnownOneAt(i);

      Carry = BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1),
                      APInt::getNullValue(1));
    }    
  }

  return ResultBitMask;
}

BitMask BitMaskAnalysis::computeMul(BitMask LHS, BitMask RHS) {
  unsigned BitWidth = LHS.getMaskWidth() + RHS.getMaskWidth();

  BitMask R(APInt::getAllOnesValue(BitWidth),
               APInt::getNullValue(BitWidth),
               APInt::getNullValue(BitWidth));

  for (unsigned i = 0; i < BitWidth; ++i) {
    // If any operand is all known zero bits or there is not any
    // known bits of R, then the iteration can be stopped.
    if (LHS.isAllZeroKnown() || RHS.isAllZeroKnown() || !R.hasAnyBitKnown())
      break;

    // If the i-th bit is known 1 in RHS, we always add the shifted LHS
    // to the result in this case.
    if (RHS.isOneKnownAt(i))
      R = computeAdd(R, BitMask(LHS.getKnownZeros().zextOrSelf(BitWidth),
                                   LHS.getKnownOnes().zextOrSelf(BitWidth),
                                   LHS.getKnownSames().zextOrSelf(BitWidth)),
                     BitWidth);
    // If the current bit is unknown, then we must make sure the known
    // zero bits of LHS is passed to the partial product.
    else if (!RHS.isZeroKnownAt(i)) {
      BitMask Mask(LHS.getKnownZeros().zextOrSelf(BitWidth), APInt::getNullValue(BitWidth), LHS.getKnownSames().zextOrSelf(BitWidth));
      R = computeAdd(R, Mask, BitWidth);
    }

    // Shift the LHS and prepare for next add
    LHS = LHS.shl(1);

    // Clear the bit after compute so when all bits are zero means the
    // end of this shift & add process.
    RHS.setKnownZeroAt(i);
  }

  return R;
}

BitMask BitMaskAnalysis::computeShl(BitMask LHS, BitMask RHS) {
  unsigned RHSMaxWidth = std::min(Log2_32_Ceil(LHS.getMaskWidth()),
                                  RHS.getMaskWidth());

  BitMask R = LHS;
  for (unsigned i = 0; i < RHSMaxWidth; ++i) {
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

BitMask BitMaskAnalysis::computeLshr(BitMask LHS, BitMask RHS) {
  BitMask R = LHS;
  for (unsigned i = 0; i < RHS.getMaskWidth(); ++i) {
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

BitMask BitMaskAnalysis::computeAshr(BitMask LHS, BitMask RHS) {
  unsigned RHSMaxWidth = std::min(Log2_32_Ceil(LHS.getMaskWidth()),
    RHS.getMaskWidth());

  BitMask R = LHS;
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

BitMask BitMaskAnalysis::computeUgt(BitMask LHS, BitMask RHS) {
  unsigned BitWidth = LHS.getMaskWidth();
  assert(BitWidth == RHS.getMaskWidth() && "Mask width not matches!");

  for (unsigned i = 0; i < BitWidth; ++i) {
    if ((LHS.isZeroKnownAt(BitWidth - 1 - i) && RHS.isZeroKnownAt(BitWidth - 1 - i)) ||
      (LHS.isOneKnownAt(BitWidth - 1 - i) && RHS.isOneKnownAt(BitWidth - 1 - i)))
      continue;
    else if (LHS.isOneKnownAt(BitWidth - 1 - i) && RHS.isZeroKnownAt(BitWidth - 1 - i))
      return BitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
    else if (LHS.isZeroKnownAt(BitWidth - 1 - i) && RHS.isOneKnownAt(BitWidth - 1 - i))
      return BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
    else
      return BitMask(1);
  }
}

BitMask BitMaskAnalysis::computeSgt(BitMask LHS, BitMask RHS) {
  unsigned BitWidth = LHS.getMaskWidth();
  assert(BitWidth == RHS.getMaskWidth() && "Mask width not matches!");

  if (LHS.isZeroKnownAt(BitWidth - 1) && RHS.isOneKnownAt(BitWidth - 1))
    return BitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
  else if (LHS.isOneKnownAt(BitWidth - 1) && RHS.isZeroKnownAt(BitWidth - 1))
    return BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
  else if (LHS.isZeroKnownAt(BitWidth - 1) && RHS.isZeroKnownAt(BitWidth - 1)) {
    for (unsigned i = 0; i < BitWidth - 1; ++i) {
      if ((LHS.isZeroKnownAt(BitWidth - 2 - i) && RHS.isZeroKnownAt(BitWidth - 2 - i)) ||
        (LHS.isOneKnownAt(BitWidth - 2 - i) && RHS.isOneKnownAt(BitWidth - 2 - i)))
        continue;
      else if (LHS.isOneKnownAt(BitWidth - 2 - i) && RHS.isZeroKnownAt(BitWidth - 2 - i))
        return BitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
      else if (LHS.isZeroKnownAt(BitWidth - 2 - i) && RHS.isOneKnownAt(BitWidth - 2 - i))
        return BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
      else
        return BitMask(1);
    }
  } else if (LHS.isOneKnownAt(BitWidth - 1) && RHS.isOneKnownAt(BitWidth - 1)) {
    for (unsigned i = 0; i < BitWidth - 1; ++i) {
      if ((LHS.isZeroKnownAt(BitWidth - 2 - i) && RHS.isZeroKnownAt(BitWidth - 2 - i)) ||
        (LHS.isOneKnownAt(BitWidth - 2 - i) && RHS.isOneKnownAt(BitWidth - 2 - i)))
        continue;
      else if (LHS.isOneKnownAt(BitWidth - 2 - i) && RHS.isZeroKnownAt(BitWidth - 2 - i))
        return BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));       
      else if (LHS.isZeroKnownAt(BitWidth - 2 - i) && RHS.isOneKnownAt(BitWidth - 2 - i))
        return BitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
      else
        return BitMask(1);
    }
  } else
    return BitMask(1);  
}

BitMask BitMaskAnalysis::computeUDiv(BitMask LHS, BitMask RHS) {
  return BitMask(LHS.getMaskWidth());
}

BitMask BitMaskAnalysis::computeSDiv(BitMask LHS, BitMask RHS) {
  return BitMask(LHS.getMaskWidth());
}

BitMask BitMaskAnalysis::computeMask(Instruction *Inst, SIR *SM, DataLayout *TD) {
  // Handle the special non-intrinsic instruction in SIR.
  if (isa<PtrToIntInst>(Inst) || isa<IntToPtrInst>(Inst) || isa<BitCastInst>(Inst)) {
    Value *Operand = Inst->getOperand(0);

    unsigned BitWidth = TD->getTypeSizeInBits(Operand->getType());

    // If the mask of operand has not been computed yet.
    if (!SM->hasBitMask(Operand)) {
      assert(isa<GlobalValue>(Operand) || (SM->lookupSIRReg(Operand) != NULL) ||
             isa<Argument>(Operand) || isa<UndefValue>(Operand) && "Unexpected value type!");

      BitMask Mask(BitWidth);

      SM->IndexVal2BitMask(Operand, Mask);
      SM->IndexVal2BitMask(Inst, Mask);
      return Mask;
    }

    BitMask OperandMask = SM->getBitMask(Operand);
    SM->IndexVal2BitMask(Inst, OperandMask);

    return OperandMask;
  }

  assert(isa<IntrinsicInst>(Inst) && "Unexpected instruction type!");

  SmallVector<BitMask, 4> Masks;
  for (unsigned i = 0; i < Inst->getNumOperands() - 1; ++i) {
    Value *Op = Inst->getOperand(i);
    unsigned BitWidth = TD->getTypeSizeInBits(Op->getType());

    // The ports of module will not have mask.
    if (!SM->hasBitMask(Op)) {
      if (Instruction *Inst = dyn_cast<Instruction>(Op)) {
        BitMask Mask(BitWidth);

        Masks.push_back(Mask);
        continue;
      }

      if (SIRRegister *Reg = SM->lookupSIRReg(Op)) {
        Masks.push_back(BitMask(BitWidth));
        continue;
      }

      assert(isa<Argument>(Op) || isa<ConstantInt>(Op) || isa<UndefValue>(Op) && "Unexpected Value Type!");

      if (ConstantInt *CI = dyn_cast<ConstantInt>(Op)) {
        APInt CIAPInt = CI->getValue();
        unsigned LeadingZeros = CIAPInt.countLeadingZeros();
        unsigned LeadingOnes = CIAPInt.countLeadingOnes();

        unsigned SignBits = std::max(LeadingZeros, LeadingOnes);
        assert(SignBits > 0 && "Unexpected result!");

        if (SignBits == 1) {
          BitMask Mask = BitMask(~CIAPInt, CIAPInt, CIAPInt.getNullValue(CIAPInt.getBitWidth()));
          SM->IndexVal2BitMask(Op, Mask);

          Masks.push_back(Mask);
        }
        else {
          APInt KnownSignBits = APInt::getAllOnesValue(CIAPInt.getBitWidth());
          KnownSignBits = KnownSignBits.lshr(CIAPInt.getBitWidth() - SignBits);
          KnownSignBits = KnownSignBits.shl(CIAPInt.getBitWidth() - SignBits);

          BitMask Mask = BitMask(~CIAPInt, CIAPInt, KnownSignBits);
          SM->IndexVal2BitMask(Op, Mask);

          Masks.push_back(BitMask(~CIAPInt, CIAPInt,KnownSignBits));
        }

        continue;
      }

      Masks.push_back(BitMask(BitWidth));
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

    return computeBitRepeat(Masks[0], RepeatTimes);
  }
  case Intrinsic::shang_bit_extract: {
    assert(Masks.size() == 3 && "Unexpected numbers of operands!");

    Value *UBVal = II->getOperand(1), *LBVal = II->getOperand(2);
    ConstantInt *UBCI = dyn_cast<ConstantInt>(UBVal), *LBCI = dyn_cast<ConstantInt>(LBVal);
    unsigned UB = UBCI->getValue().getZExtValue(), LB = LBCI->getValue().getZExtValue();

    return computeBitExtract(Masks[0], UB, LB);
  }
  case Intrinsic::shang_bit_cat: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeBitCat(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_and: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeAnd(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_or: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeOr(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_not: {
    assert(Masks.size() == 1 && "Unexpected numbers of operands!");

    return computeNot(Masks[0]);
  }
  case Intrinsic::shang_xor: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeXor(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_rand: {
    assert(Masks.size() == 1 && "Unexpected numbers of operands!");

    return computeRand(Masks[0]);
  }
  case Intrinsic::shang_rxor: {
    assert(Masks.size() == 1 && "Unexpected numbers of operands!");

    return computeRxor(Masks[0]);
  }
  case Intrinsic::shang_addc: {
    assert(Masks.size() == 3 && "Unexpected numbers of operands!");

    unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());

    BitMask UpdateMask = computeAdd(Masks[0], Masks[1], BitWidth);
    UpdateMask = computeAdd(UpdateMask, Masks[2].extend(BitWidth), BitWidth);

    if (BitWidth < UpdateMask.getMaskWidth())
      UpdateMask = computeBitExtract(UpdateMask, BitWidth, 0);

    return UpdateMask;
  }
  case Intrinsic::shang_add: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());

    BitMask UpdateMask = computeAdd(Masks[0], Masks[1], BitWidth);

    if (BitWidth < UpdateMask.getMaskWidth())
      UpdateMask = computeBitExtract(UpdateMask, BitWidth, 0);

    return UpdateMask;
  }
  case Intrinsic::shang_mul: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());

    BitMask UpdateMask = computeMul(Masks[0], Masks[1]);

    if (BitWidth < UpdateMask.getMaskWidth())
      UpdateMask = computeBitExtract(UpdateMask, BitWidth, 0);
    return UpdateMask;
  }
  case Intrinsic::shang_shl: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeShl(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_lshr: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeLshr(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_ashr: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeAshr(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_ugt: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeUgt(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_sgt: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeSgt(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_eq: {
    bool UnEqual = false;
    bool UnKnown = false;

    unsigned BitWidth = Masks[0].getMaskWidth();
    assert(BitWidth == Masks[1].getMaskWidth() && "Unexpected bitwidth!");

    for (unsigned i = 0; i < BitWidth; ++i) {
      if (Masks[0].isOneKnownAt(i) && Masks[1].isZeroKnownAt(i)) {
        UnEqual = true;
        break;
      }

      if (Masks[0].isZeroKnownAt(i) && Masks[1].isOneKnownAt(i)) {
        UnEqual = true;
        break;
      }

      if (!Masks[0].isBitKnownAt(i) || !Masks[0].isBitKnownAt(i))
        UnKnown = true;

    }

    if (UnEqual)
      return BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
    else if (!UnEqual && UnKnown)
      return BitMask(1);
    else
      return BitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
  }
  case Intrinsic::shang_ne: {
    bool UnEqual = false;
    bool UnKnown = false;

    unsigned BitWidth = Masks[0].getMaskWidth();
    assert(BitWidth == Masks[1].getMaskWidth() && "Unexpected bitwidth!");

    for (unsigned i = 0; i < BitWidth; ++i) {
      if (Masks[0].isOneKnownAt(i) && Masks[1].isZeroKnownAt(i)) {
        UnEqual = true;
        break;
      }

      if (Masks[0].isZeroKnownAt(i) && Masks[1].isOneKnownAt(i)) {
        UnEqual = true;
        break;
      }

      if (!Masks[0].isBitKnownAt(i) || !Masks[0].isBitKnownAt(i))
        UnKnown = true;

    }

    if (UnEqual)
      return BitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
    else if (!UnEqual && UnKnown)
      return BitMask(1);
    else
      return BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
  }
  case Intrinsic::shang_udiv: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeUDiv(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_sdiv: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    return computeSDiv(Masks[0], Masks[1]);
  }
  case Intrinsic::shang_reg_assign: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    //SIRRegister *Reg = SM->lookupSIRReg(Inst);
    //if (Reg->isFUInOut()) {
    //  return BitMask(Masks[0].getMaskWidth());
    //}

    //return BitMask(Masks[0].getKnownZeros(),
    //                  APInt::getNullValue(Masks[0].getMaskWidth()),
    //                  APInt::getNullValue(Masks[0].getMaskWidth()));

    return BitMask(Masks[0].getMaskWidth());
  }
  }
}

bool isDifferentMask(BitMask NewMask, BitMask OldMask) {
  return ((NewMask.getKnownOnes() != OldMask.getKnownOnes()) ||
          (NewMask.getKnownZeros() != OldMask.getKnownZeros()) ||
          (NewMask.getKnownSames() != OldMask.getKnownSames()));
}

bool BitMaskAnalysis::computeAndUpdateMask(Instruction *Inst) {
  unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());

  BitMask OldMask(BitWidth);
  if (SM->hasBitMask(Inst))
    OldMask = SM->getBitMask(Inst);

  BitMask NewMask = computeMask(Inst, SM, TD);

  if (isDifferentMask(NewMask, OldMask)) {
    NewMask.mergeKnownByOr(OldMask);

    SM->IndexVal2BitMask(Inst, NewMask);
    return true;
  }

  SM->IndexVal2BitMask(Inst, NewMask);
  return false;
}

bool BitMaskAnalysis::traverseFromRoot(Value *Val) {
  Instruction *Inst = dyn_cast<Instruction>(Val);
  assert(Inst && "Unexpected value type!");

  // Avoid visiting same instruction twice.
  if (Visited.count(Inst))
    return false;

  bool Changed = false;

  std::set<Instruction *> LocalVisited;

  typedef Instruction::op_iterator op_iterator;
  std::vector<std::pair<Instruction *, op_iterator> > VisitStack;

  VisitStack.push_back(std::make_pair(Inst, Inst->op_begin()));

  while(!VisitStack.empty()) {
    Instruction *CurNode = VisitStack.back().first;
    op_iterator &I = VisitStack.back().second;

    // All children of current node have been visited.
    if (I == CurNode->op_end()) {
      VisitStack.pop_back();

      Changed |= computeAndUpdateMask(CurNode);
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    Value *ChildVal = *I;
    ++I;
    Instruction *ChildInst = dyn_cast<Instruction>(ChildVal);

    // TODO: the mask of constant value may be useful.
    // Ignore the non-instruction value.
    if (!ChildInst)
      continue;

    // Ignore the register.
    if (IntrinsicInst *ChildII = dyn_cast<IntrinsicInst>(ChildInst))
      if (ChildII->getIntrinsicID() == Intrinsic::shang_reg_assign)
        continue;

    // No need to visit the same node twice.
    if (!LocalVisited.insert(ChildInst).second || Visited.count(ChildInst))
      continue;

    VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
  }

  return Changed;
}

bool BitMaskAnalysis::traverseDatapath() {
  bool Changed = false;

  typedef SIR::register_iterator iterator;
  for (iterator I = SM->registers_begin(), E = SM->registers_end(); I != E; ++I) {
    SIRRegister *Reg = I;

    Changed |= traverseFromRoot(Reg->getLLVMValue());
  }

  return Changed;
}

void BitMaskAnalysis::printMask(raw_fd_ostream &Output) {
  // Visit the basic block in topological order.
  Function *F = SM->getFunction();

  unsigned MaskNum = 0;
  unsigned Num = 0;

  SIRDatapathBuilder Builder(SM, *TD);

  typedef Function::iterator bb_iterator;
  typedef BasicBlock::iterator inst_iterator;
  for (bb_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    BasicBlock *BB = BI;

    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;

      if (!SM->hasBitMask(Inst)) {
        continue;
      }
      BitMask Mask = SM->getBitMask(Inst);

      Value *KnownZeros = Builder.createIntegerValue(Mask.getKnownZeros());
      Value *KnownOnes = Builder.createIntegerValue(Mask.getKnownOnes());         
 
      Output << Inst->getName() << "\t";
      Mask.print(Output);
      Output << "\n";
    }
  }

  Output << "Total useful mask numbers is " << MaskNum;
  Output << "\n\n\n";
}

void BitMaskAnalysis::verifyMaskCorrectness() {
  Function *F = SM->getFunction();
  SIRDatapathBuilder Builder(SM, *TD);

  std::set<Value *> MaskedVals;

  typedef Function::iterator bb_iterator;
  typedef BasicBlock::iterator inst_iterator;
  for (bb_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    BasicBlock *BB = BI;

    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;

      if (!SM->hasBitMask(Inst)) {
        continue;
      }

      if (MaskedVals.count(Inst))
        continue;

      BitMask Mask = SM->getBitMask(Inst);

      if (!Mask.hasAnyBitKnown())
        continue;

      Value *KnownZeros = Builder.createIntegerValue(Mask.getKnownZeros());
      Value *KnownOnes = Builder.createIntegerValue(Mask.getKnownOnes());

      unsigned BitWidth = Mask.getMaskWidth();
      Value *MaskedVal = Builder.createSOrInst(Builder.createIntegerValue(BitWidth, 1), Builder.createIntegerValue(BitWidth, 1), Inst->getType(), Inst, true);

      MaskedVals.insert(MaskedVal);
      SM->IndexVal2BitMask(MaskedVal, Mask);

      if (MaskedVal->getType() != Inst->getType()) {
        Type *OriginType = Inst->getType();
        assert(OriginType->isPointerTy() && "Unexpected Type!");

        Value *MaskedPointerVal = Builder.createIntToPtrInst(MaskedVal, OriginType, Inst, false);
      } else {
        Inst->replaceAllUsesWith(MaskedVal);
      }

      Instruction *MaskedInst = dyn_cast<Instruction>(MaskedVal);
      MaskedInst->setOperand(0, Builder.createSAndInst(Inst, Builder.createSNotInst(KnownZeros, KnownZeros->getType(), Inst, true), Inst->getType(), Inst, true));
      MaskedInst->setOperand(1, KnownOnes);
    }
  }
}

bool BitMaskAnalysis::runIteration() {
  bool Changed = false;

  Changed |= traverseDatapath();

  return Changed;
}

bool BitMaskAnalysis::runOnSIR(SIR &SM) {
  this->TD = &getAnalysis<DataLayout>();
  this->SM = &SM;

  // Get the output path for Verilog code.
  std::string MaskOutputPath = LuaI::GetString("MaskOutput");
  std::string Error;
  raw_fd_ostream Output(MaskOutputPath.c_str(), Error);

  unsigned num_iterations = 0;

  while(runIteration());

  verifyMaskCorrectness();
  printMask(Output);

  return false;
}