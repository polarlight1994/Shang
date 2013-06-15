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

#include "shang/VASTExprBuilder.h"
#include "shang/Utilities.h"

#include "llvm/Support/ErrorHandling.h"
#define DEBUG_TYPE "vast-expr-builder"
#include "llvm/Support/Debug.h"

using namespace llvm;

//===--------------------------------------------------------------------===//
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


void VASTExprBuilderContext::replaceAllUseWith(VASTValPtr From, VASTValPtr To) {
  llvm_unreachable("Function not implemented!");
}

void VASTExprBuilderContext::calculateBitCatBitMask(VASTExprPtr Expr,
                                                    APInt &KnownZeros,
                                                    APInt &KnownOnes) {
  unsigned CurUB = Expr->getBitWidth();
  unsigned ExprSize = Expr->getBitWidth();
  // Clear the mask.
  KnownOnes = KnownZeros = APInt::getNullValue(ExprSize);

  // Concatenate the bit mask together.
  for (unsigned i = 0; i < Expr->size(); ++i) {
    VASTValPtr CurBitSlice = Expr.getOperand(i);
    unsigned CurSize = CurBitSlice->getBitWidth();
    unsigned CurLB = CurUB - CurSize;
    APInt CurKnownZeros , CurKnownOnes;
    calculateBitMask(CurBitSlice, CurKnownZeros, CurKnownOnes);
    KnownZeros  |= CurKnownZeros.zextOrSelf(ExprSize).shl(CurLB);
    KnownOnes   |= CurKnownOnes.zextOrSelf(ExprSize).shl(CurLB);

    CurUB = CurLB;
  }
}

void VASTExprBuilderContext::calculateImmediateBitMask(VASTImmPtr Imm,
                                                       APInt &KnownZeros,
                                                       APInt &KnownOnes) {
  KnownOnes = Imm.getAPInt();
  KnownZeros = ~Imm.getAPInt();
}

void VASTExprBuilderContext::calculateAssignBitMask(VASTExprPtr Expr,
                                                    APInt &KnownZeros,
                                                    APInt &KnownOnes) {
  calculateBitMask(Expr.getOperand(0), KnownZeros, KnownOnes);
  // Adjust the bitmask by LB.
  KnownOnes = VASTImmediate::getBitSlice(KnownOnes, Expr->UB, Expr->LB);
  KnownZeros = VASTImmediate::getBitSlice(KnownZeros, Expr->UB, Expr->LB);
}

// The implementation of basic bit mark calucation.
void VASTExprBuilderContext::calculateBitMask(VASTValPtr V, APInt &KnownZeros,
                                              APInt &KnownOnes) {
  // Clear the mask.
  KnownOnes = KnownZeros = APInt::getNullValue(V->getBitWidth());

  // Most simple case: Immediate.
  if (VASTImmPtr Imm = dyn_cast<VASTImmPtr>(V)) {
    calculateImmediateBitMask(Imm, KnownZeros, KnownOnes);
    return;
  }

  VASTExprPtr Expr = dyn_cast<VASTExprPtr>(V);
  if (!Expr) return;

  switch(Expr->getOpcode()) {
  default: return;
  case VASTExpr::dpBitCat:
    calculateBitCatBitMask(Expr, KnownZeros, KnownOnes);
    return;
  case VASTExpr::dpAssign:
    calculateAssignBitMask(Expr, KnownZeros, KnownOnes);
    return;
  }
}

//===--------------------------------------------------------------------===//

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
  APInt KnownZeros, KnownOnes;
  calculateBitMask(Op, KnownZeros, KnownOnes);

  if (KnownZeros.getBoolValue() && Opc == VASTExpr::dpRAnd)
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

VASTValPtr VASTExprBuilder::buildBitRepeat(VASTValPtr Op, unsigned RepeatTimes){
  if (RepeatTimes == 1) return Op;

  return buildExpr(VASTExpr::dpBitRepeat, Op, getImmediate(RepeatTimes, 8),
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
  return buildOrExpr(buildAndExpr(Cnd, TrueV, BitWidth),
                     buildAndExpr(buildNotExpr(Cnd), FalseV, BitWidth),
                     BitWidth);
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
  }

  VASTValPtr Ops[] = { Op };
  return createExpr(Opc, Ops, BitWidth, 0);
}

namespace llvm {
struct AddMultOpInfoBase {
  VASTExprBuilder &Builder;
  const unsigned ResultSize;
  unsigned ActualResultSize;
  APInt ImmVal;
  unsigned ImmSize;
  unsigned MaxTailingZeros;
  VASTValPtr OpWithTailingZeros;

  AddMultOpInfoBase(VASTExprBuilder &Builder, unsigned ResultSize,
                    unsigned InitializeImmVal)
    : Builder(Builder), ResultSize(ResultSize), ActualResultSize(0),
      ImmVal(ResultSize, InitializeImmVal), ImmSize(0), MaxTailingZeros(0),
      OpWithTailingZeros(0) {}

  VASTValPtr analyzeBitMask(VASTValPtr V,  unsigned &CurTailingZeros) {
    APInt KnownZeros, KnownOnes;
    unsigned OperandSize = V->getBitWidth();
    // Trim the unused bits according to the result's size
    if (OperandSize > ResultSize)
      V = Builder.buildBitSliceExpr(V, ResultSize, 0);

    CurTailingZeros = 0;

    Builder.calculateBitMask(V, KnownZeros, KnownOnes);
    // Any known zeros?
    if (KnownZeros.getBoolValue()) {
      // Ignore the zero operand for the addition and multiplication.
      if (KnownZeros.isAllOnesValue()) return 0;

      // Any known leading zeros?
      if (unsigned LeadingZeros = KnownZeros.countLeadingOnes()) {
        unsigned NoZerosUB = OperandSize - LeadingZeros;
        V = Builder.buildBitSliceExpr(V, NoZerosUB, 0);
      }

      CurTailingZeros = KnownZeros.countTrailingOnes();
    }

    return V;
  }

  void updateTailingZeros(VASTValPtr V, unsigned CurTailingZeros) {
    // Remember the operand with tailing zeros.
    if (MaxTailingZeros < CurTailingZeros) {
      MaxTailingZeros = CurTailingZeros;
      OpWithTailingZeros = V;
    }
  }

  VASTValPtr analyzeImmOperand() {
    if (ImmVal.getBoolValue()) {
      VASTImmPtr Imm = Builder.getImmediate(ImmVal.zextOrTrunc(ImmSize));
      APInt KnownZeros = ~Imm.getAPInt();
      unsigned CurTailingZeros = KnownZeros.countTrailingOnes();

      updateTailingZeros(Imm, CurTailingZeros);

      return Imm;
    }

    return 0;
  }

  static bool sort(const VASTValPtr LHS, const VASTValPtr RHS) {
    if (LHS->getBitWidth() > RHS->getBitWidth()) return true;
    else if (LHS->getBitWidth() < RHS->getBitWidth()) return false;

    if (LHS->getASTType() < RHS->getASTType()) return true;
    else if (LHS->getASTType() > RHS->getASTType()) return false;

    return LHS < RHS;
  }
};

template<>
struct VASTExprOpInfo<VASTExpr::dpAdd> : public AddMultOpInfoBase {

  VASTExprOpInfo(VASTExprBuilder &Builder, unsigned ResultSize)
    : AddMultOpInfoBase(Builder, ResultSize, 0) {}

  void updateActualResultSize(unsigned OperandSize) {
    if (ActualResultSize == 0)
      ActualResultSize = OperandSize;
    else {
      // Each addition will produce 1 extra bit (carry bit).
      ActualResultSize = std::max(ActualResultSize, OperandSize);
      ActualResultSize = std::min(ActualResultSize + 1, ResultSize);
    }
  }

  VASTValPtr analyzeOperand(VASTValPtr V) {
    unsigned CurTailingZeros;

    V = analyzeBitMask(V, CurTailingZeros);
    if (!V) return 0;

    updateActualResultSize(V->getBitWidth());

    // Fold the immediate.
    if (VASTImmPtr Imm = dyn_cast<VASTImmPtr>(V)) {
      ImmSize = std::max(ImmSize, Imm->getBitWidth());
      // Each addition will produce 1 extra bit (carry bit).
      if (ImmVal.getBoolValue()) ImmSize = std::min(ImmSize + 1, ResultSize);

      ImmVal += Imm->getAPInt().zextOrSelf(ResultSize);
      return 0;
    }

    updateTailingZeros(V, CurTailingZeros);

    return V;
  }
};

template<>
struct VASTExprOpInfo<VASTExpr::dpMul> : public AddMultOpInfoBase {
  bool ZeroDetected;

  VASTExprOpInfo(VASTExprBuilder &Builder, unsigned ResultSize)
    : AddMultOpInfoBase(Builder, ResultSize, 1), ZeroDetected(false) {}

  void updateActualResultSize(unsigned OperandSize) {
    if (ActualResultSize == 0)
      ActualResultSize = OperandSize;
    else
      ActualResultSize = std::min(ActualResultSize + OperandSize, ResultSize);
  }

  VASTValPtr analyzeOperand(VASTValPtr V) {
    // Zero detected, no need to analyze.
    if (ZeroDetected) return 0;

    unsigned CurTailingZeros;
    V = analyzeBitMask(V, CurTailingZeros);

    if (!V) {
      ZeroDetected = true;
      return 0;
    }

    updateActualResultSize(V->getBitWidth());

    if (VASTImmPtr Imm = dyn_cast<VASTImmPtr>(V)) {
      // Fold the immediate.
      ImmSize = std::min(ResultSize, ImmSize + Imm->getBitWidth());

      ImmVal *= Imm->getAPInt().zextOrSelf(ResultSize);
      return 0;
    }

    updateTailingZeros(V, CurTailingZeros);
    return V;
  }

  VASTValPtr analyzeImmOperand() {
    // Ignore the Immediate operand if it equals to 1, because A * 1 = A.
    if (ImmVal == 1) return VASTValPtr();

    return AddMultOpInfoBase::analyzeImmOperand();
  }
};
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

static inline bool isMask(APInt Value) {
  return Value.getBoolValue() && (!((Value + 1) & Value).getBoolValue());
}

VASTValPtr VASTExprBuilder::buildMulExpr(ArrayRef<VASTValPtr> Ops,
                                         unsigned BitWidth) {
  SmallVector<VASTValPtr, 8> NewOps;
  VASTExprOpInfo<VASTExpr::dpMul> OpInfo(*this, BitWidth);

  // Do not flatten Mult at the moment.
  collectOperands<VASTExpr::dpMul>(Ops.begin(), Ops.end(),
                                   op_filler<VASTExpr::dpMul>(NewOps, OpInfo));

  if (OpInfo.ZeroDetected) return getImmediate(UINT64_C(0), BitWidth);

  // Add the immediate value back to the operand list.
  if (VASTValPtr V = OpInfo.analyzeImmOperand()) {
    APInt Imm = cast<VASTImmPtr>(V).getAPInt();
    if (Imm.isPowerOf2()) {
      unsigned lg2 = Imm.countTrailingZeros();
      VASTValPtr SubExpr = buildMulExpr(NewOps, BitWidth);
      // Implement the multiplication by shift.
      return buildShiftExpr(VASTExpr::dpShl,
                            SubExpr, getImmediate(lg2, BitWidth),
                            BitWidth);
    }

    // Implement the multiplication by shift and addition if the immediate is
    // bit mask, i.e. A * <N lower bits set> = (A << N) - A.
    if (isMask(Imm)) {
      unsigned lg2 = Imm.countTrailingOnes();
      // Lower A * -1 = A.
      if(lg2 == BitWidth)
        return buildNegative(buildMulExpr(NewOps, BitWidth));

      VASTValPtr SubExpr = buildMulExpr(NewOps, BitWidth);

      VASTValPtr ShiftedSubExpr
        = buildShiftExpr(VASTExpr::dpShl, SubExpr, getImmediate(lg2, BitWidth),
                         BitWidth);
      // Construct the subtraction: A - B = A + ~B + 1.
      VASTValPtr AddOps[] = {
        ShiftedSubExpr, buildNotExpr(SubExpr), VASTImmediate::True
      };
      return buildAddExpr(AddOps, BitWidth);
    }

    NewOps.push_back(V);
  }

  if (OpInfo.ActualResultSize < BitWidth) {
    VASTValPtr NarrowedMul = buildMulExpr(NewOps, OpInfo.ActualResultSize);
    return padHigherBits(NarrowedMul, BitWidth, false);
  }

  // Reduce the size of multiply according to tailing zero.
  if (NewOps.size() == 2 && OpInfo.OpWithTailingZeros) {
    VASTValPtr NotEndWithZeros = NewOps[0],
               EndWithZeros = OpInfo.OpWithTailingZeros;
    if (NotEndWithZeros == EndWithZeros)
      NotEndWithZeros = NewOps[1];

    unsigned TailingZeros = OpInfo.MaxTailingZeros;
    VASTValPtr TrimedOp =
      buildBitSliceExpr(EndWithZeros, EndWithZeros->getBitWidth(), TailingZeros);

    // Build the new multiply without known zeros.
    unsigned NewMultSize = BitWidth - TailingZeros;
    VASTValPtr NewMultOps[] = { NotEndWithZeros, TrimedOp };
    VASTValPtr NewMult = buildMulExpr(NewMultOps, NewMultSize);
    return padLowerBits(NewMult, BitWidth, false);
  }

  return getOrCreateCommutativeExpr(VASTExpr::dpMul, NewOps, BitWidth);
}

VASTValPtr VASTExprBuilder::buildNegative(VASTValPtr Op) {
  return buildAddExpr(buildNotExpr(Op),
                      getImmediate(1, Op->getBitWidth()),
                      Op->getBitWidth());
}

VASTValPtr VASTExprBuilder::buildAddExpr(ArrayRef<VASTValPtr> Ops,
                                         unsigned BitWidth) {
  SmallVector<VASTValPtr, 8> NewOps;
  VASTExprOpInfo<VASTExpr::dpAdd> OpInfo(*this, BitWidth);
  // Do not flatten Add at the moment.
  collectOperands<VASTExpr::dpAdd>(Ops.begin(), Ops.end(),
                                   op_filler<VASTExpr::dpAdd>(NewOps, OpInfo));

  // Add the immediate value back to the operand list.
  if (VASTValPtr V = OpInfo.analyzeImmOperand())
    NewOps.push_back(V);

  // Sort the operands excluding carry bit, we want to place the carry bit at
  // last.
  std::sort(NewOps.begin(), NewOps.end(), AddMultOpInfoBase::sort);

  // All operands are zero?
  if (NewOps.empty())
    return Context.getOrCreateImmediate(APInt::getNullValue(BitWidth));

  bool CarryPresented = NewOps.back()->getBitWidth() == 1;

  // If the addition contains only 2 operand, check if we can inline a operand
  // of this addition to make use of the carry bit.
  if (NewOps.size() < 3) {
    unsigned ExprIdx = 0;
    VASTExpr *Expr=Context.getAddExprToFlatten(NewOps[ExprIdx],!CarryPresented);
    if (Expr == 0 && NewOps.size() > 1)
      Expr = Context.getAddExprToFlatten(NewOps[++ExprIdx], !CarryPresented);

    // If we can find such expression, flatten the expression tree.
    if (Expr) {
      // Try to keep the operand bitwidth unchanged.
      unsigned OpBitwidth = NewOps[ExprIdx]->getBitWidth();
      NewOps.erase(NewOps.begin() + ExprIdx);
      assert(Expr->size() == 2&&"Unexpected operand number of sub-expression!");

      // Replace the expression by the no-carry operand
      VASTValPtr ExprLHS = Expr->getOperand(0);
      if (ExprLHS->getBitWidth() > OpBitwidth)
        ExprLHS = buildBitSliceExpr(ExprLHS, OpBitwidth, 0);

      NewOps.push_back(ExprLHS);

      VASTValPtr ExprRHS = Expr->getOperand(1);
      if (ExprRHS->getBitWidth() > OpBitwidth)
        ExprRHS = buildBitSliceExpr(ExprRHS, OpBitwidth, 0);

      NewOps.push_back(ExprRHS);

      assert(NewOps.size() < 4 && "Bad add folding!");
      return buildAddExpr(NewOps, BitWidth);
    }
  }

  if (NewOps.size() == 1)
    // Pad the higher bits by zeros.
    return padHigherBits(NewOps.back(), BitWidth, false);

  if (OpInfo.ActualResultSize < BitWidth) {
    VASTValPtr NarrowedAdd = buildAddExpr(NewOps, OpInfo.ActualResultSize);
    return padHigherBits(NarrowedAdd, BitWidth, false);
  }

  // If one of the operand has tailing zeros, we can directly forward the value
  // of the corresponding bitslice of another operand.
  if (NewOps.size() == 2 && OpInfo.OpWithTailingZeros) {
    VASTValPtr NotEndWithZeros = NewOps[0],
               EndWithZeros = OpInfo.OpWithTailingZeros;
    if (NotEndWithZeros == EndWithZeros)
      NotEndWithZeros = NewOps[1];

    unsigned TailingZeros = OpInfo.MaxTailingZeros;

    VASTValPtr Hi =
      buildBitSliceExpr(EndWithZeros, EndWithZeros->getBitWidth(),TailingZeros);
    // NotEndWithZeros cannot entirely fit into the zero bits, addition is
    // need for the higher part.
    if (NotEndWithZeros->getBitWidth() > TailingZeros) {
      VASTValPtr HiAddOps[] = {
        buildBitSliceExpr(NotEndWithZeros, NotEndWithZeros->getBitWidth(),
                          TailingZeros),
        Hi
      };
      Hi = buildAddExpr(HiAddOps, BitWidth - TailingZeros);
    } else
      // In this case, no addition is needed, we can simply concatenate the
      // operands together, still, we may pad the higher bit for additions.
      Hi = padHigherBits(Hi, BitWidth - TailingZeros, false);

    // Because the operand of addition is truncated, so it may have a smaller
    // bitwidth.
    if (NotEndWithZeros->getBitWidth() < TailingZeros)
      NotEndWithZeros = padHigherBits(NotEndWithZeros, TailingZeros, false);

    // We can directly forward the lower part.
    VASTValPtr Lo = buildBitSliceExpr(NotEndWithZeros, TailingZeros, 0);
    // Concatenate them together.
    VASTValPtr BitCatOps[] = { Hi, Lo };
    return buildBitCatExpr(BitCatOps, BitWidth);
  }

  return createExpr(VASTExpr::dpAdd, NewOps, BitWidth, 0);
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
    APInt KnownZeros, KnownOnes;
    calculateBitMask(RHSExpr, KnownZeros, KnownOnes);
    
    // Any known zeros?
    if (KnownZeros.getBoolValue()) {
      // No need to shift at all.
      if (KnownZeros.isAllOnesValue()) return LHS;

      // Any known leading zeros?
      if (unsigned LeadingZeros = KnownZeros.countLeadingOnes()) {
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
