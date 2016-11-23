#include "sir/BitMaskAnalysis.h"

using namespace llvm;
using namespace vast;

char BitMaskAnalysis::ID = 0;
char &llvm::BitMaskAnalysisID = BitMaskAnalysis::ID;
INITIALIZE_PASS_BEGIN(BitMaskAnalysis, "bit-mask-analysis",
                      "Perform the bit-level analysis",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(DFGBuild)
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
  assert(Mask.getMaskWidth() == 1 && "Unexpected width!");

  BitMask NewMask = computeBitCat(Mask, Mask);

  for (unsigned i = 2; i < RepeatTimes; ++i) {
    NewMask = computeBitCat(NewMask, Mask);
  }

  // Remember to set the Known-as-Sign-bit.
  for (unsigned i = 0; i < RepeatTimes; ++i) {
    NewMask.setKnownSignAt(i);
  }

  return NewMask;
}

BitMask BitMaskAnalysis::computeAdd(BitMask LHS, BitMask RHS, unsigned ResultBitWidth) {
  unsigned BitWidth = std::max(LHS.getMaskWidth(), RHS.getMaskWidth());
  if (LHS.getMaskWidth() != BitWidth) {
    assert(LHS.getMaskWidth() == 1 && "Unexpected width!");

    LHS.extend(BitWidth);
  }
  if (RHS.getMaskWidth() != BitWidth) {
    assert(RHS.getMaskWidth() == 1 && "Unexpected width!");

    RHS.extend(BitWidth);
  }

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

BitMask BitMaskAnalysis::computeMask(DFGNode *Node) {
  // Calculate the mask according to the node type.
  DFGNode::NodeType NodeTy = Node->getType();
  unsigned BitWidth = Node->getBitWidth();

  // Handle the special node types.
  if (NodeTy == DFGNode::TypeConversion) {
    assert(Node->parent_size() == 1 && "Unexpected parent size!");
    DFGNode *ParentNode = *(Node->parent_begin());

    BitMask ParentMask = getOrCreateMask(ParentNode);

    return ParentMask;
  }
  else if (NodeTy == DFGNode::Argument || NodeTy == DFGNode::UndefVal ||
           NodeTy == DFGNode::GlobalVal) {
    BitMask Mask = BitMask(BitWidth);

    return Mask;
  }
  else if (NodeTy == DFGNode::ConstantInt) {
    ConstantIntDFGNode *CINode = dyn_cast<ConstantIntDFGNode>(Node);
    assert(CINode && "Unexpected node type!");

    uint64_t Val = CINode->getIntValue();
    APInt CIAPInt = APInt(BitWidth, Val);
    unsigned LeadingZeros = CIAPInt.countLeadingZeros();
    unsigned LeadingOnes = CIAPInt.countLeadingOnes();

    unsigned SignBits = std::max(LeadingZeros, LeadingOnes);
    assert(SignBits > 0 && "Unexpected result!");

    if (SignBits == 1) {
      BitMask Mask = BitMask(~CIAPInt, CIAPInt, CIAPInt.getNullValue(CIAPInt.getBitWidth()));

      return Mask;
    }
    else {
      APInt KnownSignBits = APInt::getAllOnesValue(CIAPInt.getBitWidth());
      KnownSignBits = KnownSignBits.lshr(CIAPInt.getBitWidth() - SignBits);
      KnownSignBits = KnownSignBits.shl(CIAPInt.getBitWidth() - SignBits);

      BitMask Mask = BitMask(~CIAPInt, CIAPInt, KnownSignBits);

      return Mask;
    }
  }
  else {
    // First collect the mask of parent nodes.
    std::vector<BitMask> ParentMasks;
    if (CommutativeDFGNode *CNode = dyn_cast<CommutativeDFGNode>(Node)) {
      typedef DFGNode::iterator iterator;
      for (iterator I = Node->parent_begin(), E = Node->parent_end(); I != E; ++I) {
        DFGNode *ParentNode = *I;
        unsigned ParentNodeNum = CNode->getNumOfParentNode(ParentNode);

        // Get the mask.
        BitMask ParentMask = getOrCreateMask(ParentNode);
        for (unsigned i = 0; i < ParentNodeNum; ++i)
          ParentMasks.push_back(ParentMask);
      }
    }
    else {
      NonCommutativeDFGNode *NCNode = dyn_cast<NonCommutativeDFGNode>(Node);
      assert(NCNode && "Unexpected node type!");

      unsigned ParentSize = NCNode->parent_size();
      for (unsigned i = 0; i < ParentSize; ++i) {
        DFGNode *ParentNode = NCNode->getParentNode(i);

        // Get the mask.
        BitMask ParentMask = getOrCreateMask(ParentNode);
        ParentMasks.push_back(ParentMask);
      }
    }

    switch (NodeTy) {
    case llvm::DFGNode::Add: {
      BitMask Mask;
      if (ParentMasks.size() == 2)
        Mask = computeAdd(ParentMasks[0], ParentMasks[1], BitWidth);
      else {
        Mask = computeAdd(computeAdd(ParentMasks[0], ParentMasks[1], BitWidth),
                          ParentMasks[2], BitWidth);
      }

      return Mask;
    }
    case llvm::DFGNode::Mul: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      BitMask UpdateMask = computeMul(ParentMasks[0], ParentMasks[1]);

      if (BitWidth < UpdateMask.getMaskWidth())
        UpdateMask = computeBitExtract(UpdateMask, BitWidth, 0);

      return UpdateMask;
    }
    case llvm::DFGNode::Div: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      BitMask Mask = computeUDiv(ParentMasks[0], ParentMasks[1]);

      return Mask;
    }
    case llvm::DFGNode::LShr: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      BitMask Mask = computeLshr(ParentMasks[0], ParentMasks[1]);

      return Mask;
    }
    case llvm::DFGNode::AShr: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      BitMask Mask = computeAshr(ParentMasks[0], ParentMasks[1]);

      return Mask;
    }
    case llvm::DFGNode::Shl: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      BitMask Mask = computeShl(ParentMasks[0], ParentMasks[1]);

      return Mask;
    }
    case llvm::DFGNode::Not: {
      assert(ParentMasks.size() == 1 && "Unexpected numbers of operands!");

      BitMask Mask = computeNot(ParentMasks[0]);

      return Mask;
    }
    case llvm::DFGNode::And: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      BitMask Mask = computeAnd(ParentMasks[0], ParentMasks[1]);

      return Mask;
    }
    case llvm::DFGNode::Or: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      BitMask Mask = computeOr(ParentMasks[0], ParentMasks[1]);

      return Mask;
    }
    case llvm::DFGNode::Xor: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      BitMask Mask = computeXor(ParentMasks[0], ParentMasks[1]);

      return Mask;
    }
    case llvm::DFGNode::RAnd: {
      assert(ParentMasks.size() == 1 && "Unexpected numbers of operands!");

      BitMask Mask = computeRand(ParentMasks[0]);

      return Mask;
    }
    case llvm::DFGNode::GT: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      BitMask Mask = computeUgt(ParentMasks[0], ParentMasks[1]);

      return Mask;
    }
    case llvm::DFGNode::EQ: {
      bool UnEqual = false;
      bool UnKnown = false;

      unsigned BitWidth = ParentMasks[0].getMaskWidth();
      assert(BitWidth == ParentMasks[1].getMaskWidth() && "Unexpected bitwidth!");

      for (unsigned i = 0; i < BitWidth; ++i) {
        if (ParentMasks[0].isOneKnownAt(i) && ParentMasks[1].isZeroKnownAt(i)) {
          UnEqual = true;
          break;
        }

        if (ParentMasks[0].isZeroKnownAt(i) && ParentMasks[1].isOneKnownAt(i)) {
          UnEqual = true;
          break;
        }

        if (!ParentMasks[0].isBitKnownAt(i) || !ParentMasks[1].isBitKnownAt(i)) {
          UnKnown = true;
        }
      }

      BitMask Mask;
      if (UnEqual) {
        Mask = BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1),
                       APInt::getNullValue(1));
      }
      else {
        if (UnKnown)
          Mask = BitMask(1);
        else
          Mask = BitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1),
                         APInt::getNullValue(1));
      }

      return Mask;
    }
    case llvm::DFGNode::NE: {
      bool UnEqual = false;
      bool UnKnown = false;

      unsigned BitWidth = ParentMasks[0].getMaskWidth();
      assert(BitWidth == ParentMasks[1].getMaskWidth() && "Unexpected bitwidth!");

      for (unsigned i = 0; i < BitWidth; ++i) {
        if (ParentMasks[0].isOneKnownAt(i) && ParentMasks[1].isZeroKnownAt(i)) {
          UnEqual = true;
          break;
        }

        if (ParentMasks[0].isZeroKnownAt(i) && ParentMasks[1].isOneKnownAt(i)) {
          UnEqual = true;
          break;
        }

        if (!ParentMasks[0].isBitKnownAt(i) || !ParentMasks[1].isBitKnownAt(i))
          UnKnown = true;

      }

      BitMask Mask;
      if (UnEqual)
        Mask = BitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1),
                       APInt::getNullValue(1));
      else {
        if (UnKnown)
          Mask = BitMask(1);
        else
          Mask = BitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1),
                         APInt::getNullValue(1));
      }

      return Mask;
    }
    case llvm::DFGNode::BitExtract: {
      assert(ParentMasks.size() == 3 && "Unexpected numbers of operands!");

      NonCommutativeDFGNode *NCNode = dyn_cast<NonCommutativeDFGNode>(Node);
      assert(NCNode && "Unexpected node type!");

      ConstantIntDFGNode *UBNode
        = dyn_cast<ConstantIntDFGNode>(NCNode->getParentNode(1));
      ConstantIntDFGNode *LBNode
        = dyn_cast<ConstantIntDFGNode>(NCNode->getParentNode(2));

      assert(UBNode && LBNode && "Unexpected node type!");

      unsigned UB = UBNode->getIntValue();
      unsigned LB = LBNode->getIntValue();

      BitMask Mask = computeBitExtract(ParentMasks[0], UB, LB);

      return Mask;
    }
    case llvm::DFGNode::BitCat: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      BitMask Mask = computeBitCat(ParentMasks[0], ParentMasks[1]);

      return Mask;
    }
    case llvm::DFGNode::BitRepeat: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      NonCommutativeDFGNode *NCNode = dyn_cast<NonCommutativeDFGNode>(Node);
      assert(NCNode && "Unexpected node type!");

      ConstantIntDFGNode *RepeatTimesNode
        = dyn_cast<ConstantIntDFGNode>(NCNode->getParentNode(1));
      unsigned RepeatTimes = RepeatTimesNode->getIntValue();

      assert(RepeatTimes * ParentMasks[0].getMaskWidth() == BitWidth &&
             "Unexpected width!");

      return computeBitRepeat(ParentMasks[0], RepeatTimes);
    }
    case llvm::DFGNode::Ret:
      break;
    case llvm::DFGNode::Register: {
      assert(ParentMasks.size() == 2 && "Unexpected numbers of operands!");

      //SIRRegister *Reg = SM->lookupSIRReg(Inst);
      //if (Reg->isFUInOut()) {
      //  return BitMask(Masks[0].getMaskWidth());
      //}

      //return BitMask(Masks[0].getKnownZeros(),
      //                  APInt::getNullValue(Masks[0].getMaskWidth()),
      //                  APInt::getNullValue(Masks[0].getMaskWidth()));

      BitMask Mask = BitMask(BitWidth);

      return Mask;
    }
    case llvm::DFGNode::InValid:
      break;
    default:
      assert(false && "Unexpected node type!");
    }
  }
}

bool isDifferentMask(BitMask NewMask, BitMask OldMask) {
  return ((NewMask.getKnownOnes() != OldMask.getKnownOnes()) ||
          (NewMask.getKnownZeros() != OldMask.getKnownZeros()) ||
          (NewMask.getKnownSames() != OldMask.getKnownSames()));
}

bool BitMaskAnalysis::computeAndUpdateMask(DFGNode *Node) {
  unsigned BitWidth = Node->getBitWidth();

  if (BitWidth == 0) {
    assert(Node->getType() == DFGNode::Ret && "Unexpected type!");
    return false;
  }

  BitMask OldMask(BitWidth);
  if (SM->hasBitMask(Node))
    OldMask = SM->getBitMask(Node);

  BitMask NewMask = computeMask(Node);

  if (isDifferentMask(NewMask, OldMask)) {
    NewMask.mergeKnownByOr(OldMask);

    SM->IndexNode2BitMask(Node, NewMask);
    return true;
  }

  SM->IndexNode2BitMask(Node, NewMask);

  return false;
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

      if (!SM->isDFGNodeExisted(Inst)) {
        continue;
      }
      DFGNode *Node = SM->getDFGNodeOfVal(Inst);
      if (!SM->hasBitMask(Node)) {
        continue;
      }
      BitMask Mask = SM->getBitMask(Node);

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

  unsigned VerifyIdx = 0;

  typedef Function::iterator bb_iterator;
  typedef BasicBlock::iterator inst_iterator;
  for (bb_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    BasicBlock *BB = BI;

    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;

      DFGNode *Node = SM->getDFGNodeOfVal(Inst);
      assert(Node && "DFG node not created!");

      if (!SM->hasBitMask(Node))
        continue;

      if (MaskedVals.count(Inst))
        continue;

      BitMask Mask = SM->getBitMask(Node);

      if (!Mask.hasAnyBitKnown())
        continue;

//       if (VerifyIdx++ > 500)
//         continue;

      Value *KnownZeros = Builder.createIntegerValue(Mask.getKnownZeros());
      Value *KnownOnes = Builder.createIntegerValue(Mask.getKnownOnes());

      unsigned BitWidth = Mask.getMaskWidth();
      Value *MaskedVal = Builder.createSOrInst(Builder.createIntegerValue(BitWidth, 1),
                                               Builder.createIntegerValue(BitWidth, 1),
                                               Inst->getType(), Inst, true);

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
      Value *Op0_part1 = Builder.createSNotInst(KnownZeros, KnownZeros->getType(), Inst, true);
      Value *Op0 = Builder.createSAndInst(Inst, Op0_part1, Inst->getType(), Inst, true);
      MaskedInst->setOperand(0, Op0);
      MaskedInst->setOperand(1, KnownOnes);
    }
  }
}

bool BitMaskAnalysis::runIteration() {
  bool Changed = false;

  typedef DataFlowGraph::node_iterator iterator;
  for (iterator I = DFG->begin(), E = DFG->end(); I != E; ++I) {
    DFGNode *Node = I;

    // Ignore the Entry & Exit.
    if (Node->isEntryOrExit())
      continue;

    // Compute and update the mask
    Changed |= computeAndUpdateMask(Node);
  }

  return Changed;
}

bool BitMaskAnalysis::runOnSIR(SIR &SM) {
  this->TD = &getAnalysis<DataLayout>();
  this->SM = &SM;

  // Get the DFG.
  DFGBuild &DB = getAnalysis<DFGBuild>();
  this->DFG = DB.getDFG();

  // Get the output path for Verilog code.
  std::string MaskOutputPath = LuaI::GetString("MaskOutput");
  std::string Error;
  raw_fd_ostream Output(MaskOutputPath.c_str(), Error);

  unsigned IterationNum = 0;

  errs() << "==========BitMask Analysis Start==========\n";
  bool Changed = true;
  while (Changed) {
    errs() << "Running BitMask Analysis in iteration #"
           << utostr_32(IterationNum++) << "\n";
    Changed = runIteration();
  }
  errs() << "==========BitMask Analysis End============\n";

  printMask(Output);
  //verifyMaskCorrectness();

  return false;
}