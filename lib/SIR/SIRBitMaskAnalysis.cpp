#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

#include "llvm/ADT/Statistic.h"

using namespace llvm;
using namespace vast;

namespace {
struct SIRBitMaskAnalysis : public SIRPass {
  SIR *SM;
  DataLayout *TD;

  // Avoid visit the instruction twice in traverse.
  std::set<Instruction *> Visited;

  static char ID;
  SIRBitMaskAnalysis() : SIRPass(ID) {
    initializeSIRBitMaskAnalysisPass(*PassRegistry::getPassRegistry());
  }

  void printMask(raw_fd_ostream &Output);
  void verifyMaskCorrectness();

  // Bit extraction of BitMasks.
  APInt getBitExtraction(const APInt &OriginMask,
                         unsigned UB, unsigned LB) const {
    if (UB != OriginMask.getBitWidth() || LB != 0)
      return OriginMask.lshr(LB).sextOrTrunc(UB - LB);

    return OriginMask;
  }

  SIRBitMask computeAnd(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeOr(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeNot(SIRBitMask Mask);
  SIRBitMask computeXor(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeRand(SIRBitMask Mask);
  SIRBitMask computeRxor(SIRBitMask Mask);
  SIRBitMask computeBitCat(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeBitExtract(SIRBitMask Mask, unsigned UB, unsigned LB);
  SIRBitMask computeBitRepeat(SIRBitMask Mask, unsigned RepeatTimes);
  SIRBitMask computeAdd(SIRBitMask LHS, SIRBitMask RHS, unsigned ResultBitWidth);
  SIRBitMask computeAddc(SIRBitMask LHS, SIRBitMask RHS, SIRBitMask Carry);
  SIRBitMask computeMul(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeShl(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeLshr(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeAshr(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeUgt(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeSgt(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeUDiv(SIRBitMask LHS, SIRBitMask RHS);
  SIRBitMask computeSDiv(SIRBitMask LHS, SIRBitMask RHS);

  SIRBitMask computeMask(Instruction *Inst, SIR *SM, DataLayout *TD);
  bool computeAndUpdateMask(Instruction *Inst);
  bool traverseFromRoot(Value *Val);
  bool traverseDatapath();

  bool runIteration();
  bool runOnSIR(SIR &SM);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(SIRRegisterSynthesisForCodeGenID);
    AU.setPreservesAll();
  }
};

struct SIRDatapathOpt : public SIRPass {
  SIR *SM;
  DataLayout *TD;

  static char ID;
  SIRDatapathOpt() : SIRPass(ID) {
    initializeSIRDatapathOptPass(*PassRegistry::getPassRegistry());
  }

  bool shrinkOperatorStrengh(IntrinsicInst *II, SIRDatapathBuilder Builder);
  bool shrinkOperatorStrengh();
  bool runOnSIR(SIR &SM);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(SIRBitMaskAnalysisID);
    AU.setPreservesAll();
  }
};
}

char SIRBitMaskAnalysis::ID = 0;
char &llvm::SIRBitMaskAnalysisID = SIRBitMaskAnalysis::ID;
INITIALIZE_PASS_BEGIN(SIRBitMaskAnalysis, "sir-bit-mask-analysis",
                      "Perform the bit-level optimization",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRRegisterSynthesisForCodeGen)
INITIALIZE_PASS_END(SIRBitMaskAnalysis, "sir-bit-mask-analysis",
                    "Perform the bit-level optimization",
                    false, true)

SIRBitMask SIRBitMaskAnalysis::computeAnd(SIRBitMask LHS, SIRBitMask RHS) {
  return SIRBitMask(
    // If any zero in some bits of one of operands, then these bits of result will be zero
    LHS.getKnownZeros() | RHS.getKnownZeros(),
    // If any one in some bits of both two operands, then these bits of result will be one
    LHS.getKnownOnes() & RHS.getKnownOnes(),
    // Sign bits will be And,
    LHS.getKnownSames() & RHS.getKnownSames());
}

SIRBitMask SIRBitMaskAnalysis::computeOr(SIRBitMask LHS, SIRBitMask RHS) {
  return SIRBitMask(
    // If any zero in some bits of both two operands, then these bits of result will be zero
    LHS.getKnownZeros() & RHS.getKnownZeros(),
    // If any one in some bits of one of operands, then these bits of result will be one
    LHS.getKnownOnes() | RHS.getKnownOnes(),
    // Sign bits will be And,
    LHS.getKnownSames() & RHS.getKnownSames());
}

SIRBitMask SIRBitMaskAnalysis::computeNot(SIRBitMask Mask) {
  return SIRBitMask(Mask.getKnownOnes(), Mask.getKnownZeros(), Mask.getKnownSames());
}

SIRBitMask SIRBitMaskAnalysis::computeXor(SIRBitMask LHS, SIRBitMask RHS) {
  return SIRBitMask(
    // If some bits of both two operands are known to be same, then these bits of result will be zero
    (LHS.getKnownZeros() & RHS.getKnownZeros()) | (LHS.getKnownOnes() & RHS.getKnownOnes()),
    // If some bits of both two operands are known to be different, then these bits of result will be one
    (LHS.getKnownZeros() & RHS.getKnownOnes()) | (LHS.getKnownOnes() & RHS.getKnownZeros()),
    // Sign bits will be And,
    LHS.getKnownSames() & RHS.getKnownSames());
}

SIRBitMask SIRBitMaskAnalysis::computeRand(SIRBitMask Mask) {
  return Mask.isAllOneKnown() ? SIRBitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1)) :
                                (Mask.hasAnyZeroKnown() ? SIRBitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1)) : SIRBitMask(APInt::getNullValue(1), APInt::getNullValue(1), APInt::getNullValue(1)));
}

SIRBitMask SIRBitMaskAnalysis::computeRxor(SIRBitMask Mask) {
  if (Mask.hasAnyOneKnown() && Mask.hasAnyZeroKnown())
    return SIRBitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
  else if (Mask.isAllOneKnown() || Mask.isAllZeroKnown())
    return SIRBitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
  else
    return SIRBitMask(APInt::getNullValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
}

SIRBitMask SIRBitMaskAnalysis::computeBitCat(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned MaskWidth = LHS.getMaskWidth() + RHS.getMaskWidth();

  APInt KnownZeros = LHS.getKnownZeros().zextOrSelf(MaskWidth).shl(RHS.getMaskWidth()) | RHS.getKnownZeros().zextOrSelf(MaskWidth);
  APInt KnownOnes = LHS.getKnownOnes().zextOrSelf(MaskWidth).shl(RHS.getMaskWidth()) | RHS.getKnownOnes().zextOrSelf(MaskWidth);

  return SIRBitMask(KnownZeros, KnownOnes, LHS.getKnownSames().zextOrSelf(MaskWidth).shl(RHS.getMaskWidth()));;
}

SIRBitMask SIRBitMaskAnalysis::computeBitExtract(SIRBitMask Mask, unsigned UB, unsigned LB) {
  return SIRBitMask(getBitExtraction(Mask.getKnownZeros(), UB, LB),
                    getBitExtraction(Mask.getKnownOnes(), UB, LB),
                    getBitExtraction(Mask.getKnownSames(), UB, LB));
}

SIRBitMask SIRBitMaskAnalysis::computeBitRepeat(SIRBitMask Mask, unsigned RepeatTimes) {
  SIRBitMask NewMask = computeBitCat(Mask, Mask);

  for (unsigned i = 2; i < RepeatTimes; ++i) {
    NewMask = computeBitCat(NewMask, Mask);
  }

  if (!Mask.isAllBitKnown())
    NewMask = SIRBitMask(NewMask.getKnownZeros(), NewMask.getKnownOnes(), APInt::getAllOnesValue(RepeatTimes));

  return NewMask;
}

SIRBitMask SIRBitMaskAnalysis::computeAdd(SIRBitMask LHS, SIRBitMask RHS, unsigned ResultBitWidth) {
  unsigned BitWidth = LHS.getMaskWidth();
  assert(BitWidth == RHS.getMaskWidth() && "BitWidth not matches!");

  // Without consideration of cin, the known bits of sum will be
  // determined by s = a ^ b;
  SIRBitMask S = computeXor(LHS, RHS);

  // Without consideration of cin, the known bits of sum will be
  // determined by c = a & b;
  SIRBitMask C = computeAnd(LHS, RHS);

  SIRBitMask Carry = computeBitExtract(C, BitWidth, BitWidth - 1);
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
    S = computeXor(S, ShiftedC);
    C = computeAnd(S, ShiftedC);

    Carry = computeXor(Carry, computeBitExtract(C, BitWidth, BitWidth - 1));
  }

  SIRBitMask MaskWithCarry =  computeBitCat(Carry, S);

  if (ResultBitWidth >= MaskWithCarry.getMaskWidth())
    return SIRBitMask(MaskWithCarry.getKnownZeros().zextOrSelf(ResultBitWidth),
                      MaskWithCarry.getKnownOnes().zextOrSelf(ResultBitWidth),
                      MaskWithCarry.getKnownSames().zextOrSelf(ResultBitWidth));
  else
    return computeBitExtract(MaskWithCarry, ResultBitWidth, 0);
}

SIRBitMask SIRBitMaskAnalysis::computeAddc(SIRBitMask LHS, SIRBitMask RHS, SIRBitMask Carry) {
  unsigned BitWidth = LHS.getMaskWidth();
  assert(BitWidth == RHS.getMaskWidth() && "BitWidth not matches!");
  assert(Carry.getMaskWidth() == 1 && "Unexpected Carry BitWidth!");

  // Without consideration of cin, the known bits of sum will be
  // determined by s = a ^ b;
  SIRBitMask S = computeXor(LHS, RHS);

  // Without consideration of cin, the known bits of sum will be
  // determined by c = a & b; To be noted that, the cin will
  // catenate with the result.
  SIRBitMask C = computeAnd(LHS, RHS);

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
      ShiftedC = computeAnd(ShiftedC, Carry.extend(BitWidth));

    // Calculate the mask bit by bit considering the cin.
    S = computeXor(S, ShiftedC);
    C = computeAnd(S, ShiftedC);
  }

  return S;
}

SIRBitMask SIRBitMaskAnalysis::computeMul(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned BitWidth = LHS.getMaskWidth() + RHS.getMaskWidth();

  SIRBitMask R(APInt::getAllOnesValue(BitWidth),
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
      R = computeAdd(R, SIRBitMask(LHS.getKnownZeros().zextOrSelf(BitWidth),
                                   LHS.getKnownOnes().zextOrSelf(BitWidth),
                                   LHS.getKnownSames().zextOrSelf(BitWidth)),
                     BitWidth);
    // If the current bit is unknown, then we must make sure the known
    // zero bits of LHS is passed to the partial product.
    else if (!RHS.isZeroKnownAt(i)) {
      SIRBitMask Mask(LHS.getKnownZeros().zextOrSelf(BitWidth), APInt::getNullValue(BitWidth), LHS.getKnownSames().zextOrSelf(BitWidth));
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

SIRBitMask SIRBitMaskAnalysis::computeShl(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned RHSMaxWidth = std::min(Log2_32_Ceil(LHS.getMaskWidth()),
                                  RHS.getMaskWidth());

  SIRBitMask R = LHS;
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

SIRBitMask SIRBitMaskAnalysis::computeLshr(SIRBitMask LHS, SIRBitMask RHS) {
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

SIRBitMask SIRBitMaskAnalysis::computeAshr(SIRBitMask LHS, SIRBitMask RHS) {
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

SIRBitMask SIRBitMaskAnalysis::computeUgt(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned BitWidth = LHS.getMaskWidth();
  assert(BitWidth == RHS.getMaskWidth() && "Mask width not matches!");

  for (unsigned i = 0; i < BitWidth; ++i) {
    if ((LHS.isZeroKnownAt(BitWidth - 1 - i) && RHS.isZeroKnownAt(BitWidth - 1 - i)) ||
      (LHS.isOneKnownAt(BitWidth - 1 - i) && RHS.isOneKnownAt(BitWidth - 1 - i)))
      continue;
    else if (LHS.isOneKnownAt(BitWidth - 1 - i) && RHS.isZeroKnownAt(BitWidth - 1 - i))
      return SIRBitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
    else if (LHS.isZeroKnownAt(BitWidth - 1 - i) && RHS.isOneKnownAt(BitWidth - 1 - i))
      return SIRBitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
    else
      return SIRBitMask(1);
  }
}

SIRBitMask SIRBitMaskAnalysis::computeSgt(SIRBitMask LHS, SIRBitMask RHS) {
  unsigned BitWidth = LHS.getMaskWidth();
  assert(BitWidth == RHS.getMaskWidth() && "Mask width not matches!");

  if (LHS.isZeroKnownAt(BitWidth - 1) && RHS.isOneKnownAt(BitWidth - 1))
    return SIRBitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
  else if (LHS.isOneKnownAt(BitWidth - 1) && RHS.isZeroKnownAt(BitWidth - 1))
    return SIRBitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
  else if (LHS.isZeroKnownAt(BitWidth - 1) && RHS.isZeroKnownAt(BitWidth - 1)) {
    for (unsigned i = 0; i < BitWidth - 1; ++i) {
      if ((LHS.isZeroKnownAt(BitWidth - 2 - i) && RHS.isZeroKnownAt(BitWidth - 2 - i)) ||
        (LHS.isOneKnownAt(BitWidth - 2 - i) && RHS.isOneKnownAt(BitWidth - 2 - i)))
        continue;
      else if (LHS.isOneKnownAt(BitWidth - 2 - i) && RHS.isZeroKnownAt(BitWidth - 2 - i))
        return SIRBitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
      else if (LHS.isZeroKnownAt(BitWidth - 2 - i) && RHS.isOneKnownAt(BitWidth - 2 - i))
        return SIRBitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));
      else
        return SIRBitMask(1);
    }
  } else if (LHS.isOneKnownAt(BitWidth - 1) && RHS.isOneKnownAt(BitWidth - 1)) {
    for (unsigned i = 0; i < BitWidth - 1; ++i) {
      if ((LHS.isZeroKnownAt(BitWidth - 2 - i) && RHS.isZeroKnownAt(BitWidth - 2 - i)) ||
        (LHS.isOneKnownAt(BitWidth - 2 - i) && RHS.isOneKnownAt(BitWidth - 2 - i)))
        continue;
      else if (LHS.isOneKnownAt(BitWidth - 2 - i) && RHS.isZeroKnownAt(BitWidth - 2 - i))
        return SIRBitMask(APInt::getAllOnesValue(1), APInt::getNullValue(1), APInt::getNullValue(1));       
      else if (LHS.isZeroKnownAt(BitWidth - 2 - i) && RHS.isOneKnownAt(BitWidth - 2 - i))
        return SIRBitMask(APInt::getNullValue(1), APInt::getAllOnesValue(1), APInt::getNullValue(1));
      else
        return SIRBitMask(1);
    }
  } else
    return SIRBitMask(1);  
}

SIRBitMask SIRBitMaskAnalysis::computeUDiv(SIRBitMask LHS, SIRBitMask RHS) {
  return SIRBitMask(LHS.getMaskWidth());
}

SIRBitMask SIRBitMaskAnalysis::computeSDiv(SIRBitMask LHS, SIRBitMask RHS) {
  return SIRBitMask(LHS.getMaskWidth());
}

SIRBitMask SIRBitMaskAnalysis::computeMask(Instruction *Inst, SIR *SM, DataLayout *TD) {
  // Handle the special non-intrinsic instruction in SIR.
  if (isa<PtrToIntInst>(Inst) || isa<IntToPtrInst>(Inst) || isa<BitCastInst>(Inst)) {
    Value *Operand = Inst->getOperand(0);

    unsigned BitWidth = TD->getTypeSizeInBits(Operand->getType());

    // If the mask of operand has not been computed yet.
    if (!SM->hasBitMask(Operand)) {
      assert(isa<GlobalValue>(Operand) || (SM->lookupSIRReg(Operand) != NULL) ||
             isa<Argument>(Operand) || isa<UndefValue>(Operand) && "Unexpected value type!");

      SIRBitMask Mask(BitWidth);

      SM->IndexVal2BitMask(Operand, Mask);
      SM->IndexVal2BitMask(Inst, Mask);
      return Mask;
    }

    SIRBitMask OperandMask = SM->getBitMask(Operand);
    SM->IndexVal2BitMask(Inst, OperandMask);

    return OperandMask;
  }

  assert(isa<IntrinsicInst>(Inst) && "Unexpected instruction type!");

  SmallVector<SIRBitMask, 4> Masks;
  for (unsigned i = 0; i < Inst->getNumOperands() - 1; ++i) {
    Value *Op = Inst->getOperand(i);
    unsigned BitWidth = TD->getTypeSizeInBits(Op->getType());

    // The ports of module will not have mask.
    if (!SM->hasBitMask(Op)) {
      if (Instruction *Inst = dyn_cast<Instruction>(Op)) {
        SIRBitMask Mask(BitWidth);

        Masks.push_back(Mask);
        continue;
      }

      if (SIRRegister *Reg = SM->lookupSIRReg(Op)) {
        Masks.push_back(SIRBitMask(BitWidth));
        continue;
      }

      assert(isa<Argument>(Op) || isa<ConstantInt>(Op) || isa<UndefValue>(Op) && "Unexpected Value Type!");

      if (ConstantInt *CI = dyn_cast<ConstantInt>(Op)) {
        APInt CIAPInt = CI->getValue();
        Masks.push_back(SIRBitMask(~CIAPInt, CIAPInt, CIAPInt.getNullValue(CIAPInt.getBitWidth())));
        continue;
      }

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

    return computeAddc(Masks[0], Masks[1], Masks[2]);
  }
  case Intrinsic::shang_add: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());

    SIRBitMask UpdateMask = computeAdd(Masks[0], Masks[1], BitWidth);

    if (BitWidth < UpdateMask.getMaskWidth())
      UpdateMask = computeBitExtract(UpdateMask, BitWidth, 0);

    return UpdateMask;
  }
  case Intrinsic::shang_mul: {
    assert(Masks.size() == 2 && "Unexpected numbers of operands!");

    unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());

    SIRBitMask UpdateMask = computeMul(Masks[0], Masks[1]);

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

    SIRRegister *Reg = SM->lookupSIRReg(Inst);
    if (Reg->isFUInOut()) {
      return SIRBitMask(Masks[0].getMaskWidth());
    }

    return SIRBitMask(Masks[0].getKnownZeros(),
                      APInt::getNullValue(Masks[0].getMaskWidth()),
                      APInt::getNullValue(Masks[0].getMaskWidth()));
  }
  }
}

bool isDifferentMask(SIRBitMask NewMask, SIRBitMask OldMask) {
  return ((NewMask.getKnownOnes() != OldMask.getKnownOnes()) ||
          (NewMask.getKnownZeros() != OldMask.getKnownZeros()) ||
          (NewMask.getKnownSames() != OldMask.getKnownSames()));
}

bool SIRBitMaskAnalysis::computeAndUpdateMask(Instruction *Inst) {
  unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());

  SIRBitMask OldMask(BitWidth);
  if (SM->hasBitMask(Inst))
    OldMask = SM->getBitMask(Inst);

  SIRBitMask NewMask = computeMask(Inst, SM, TD);

  if (isDifferentMask(NewMask, OldMask)) {
    NewMask.mergeKnownByOr(OldMask);

    SM->IndexVal2BitMask(Inst, NewMask);
    return true;
  }

  SM->IndexVal2BitMask(Inst, NewMask);
  return false;
}

bool SIRBitMaskAnalysis::traverseFromRoot(Value *Val) {
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

bool SIRBitMaskAnalysis::traverseDatapath() {
  bool Changed = false;

  typedef SIR::register_iterator iterator;
  for (iterator I = SM->registers_begin(), E = SM->registers_end(); I != E; ++I) {
    SIRRegister *Reg = I;

    Changed |= traverseFromRoot(Reg->getLLVMValue());
  }

  return Changed;
}

void SIRBitMaskAnalysis::printMask(raw_fd_ostream &Output) {
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
      SIRBitMask Mask = SM->getBitMask(Inst);

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

void SIRBitMaskAnalysis::verifyMaskCorrectness() {
  Function *F = SM->getFunction();
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

      SIRBitMask Mask = SM->getBitMask(Inst);

      if (!Mask.hasAnyBitKnown())
        continue;

      Value *KnownZeros = Builder.createIntegerValue(Mask.getKnownZeros());
      Value *KnownOnes = Builder.createIntegerValue(Mask.getKnownOnes());

      unsigned BitWidth = Mask.getMaskWidth();
      Value *MaskedVal = Builder.createSOrInst(Builder.createIntegerValue(BitWidth, 1), Builder.createIntegerValue(BitWidth, 1), Inst->getType(), Inst, true);

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

bool SIRBitMaskAnalysis::runIteration() {
  bool Changed = false;

  Changed |= traverseDatapath();

  return Changed;
}

bool SIRBitMaskAnalysis::runOnSIR(SIR &SM) {
  this->TD = &getAnalysis<DataLayout>();
  this->SM = &SM;

  // Get the output path for Verilog code.
  std::string MaskOutputPath = LuaI::GetString("MaskOutput");
  std::string Error;
  raw_fd_ostream Output(MaskOutputPath.c_str(), Error);

  unsigned num_iterations = 0;

  while(runIteration()) {
    errs() << "run iteration #" << num_iterations++ << "\n";
  }

  //verifyMaskCorrectness();
  printMask(Output);

  return false;
}

char SIRDatapathOpt::ID = 0;
char &llvm::SIRDatapathOptID = SIRDatapathOpt::ID;
INITIALIZE_PASS_BEGIN(SIRDatapathOpt, "sir-datapath-optimization",
                      "Perform the datapath optimization",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRBitMaskAnalysis)
INITIALIZE_PASS_END(SIRDatapathOpt, "sir-datapath-optimization",
                    "Perform the datapath optimization",
                    false, true)

bool SIRDatapathOpt::shrinkOperatorStrengh(IntrinsicInst *II, SIRDatapathBuilder Builder) {
  SIRBitMask Mask = SM->getBitMask(II);

  // Without useful mask information, we can do nothing.
  unsigned LeadingZero = Mask.countLeadingZeros();
  if (LeadingZero == 0)
    return false;

  Intrinsic::ID ID = II->getIntrinsicID();

  if (ID == Intrinsic::shang_add || ID == Intrinsic::shang_addc) {
    errs() << "Shrinked operation by width of " << utostr_32(LeadingZero) + "\n";

    unsigned ShrinkedBitWidth = Builder.getBitWidth(II) - LeadingZero;

    // Create the shrinked operator.
    Type *ShrinkedRetTy = Builder.createIntegerType(ShrinkedBitWidth);
    Value *ShrinkedLHS = Builder.createSBitExtractInst(II->getOperand(0), ShrinkedBitWidth, 0,
                                                       ShrinkedRetTy, II, true);
    Value *ShrinkedRHS = Builder.createSBitExtractInst(II->getOperand(1), ShrinkedBitWidth, 0,
                                                       ShrinkedRetTy, II, true);

    Value *ShrinkedAdder;
    if (ID == Intrinsic::shang_add)
      ShrinkedAdder = Builder.createSAddInst(ShrinkedLHS, ShrinkedRHS, ShrinkedRetTy, II, true);
    else
      ShrinkedAdder = Builder.createSAddInst(ShrinkedLHS, ShrinkedRHS, II->getOperand(2), ShrinkedRetTy, II, true);

    // Remember to pad to bit width we need.
    Value *PadingZeros = Builder.createIntegerValue(LeadingZero, 0);
    Value *FinalResult = Builder.createSBitCatInst(PadingZeros, ShrinkedAdder, II->getType(), II, false);

    return true;
  }

  return false;
}

bool SIRDatapathOpt::shrinkOperatorStrengh() {
  // Collect all Adders and Multipliers
  SmallVector<IntrinsicInst *, 4> AddMulVector;

  Function *F = SM->getFunction();

  typedef Function::iterator bb_iterator;
  for (bb_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    BasicBlock *BB = BI;

    typedef BasicBlock::iterator inst_iterator;
    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;

      if (IntrinsicInst *IInst = dyn_cast<IntrinsicInst>(Inst)) {
        Intrinsic::ID ID = IInst->getIntrinsicID();
        
        if (ID == Intrinsic::shang_add || ID == Intrinsic::shang_addc)
          AddMulVector.push_back(IInst);
      }
    }
  }

  // Shrink the operator strength according to the bit mask.
  SIRDatapathBuilder Builder(SM, *TD);
  for (unsigned i = 0; i < AddMulVector.size(); ++i) {
    IntrinsicInst *II = AddMulVector[i];

    shrinkOperatorStrengh(II, Builder);
  }

  return false;
}

bool SIRDatapathOpt::runOnSIR(SIR &SM) {
  this->TD = &getAnalysis<DataLayout>();
  this->SM = &SM;

  shrinkOperatorStrengh();

  return false;
}