//===--- VASTExprOptAddMult.cpp - Optimize the Adds and Mults ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement optimizations on the addition and multiplications.
//
//===----------------------------------------------------------------------===//

#include "shang/VASTExprBuilder.h"

#include "shang/Utilities.h"

#include "llvm/Support/ErrorHandling.h"
#define DEBUG_TYPE "vast-expr-opt-add-mult"
#include "llvm/Support/Debug.h"

using namespace llvm;

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
    unsigned OperandSize = V->getBitWidth();
    // Trim the unused bits according to the result's size
    if (OperandSize > ResultSize)
      V = Builder.buildBitSliceExpr(V, ResultSize, 0);

    CurTailingZeros = 0;

    VASTExprBuilder::BitMasks Masks = Builder.calculateBitMask(V);
    // Any known zeros?
    if (Masks.KnownZeros.getBoolValue()) {
      // Ignore the zero operand for the addition and multiplication.
      if (Masks.KnownZeros.isAllOnesValue()) return 0;

      // Any known leading zeros?
      if (unsigned LeadingZeros = Masks.KnownZeros.countLeadingOnes()) {
        unsigned NoZerosUB = V->getBitWidth() - LeadingZeros;
        V = Builder.buildBitSliceExpr(V, NoZerosUB, 0);
      }

      CurTailingZeros = Masks.KnownZeros.countTrailingOnes();
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
