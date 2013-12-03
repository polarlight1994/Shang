//===--- VASTExprBuilder.cpp - Building Verilog AST Expressions -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Verilog AST Expressions building and optimizating
// functions.
//
//===----------------------------------------------------------------------===//

#include "vast/VASTExprBuilder.h"
#include "vast/Utilities.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"
#define DEBUG_TYPE "vast-expr-builder"
#include "llvm/Support/Debug.h"

using namespace llvm;

//===--------------------------------------------------------------------===//
APInt VASTExprBuilderContext::BitMasks::getKnownBits() const {
  return KnownZeros | KnownOnes;
}

bool VASTExprBuilderContext::BitMasks::isSubSetOf(const BitMasks &RHS) const {
  assert(!(KnownOnes & RHS.KnownZeros)
        && !(KnownZeros & RHS.KnownOnes)
        && "Bit masks contradict!");

  APInt KnownBits = getKnownBits(), RHSKnownBits = RHS.getKnownBits();
  if (KnownBits == RHSKnownBits) return false;

  return (KnownBits | RHSKnownBits) == RHSKnownBits;
}

void VASTExprBuilderContext::BitMasks::dump() const {
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

VASTImmediate *VASTExprBuilderContext::getOrCreateImmediate(const APInt &Value) {
  llvm_unreachable("reach Unimplemented function of VASTExprBuilderContext!");
  return 0;
}

VASTValPtr VASTExprBuilderContext::createExpr(VASTExpr::Opcode Opc,
                                              ArrayRef<VASTValPtr> Ops,
                                              unsigned UB, unsigned LB) {
  llvm_unreachable("reach Unimplemented function of VASTExprBuilderContext!");
  return 0;
}

void VASTExprBuilderContext::deleteContenxt(VASTValue *V) {
  BitMaskCache.erase(V);
}

void VASTExprBuilderContext::replaceAllUseWith(VASTValPtr From, VASTValPtr To) {
  llvm_unreachable("Function not implemented!");
}

VASTExprBuilderContext::BitMasks
VASTExprBuilderContext::calculateBitCatBitMask(VASTExpr *Expr) {
  unsigned CurUB = Expr->getBitWidth();
  unsigned ExprSize = Expr->getBitWidth();
  // Clear the mask.
  APInt KnownOnes = APInt::getNullValue(ExprSize),
        KnownZeros = APInt::getNullValue(ExprSize);

  // Concatenate the bit mask together.
  for (unsigned i = 0; i < Expr->size(); ++i) {
    VASTValPtr CurBitSlice = Expr->getOperand(i);
    unsigned CurSize = CurBitSlice->getBitWidth();
    unsigned CurLB = CurUB - CurSize;
    BitMasks CurMask = calculateBitMask(CurBitSlice);
    KnownZeros  |= CurMask.KnownZeros.zextOrSelf(ExprSize).shl(CurLB);
    KnownOnes   |= CurMask.KnownOnes.zextOrSelf(ExprSize).shl(CurLB);

    CurUB = CurLB;
  }

  return BitMasks(KnownZeros, KnownOnes);
}

VASTExprBuilderContext::BitMasks
VASTExprBuilderContext::calculateImmediateBitMask(VASTImmediate *Imm) {
  return BitMasks(~Imm->getAPInt(), Imm->getAPInt());
}

VASTExprBuilderContext::BitMasks
VASTExprBuilderContext::calculateAssignBitMask(VASTExpr *Expr) {
  unsigned UB = Expr->UB, LB = Expr->LB;
  BitMasks CurMask = calculateBitMask(Expr->getOperand(0));
  // Adjust the bitmask by LB.
  return BitMasks(VASTImmediate::getBitSlice(CurMask.KnownZeros, UB, LB),
                  VASTImmediate::getBitSlice(CurMask.KnownOnes, UB, LB));
}

VASTExprBuilderContext::BitMasks
VASTExprBuilderContext::calculateAndBitMask(VASTExpr *Expr) {
  unsigned BitWidth = Expr->getBitWidth();
  // Assume all bits are 1s.
  BitMasks Mask(APInt::getNullValue(BitWidth),
                APInt::getAllOnesValue(BitWidth));

  for (unsigned i = 0; i < Expr->size(); ++i) {
    BitMasks OperandMask = calculateBitMask(Expr->getOperand(i));
    // The bit become zero if the same bit in any operand is zero.
    Mask.KnownZeros |= OperandMask.KnownZeros;
    // The bit is one only if the same bit in all operand are zeros.
    Mask.KnownOnes &= OperandMask.KnownOnes;
  }

  return Mask;
}

// The implementation of basic bit mark calucation.
VASTExprBuilderContext::BitMasks
VASTExprBuilderContext::calculateBitMask(VASTValue *V) {
  BitMaskCacheTy::iterator I = BitMaskCache.find(V);
  // Return the cached version if possible.
  if (I != BitMaskCache.end()) {
    return I->second;
  }

  // Most simple case: Immediate.
  if (VASTImmediate *Imm = dyn_cast<VASTImmediate>(V))
    return setBitMask(V, calculateImmediateBitMask(Imm));

  VASTExpr *Expr = dyn_cast<VASTExpr>(V);
  if (!Expr) return BitMasks(V->getBitWidth());

  switch(Expr->getOpcode()) {
  default: break;
  case VASTExpr::dpBitCat:
    return setBitMask(V, calculateBitCatBitMask(Expr));
  case VASTExpr::dpAssign:
    return setBitMask(V, calculateAssignBitMask(Expr));
  case VASTExpr::dpAnd:
    return setBitMask(V, calculateAndBitMask(Expr));
  case VASTExpr::dpKeep:
    return setBitMask(V, calculateBitMask(Expr->getOperand(0)));
  }

  return BitMasks(Expr->getBitWidth());
}

VASTExprBuilderContext::BitMasks
VASTExprBuilderContext::calculateBitMask(VASTValPtr V) {
  BitMasks Masks = calculateBitMask(V.get());
  // Flip the bitmask if the value is inverted.
  if (V.isInverted()) return BitMasks(Masks.KnownOnes, Masks.KnownZeros);

  return Masks;
}

//===--------------------------------------------------------------------===//

MinimalExprBuilderContext::MinimalExprBuilderContext(DatapathContainer &Datapath)
  : Datapath(Datapath) {
  Datapath.pushContext(this);
}

VASTImmediate *MinimalExprBuilderContext::getOrCreateImmediate(const APInt &Value) {
  return Datapath.getOrCreateImmediateImpl(Value);
}

VASTValPtr MinimalExprBuilderContext::createExpr(VASTExpr::Opcode Opc,
                                                 ArrayRef<VASTValPtr> Ops,
                                                 unsigned UB, unsigned LB) {
  return Datapath.createExprImpl(Opc, Ops, UB, LB);
}

void MinimalExprBuilderContext::replaceAllUseWith(VASTValPtr From,
                                                  VASTValPtr To) {
  Datapath.replaceAllUseWithImpl(From, To);
}

MinimalExprBuilderContext::~MinimalExprBuilderContext() {
  Datapath.popContext(this);
  Datapath.gc();
}

//===--------------------------------------------------------------------===//
bool
VASTExprBuilder::GetMaskSplitPoints(APInt Mask, unsigned &HiPt, unsigned &LoPt) {
  unsigned BitWidth = Mask.getBitWidth();
  HiPt = BitWidth;
  LoPt = 0;

  if (Mask.isAllOnesValue() || !Mask.getBoolValue()) return false;

  unsigned NormalLoPt = Mask.countTrailingZeros();
  unsigned NormalHiPt = BitWidth - Mask.countLeadingZeros();

  APInt InvertedMask = ~Mask;
  unsigned InvertedLoPt = InvertedMask.countTrailingZeros();
  unsigned InvertedHiPt = BitWidth - InvertedMask.countLeadingZeros();

  HiPt = std::min(NormalHiPt, InvertedHiPt);
  LoPt = std::max(NormalLoPt, InvertedLoPt);

  // We may get a single spliting point, set the lower spliting point to 0 in
  // this case.
  if (HiPt == LoPt) LoPt = 0;

  assert(HiPt > LoPt && "Illegal spliting point!");

  return true;
}

VASTValPtr VASTExprBuilder::buildNotExpr(VASTValPtr U) {
  U = U.invert();

  // Try to promote the invert flag
  if (U.isInverted()) {
    if (VASTImmPtr ImmPtr = dyn_cast<VASTImmPtr>(U))
        return getImmediate(ImmPtr.getAPInt());

    if (VASTExpr *Expr = dyn_cast<VASTExpr>(U.get())) {
      if (Expr->getOpcode() == VASTExpr::dpBitCat) {
        typedef VASTExpr::op_iterator it;
        SmallVector<VASTValPtr, 4> Ops;
        for (it I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I)
          Ops.push_back(buildNotExpr(*I));

        return buildBitCatExpr(Ops, Expr->getBitWidth());
      }
    }
  }

  return U;
}

VASTValPtr VASTExprBuilder::foldBitSliceExpr(VASTValPtr U, uint8_t UB,
                                             uint8_t LB) {
  unsigned OperandSize = U->getBitWidth();
  // Not a sub bitslice.
  if (UB == OperandSize && LB == 0) return U;

  if (VASTImmPtr Imm = dyn_cast<VASTImmPtr>(U))
    return Context.getOrCreateImmediate(Imm.getBitSlice(UB, LB));

  VASTExprPtr Expr = dyn_cast<VASTExprPtr>(U);

  if (Expr == 0) return VASTValPtr(0);

  if (Expr->getOpcode() == VASTExpr::dpAssign){
    unsigned Offset = Expr->LB;
    UB += Offset;
    LB += Offset;
    return buildBitSliceExpr(Expr.getOperand(0), UB, LB);
  }

  if (Expr->getOpcode() == VASTExpr::dpBitCat) {
    // Collect the bitslices which fall into (UB, LB]
    SmallVector<VASTValPtr, 8> Ops;
    unsigned CurUB = Expr->getBitWidth(), CurLB = 0;
    unsigned LeadingBitsToLeft = 0, TailingBitsToTrim = 0;
    for (unsigned i = 0; i < Expr->size(); ++i) {
      VASTValPtr CurBitSlice = Expr.getOperand(i);
      CurLB = CurUB - CurBitSlice->getBitWidth();
      // Not fall into (UB, LB] yet.
      if (CurLB >= UB) {
        CurUB = CurLB;
        continue;
      }
      // The entire range is visited.
      if (CurUB <= LB) break;
      // Now we have CurLB < UB and CurUB > LB.
      // Compute LeadingBitsToLeft if UB fall into [CurUB, CurLB), which imply
      // CurUB >= UB >= CurLB.
      if (CurUB >= UB) LeadingBitsToLeft = UB - CurLB;
      // Compute TailingBitsToTrim if LB fall into (CurUB, CurLB], which imply
      // CurUB >= LB >= CurLB.
      if (LB >= CurLB) TailingBitsToTrim = LB - CurLB;

      Ops.push_back(CurBitSlice);
      CurUB = CurLB;
    }

    // Trivial case: Only 1 bitslice in range.
    if (Ops.size() == 1)
      return buildBitSliceExpr(Ops.back(), LeadingBitsToLeft, TailingBitsToTrim);

    Ops.front() = buildBitSliceExpr(Ops.front(), LeadingBitsToLeft, 0);
    Ops.back() = buildBitSliceExpr(Ops.back(), Ops.back()->getBitWidth(),
                                   TailingBitsToTrim);

    return buildBitCatExpr(Ops, UB - LB);
  }

  if (Expr->getOpcode() == VASTExpr::dpBitRepeat) {
    VASTValPtr Pattern = Expr.getOperand(0);
    // Simply repeat the pattern by the correct number.
    if (Pattern->getBitWidth() == 1) return buildBitRepeat(Pattern, UB - LB);
    // TODO: Build the correct pattern.
  }

  return VASTValPtr(0);
}

static VASTExprPtr getAsBitSliceExpr(VASTValPtr V) {
  VASTExprPtr Expr = dyn_cast<VASTExprPtr>(V);
  if (!Expr || !Expr->isSubBitSlice()) return 0;

  return Expr;
}

VASTValPtr VASTExprBuilder::buildBitCatExpr(ArrayRef<VASTValPtr> Ops,
                                            unsigned BitWidth) {
  SmallVector<VASTValPtr, 8> NewOps;
  flattenExpr<VASTExpr::dpBitCat>(Ops.begin(), Ops.end(),
                                  std::back_inserter(NewOps));

  VASTImmPtr LastImm = dyn_cast<VASTImmPtr>(NewOps[0]);
  VASTExprPtr LastBitSlice = getAsBitSliceExpr(NewOps[0]);

  unsigned ActualOpPos = 1;

  // Merge the constant sequence.
  for (unsigned i = 1, e = NewOps.size(); i < e; ++i) {
    VASTValPtr V = NewOps[i];
    if (VASTImmPtr CurImm = dyn_cast<VASTImmPtr>(V)) {
      if (LastImm) {
        // Merge the constants.
        APInt HiVal = LastImm.getAPInt(), LoVal = CurImm.getAPInt();
        unsigned HiSizeInBits = LastImm->getBitWidth(),
                 LoSizeInBits = CurImm->getBitWidth();
        unsigned SizeInBits = LoSizeInBits + HiSizeInBits;
        APInt Val = LoVal.zextOrSelf(SizeInBits);
        Val |= HiVal.zextOrSelf(SizeInBits).shl(LoSizeInBits);
        LastImm = Context.getOrCreateImmediate(Val);
        NewOps[ActualOpPos - 1] = LastImm; // Modify back.
        continue;
      } else {
        LastImm = CurImm;
        NewOps[ActualOpPos++] = V; //push_back.
        continue;
      }
    } else // Reset LastImm, since the current value is not immediate.
      LastImm = 0;

    if (VASTExprPtr CurBitSlice = getAsBitSliceExpr(V)) {
      VASTValPtr CurBitSliceParent = CurBitSlice.getOperand(0);
      if (LastBitSlice && CurBitSliceParent == LastBitSlice.getOperand(0)
          && LastBitSlice->LB == CurBitSlice->UB) {
        VASTValPtr MergedBitSlice
          = buildBitSliceExpr(CurBitSliceParent, LastBitSlice->UB,
                              CurBitSlice->LB);
        NewOps[ActualOpPos - 1] = MergedBitSlice; // Modify back.
        LastBitSlice = getAsBitSliceExpr(MergedBitSlice);
        continue;
      } else {
        LastBitSlice = CurBitSlice;
        NewOps[ActualOpPos++] = V; //push_back.
        continue;
      }
    } else
      LastBitSlice = 0;

    NewOps[ActualOpPos++] = V; //push_back.
  }

  NewOps.resize(ActualOpPos);
  if (NewOps.size() == 1) return NewOps.back();

#ifndef NDEBUG
  unsigned TotalBits = 0;
  for (unsigned i = 0, e = NewOps.size(); i < e; ++i)
    TotalBits += NewOps[i]->getBitWidth();
  if (TotalBits != BitWidth) {
    dbgs() << "Bad bitcat operands: \n";
    for (unsigned i = 0, e = NewOps.size(); i < e; ++i)
      NewOps[i]->dump();
    llvm_unreachable("Bitwidth not match!");
  }
#endif

  return createExpr(VASTExpr::dpBitCat, NewOps, BitWidth, 0);
}

VASTValPtr VASTExprBuilder::buildBitSliceExpr(VASTValPtr U, uint8_t UB,
                                              uint8_t LB) {
  assert(UB <= U->getBitWidth() && UB > LB && "Bad bit range!");
  // Try to fold the expression.
  if (VASTValPtr P = foldBitSliceExpr(U, UB, LB)) return P;

  VASTValPtr Ops[] = { U };
  return createExpr(VASTExpr::dpAssign, Ops, UB, LB);
}

VASTValPtr VASTExprBuilder::buildReduction(VASTExpr::Opcode Opc,VASTValPtr Op) {
  if (VASTImmPtr Imm = dyn_cast<VASTImmPtr>(Op)) {
    APInt Val = Imm.getAPInt();
    switch (Opc) {
    case VASTExpr::dpRAnd:
      // Only reduce to 1 if all bits are 1.
      if (Val.isAllOnesValue())
        return getBoolImmediate(true);
      else
        return getBoolImmediate(false);
    case VASTExpr::dpRXor:
      // Only reduce to 1 if there are odd 1s.
      if (Val.countPopulation() & 0x1)
        return getBoolImmediate(true);
      else
        return getBoolImmediate(false);
      break; // FIXME: Who knows how to evaluate this?
    default:  llvm_unreachable("Unexpected Reduction Node!");
    }
  }

  // Try to fold the expression according to the bit mask.
  BitMasks Masks = calculateBitMask(Op);

  if (Masks.KnownZeros.getBoolValue() && Opc == VASTExpr::dpRAnd)
    return getBoolImmediate(false);

  // Promote the reduction to the operands.
  if (VASTExpr *Expr = dyn_cast<VASTExpr>(Op)) {
    switch (Expr->getOpcode()) {
    default: break;
    case VASTExpr::dpBitCat: {
      SmallVector<VASTValPtr, 8> Ops;
      typedef VASTExpr::op_iterator it;
      for (it I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I)
        Ops.push_back(buildReduction(Opc, *I));

      switch (Opc) {
      case VASTExpr::dpRAnd: return buildAndExpr(Ops, 1);
      case VASTExpr::dpRXor: return buildXorExpr(Ops, 1);
      default:  llvm_unreachable("Unexpected Reduction Node!");
      }
    }
    }
  }

  return createExpr(Opc, Op, 1, 0);
}

VASTValPtr
VASTExprBuilder::getOrCreateCommutativeExpr(VASTExpr::Opcode Opc,
                                            SmallVectorImpl<VASTValPtr> &Ops,
                                             unsigned BitWidth) {
  std::sort(Ops.begin(), Ops.end(), VASTValPtr::type_less);
  return createExpr(Opc, Ops, BitWidth, 0);
}

VASTValPtr
VASTExprBuilder::replaceKnownBits(VASTValPtr V, const BitMasks &Mask) {
  // Split the word according to known bits.
  unsigned HiPt, LoPt;
  unsigned BitWidth = V->getBitWidth();
  APInt Knowns = Mask.getKnownBits();

  if (!GetMaskSplitPoints(Knowns, HiPt, LoPt)) return VASTValPtr();

  VASTImmediate *Imm = getImmediate(Mask.KnownOnes);
  assert(BitWidth >= HiPt && HiPt > LoPt && "Bad split point!");
  SmallVector<VASTValPtr, 4> Ops;

  if (HiPt != BitWidth) {
    bool BitsKnown
      = VASTImmediate::getBitSlice(Knowns, BitWidth, HiPt).isAllOnesValue();
    Ops.push_back(buildBitSliceExpr(BitsKnown ? Imm : V, BitWidth, HiPt));
  }

  {
    bool BitsKnown
      = VASTImmediate::getBitSlice(Knowns, HiPt, LoPt).isAllOnesValue();
    Ops.push_back(buildBitSliceExpr(BitsKnown ? Imm : V, HiPt, LoPt));
  }

  if (LoPt != 0) {
    bool BitsKnown
      = VASTImmediate::getBitSlice(Knowns, LoPt, 0).isAllOnesValue();
    Ops.push_back(buildBitSliceExpr(BitsKnown ? Imm : V, LoPt, 0));
  }

  return buildBitCatExpr(Ops, BitWidth);
}

VASTValPtr VASTExprBuilder::buildBitRepeat(VASTValPtr Op, unsigned RepeatTimes){
  if (RepeatTimes == 1) return Op;

  if (VASTImmPtr Imm = dyn_cast<VASTImmediate>(Op)) {
    if (Op->getBitWidth() == 1)
      return Imm.getAPInt().getBoolValue() ?
             getImmediate(APInt::getAllOnesValue(RepeatTimes)) :
             getImmediate(APInt::getNullValue(RepeatTimes));
  }

  return createExpr(VASTExpr::dpBitRepeat, Op, getImmediate(RepeatTimes, 8),
                    RepeatTimes * Op->getBitWidth());
}

VASTValPtr VASTExprBuilder::buildSelExpr(VASTValPtr Cnd, VASTValPtr TrueV,
                                         VASTValPtr FalseV, unsigned BitWidth) {
  assert(Cnd->getBitWidth() == 1 && "Bad condition width!");
  assert(TrueV->getBitWidth() == FalseV->getBitWidth()
         && TrueV->getBitWidth() == BitWidth && "Bad bitwidth!");

  if (VASTImmPtr Imm = dyn_cast<VASTImmPtr>(Cnd))
    return Imm.getAPInt().getBoolValue() ? TrueV : FalseV;


  Cnd = buildBitRepeat(Cnd, BitWidth);
  VASTValPtr V = buildOrExpr(buildAndExpr(Cnd, TrueV, BitWidth),
                             buildAndExpr(buildNotExpr(Cnd), FalseV, BitWidth),
                             BitWidth);

  // Use the known bits if possible.
  BitMasks TrueBits = calculateBitMask(TrueV);
  BitMasks FalseBits = calculateBitMask(FalseV);
  BitMasks Knowns(TrueBits.KnownZeros & FalseBits.KnownZeros,
                  TrueBits.KnownOnes & FalseBits.KnownOnes);

  if (VASTValPtr NewV = replaceKnownBits(V, Knowns))
    return NewV;

  return V;
}

VASTValPtr VASTExprBuilder::buildExpr(VASTExpr::Opcode Opc, VASTValPtr LHS,
                                      VASTValPtr RHS, unsigned BitWidth) {
  VASTValPtr Ops[] = { LHS, RHS };
  return buildExpr(Opc, Ops, BitWidth);
}

VASTValPtr VASTExprBuilder::buildExpr(VASTExpr::Opcode Opc, VASTValPtr Op0,
                                       VASTValPtr Op1, VASTValPtr Op2,
                                       unsigned BitWidth) {
  VASTValPtr Ops[] = { Op0, Op1, Op2 };
  return buildExpr(Opc, Ops, BitWidth);
}

VASTValPtr VASTExprBuilder::buildExpr(VASTExpr::Opcode Opc,
                                      ArrayRef<VASTValPtr> Ops,
                                      unsigned BitWidth) {
  switch (Opc) {
  default: break;
  case VASTExpr::dpAdd:  return buildAddExpr(Ops, BitWidth);
  case VASTExpr::dpMul:  return buildMulExpr(Ops, BitWidth);
  case VASTExpr::dpAnd:  return buildAndExpr(Ops, BitWidth);
  case VASTExpr::dpBitCat: return buildBitCatExpr(Ops, BitWidth);
  case VASTExpr::dpShl:
  case VASTExpr::dpSRA:
  case VASTExpr::dpSRL:
    assert(Ops.size() == 2 && "Bad Operand input!");
    return buildShiftExpr(Opc, Ops[0], Ops[1], BitWidth);
  case VASTExpr::dpSGT:
  case VASTExpr::dpUGT:
    assert(Ops.size() == 2 && "Bad Operand input!");
    assert(BitWidth == 1 && "Bitwidth of ICmp should be 1!");
    return buildICmpExpr(Opc, Ops[0], Ops[1]);
  case VASTExpr::dpRAnd:
  case VASTExpr::dpRXor:
    assert(Ops.size() == 1 && "Unexpected more than 1 operands for reduction!");
    assert(BitWidth == 1 && "Bitwidth of reduction should be 1!");
    return buildReduction(Opc, Ops[0]);
  case VASTExpr::dpBitRepeat: {
    assert(Ops.size() == 2 && "Bad expression size!");
    VASTImmPtr Imm = cast<VASTImmPtr>(Ops[1]);
    unsigned Times = Imm.getAPInt().getZExtValue();
    assert(Times * Ops[0]->getBitWidth() == BitWidth && "Bitwidth not match!");
    return buildBitRepeat(Ops[0], Times);
  }
  case VASTExpr::dpKeep:
    assert(Ops.size() == 1 && "Unexpected more than 1 operands for reduction!");
    assert(BitWidth == Ops[0]->getBitWidth() && "Bad bitwidth!");
    return buildKeep(Ops[0]);
  }

  return createExpr(Opc, Ops, BitWidth, 0);
}

VASTValPtr VASTExprBuilder::buildExpr(VASTExpr::Opcode Opc, VASTValPtr Op,
                                      unsigned BitWidth) {
  switch (Opc) {
  default: break;
  case VASTExpr::dpRAnd:
  case VASTExpr::dpRXor:
    assert(BitWidth == 1 && "Bitwidth of reduction should be 1!");
    return buildReduction(Opc, Op);
  case VASTExpr::dpKeep:
    assert(BitWidth == Op->getBitWidth() && "Bad bitwidth!");
    return buildKeep(Op);
  }

  VASTValPtr Ops[] = { Op };
  return createExpr(Opc, Ops, BitWidth, 0);
}

VASTValPtr VASTExprBuilder::buildKeep(VASTValPtr V) {
  VASTExprPtr Expr = dyn_cast<VASTExprPtr>(V);

  // Only keep expressions!
  if (!Expr) return V;
  
  switch (Expr->getOpcode()) {
  default:break;
    // No need to keep twice!
  case VASTExpr::dpKeep:
    return V;
  case VASTExpr::dpBitRepeat:
    return buildExpr(VASTExpr::dpBitRepeat, buildKeep(Expr.getOperand(0)),
                     Expr->getOperand(1), Expr->getBitWidth());
  case VASTExpr::dpBitCat: {
    typedef VASTExpr::op_iterator iterator;
    SmallVector<VASTValPtr, 4> Ops;
    for (iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I)
      Ops.push_back(buildKeep(*I));
    return buildBitCatExpr(Ops, Expr->getBitWidth());
  }
  }

  VASTValPtr Ops[] = { V };
  return createExpr(VASTExpr::dpKeep, Ops, V->getBitWidth(), 0);
}

VASTValPtr VASTExprBuilder::padHeadOrTail(VASTValPtr V, unsigned BitWidth,
                                          bool ByOnes, bool PadTail) {
  assert(BitWidth >= V->getBitWidth() && "Bad bitwidth!");
  unsigned ZeroBits = BitWidth - V->getBitWidth();

  if (ZeroBits == 0) return V;

  VASTValPtr Pader =
    Context.getOrCreateImmediate(ByOnes ? ~UINT64_C(0) : UINT64_C(0), ZeroBits);

  VASTValPtr Hi = PadTail ? V : Pader, Lo = PadTail ? Pader : V;

  VASTValPtr Ops[] = { Hi, Lo};
  return buildBitCatExpr(Ops, BitWidth);
}

VASTValPtr VASTExprBuilder::buildOrExpr(ArrayRef<VASTValPtr> Ops,
                                        unsigned BitWidth) {
  if (Ops.size() == 1) return Ops[0];

  assert (Ops.size() > 1 && "There should be more than one operand!!");

  SmallVector<VASTValPtr, 4> NotExprs;
  // Build the operands of Or operation into not Expr.
  for (unsigned i = 0; i < Ops.size(); ++i) {
    VASTValPtr V = buildNotExpr(Ops[i]);
    NotExprs.push_back(V);
  }

  // Build Or operation with the And Inverter Graph (AIG).
  return buildNotExpr(buildAndExpr(NotExprs, BitWidth));
}

VASTValPtr VASTExprBuilder::buildXorExpr(ArrayRef<VASTValPtr> Ops,
                                         unsigned BitWidth) {
  assert (Ops.size() == 2 && "There should be more than one operand!!");

  // Build the Xor Expr with the And Inverter Graph (AIG).
  return buildAndExpr(buildOrExpr(Ops, BitWidth),
                      buildNotExpr(buildAndExpr(Ops, BitWidth)),
                      BitWidth);
}

VASTValPtr VASTExprBuilder::buildShiftExpr(VASTExpr::Opcode Opc, 
                                           VASTValPtr LHS, 
                                           VASTValPtr RHS, 
                                           unsigned BitWidth) {
  // Limit the shift amount so keep the behavior of the hardware the same as
  // the corresponding software.
  unsigned RHSMaxSize = Log2_32_Ceil(LHS->getBitWidth());
  if (RHS->getBitWidth() > RHSMaxSize)
    RHS = buildBitSliceExpr(RHS, RHSMaxSize, 0);

  if (VASTExprPtr RHSExpr = dyn_cast<VASTExprPtr>(RHS)) {
    BitMasks Masks = calculateBitMask(RHSExpr);
    
    // Any known zeros?
    if (Masks.KnownZeros.getBoolValue()) {
      // No need to shift at all.
      if (Masks.KnownZeros.isAllOnesValue()) return LHS;

      // Any known leading zeros?
      if (unsigned LeadingZeros = Masks.KnownZeros.countLeadingOnes()) {
        unsigned NoZerosUB = RHS->getBitWidth() - LeadingZeros;
        RHS = buildBitSliceExpr(RHS, NoZerosUB, 0);
      }
    }
  }

  if (VASTImmPtr Imm = dyn_cast<VASTImmPtr>(RHS)) {
    unsigned ShiftAmount = Imm.getAPInt().getZExtValue();

    // If we not shift at all, simply return the operand.
    if (ShiftAmount == 0) return LHS;

    switch(Opc) {
    case VASTExpr::dpShl:{
      VASTValPtr PaddingBits = getImmediate(0, ShiftAmount);
      LHS = buildBitSliceExpr(LHS, LHS->getBitWidth() - ShiftAmount, 0);
      VASTValPtr Ops[] = { LHS, PaddingBits }; 
      return buildBitCatExpr(Ops, BitWidth);
    }
    case VASTExpr::dpSRL:{
      VASTValPtr PaddingBits = getImmediate(0, ShiftAmount);
      LHS = buildBitSliceExpr(LHS, LHS->getBitWidth(), ShiftAmount);
      VASTValPtr Ops[] = { PaddingBits, LHS }; 
      return buildBitCatExpr(Ops, BitWidth);
    }
    case VASTExpr::dpSRA:{
      VASTValPtr SignBits = buildBitRepeat(getSignBit(LHS), ShiftAmount);
      LHS = buildBitSliceExpr(LHS, LHS->getBitWidth(), ShiftAmount);
      VASTValPtr Ops[] = { SignBits, LHS }; 
      return buildBitCatExpr(Ops, BitWidth);   
    }
    default: llvm_unreachable("Unexpected opcode!"); break;
    }
  }

  // Optimize the 1-bit shift: A = B operator C (C is bool) =>
  // A = C ? (B operator 1) : B.
  if (RHS->getBitWidth() == 1) {
    VASTValPtr ShiftBy1 = buildExpr(Opc, LHS, getBoolImmediate(true), BitWidth);
    return buildSelExpr(RHS, ShiftBy1, LHS, BitWidth);
  }

  VASTValPtr Ops[] = { LHS, RHS }; 
  return createExpr(Opc, Ops, BitWidth, 0);
}

VASTValPtr VASTExprBuilder::buildZExtExpr(VASTValPtr V, unsigned DstBitWidth) {
  assert(DstBitWidth > V->getBitWidth() && "Unexpected DstBitWidth!");
  return padHigherBits(V, DstBitWidth, false);
}

VASTValPtr VASTExprBuilder::buildSExtExpr(VASTValPtr V, unsigned DstBitWidth) {
  assert(DstBitWidth > V->getBitWidth() && "Unexpected DstBitWidth!");
  unsigned NumExtendBits = DstBitWidth - V->getBitWidth();
  VASTValPtr SignBit = getSignBit(V);

  VASTValPtr ExtendBits = buildExpr(VASTExpr::dpBitRepeat, SignBit,
                                    getImmediate(NumExtendBits, 8),
                                    NumExtendBits);
  VASTValPtr Ops[] = { ExtendBits, V };
  return buildBitCatExpr(Ops, DstBitWidth);
}
