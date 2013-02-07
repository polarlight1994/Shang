//===----- VASTExprBuilder.h - Building Verilog AST Expressions -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The Interface to build and optimze VASTExprs.
//
//===----------------------------------------------------------------------===//
#ifndef VTM_VASTEXPR_BUILDER_H
#define VTM_VASTEXPR_BUILDER_H

#include "shang/VASTDatapathNodes.h"
#include "llvm/Support/Allocator.h"

namespace llvm {
struct VASTExprHelper {
  SmallVector<VASTValPtr, 4> Operands;
  VASTExpr::Opcode Opc;
  unsigned BitWidth;
  bool BuildNot;

  void init(VASTExpr::Opcode opc, unsigned bitWidth, bool buildNot = false) {
    Opc = opc;
    BitWidth = bitWidth;
    BuildNot = buildNot;
  }

  void addOperand(VASTValPtr V) {
    Operands.push_back(V.getAsInlineOperand());
  }
};

class VASTExprBuilderContext {
public:
  virtual ~VASTExprBuilderContext() {}

  virtual bool shouldExprBeFlatten(VASTExpr *E) const {
    return E->isInlinable();
  }

  VASTValPtr stripZeroBasedBitSlize(VASTValPtr V) {
    VASTExprPtr Expr = dyn_cast<VASTExprPtr>(V);
    if (Expr.get() && Expr->isSubBitSlice() && Expr->LB == 0)
      return Expr.getOperand(0);

    return V;
  }

  // If V is an addition which can be flatten the addition that using its result
  // return the expression, or return null otherwise.
  virtual VASTExpr *getAddExprToFlatten(VASTValPtr V, bool MustHasCarry) {
    V = stripZeroBasedBitSlize(V);

    VASTExpr *Expr = dyn_cast<VASTExpr>(V);
    if (!Expr || Expr->getOpcode() != VASTExpr::dpAdd) return 0;

    // We only flatten the expression to make full use of the carry bit.
    // So check if there is only 2 operand and the second operand can be fitted
    // into the carry bit.
    if (Expr->size() != 2) return 0;

    if (MustHasCarry && Expr->getOperand(1)->getBitWidth() != 1)
      return 0;

    return Expr;
  }

  VASTImmediate *getOrCreateImmediate(uint64_t Value, int8_t BitWidth) {
    return getOrCreateImmediate(APInt(BitWidth, Value));
  }

  virtual VASTImmediate *getOrCreateImmediate(const APInt &Value);

  virtual VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                                unsigned UB, unsigned LB);

  virtual void replaceAllUseWith(VASTValPtr From, VASTValPtr To);
};

// The helper class to collect the information about the operands of a VASTExpr.
template<VASTExpr::Opcode Opcode>
struct VASTExprOpInfo {
  VASTExprOpInfo() {}

  VASTValPtr analyzeOperand(VASTValPtr V) {
    // Do nothing by default.
    return V;
  }
};

class VASTExprBuilder {
  void operator=(const VASTExprBuilder &RHS); // DO NOT IMPLEMENT
  VASTExprBuilder(const VASTExprBuilder &RHS); // DO NOT IMPLEMENT

  VASTValPtr foldBitSliceExpr(VASTValPtr U, uint8_t UB, uint8_t LB);

  // Inline all operands in the expression whose Opcode is the same as Opc
  // recursively;
  template<VASTExpr::Opcode Opcode, typename visitor>
  void flattenExpr(VASTValPtr V, visitor F) {
    if (VASTExpr *Expr = dyn_cast<VASTExpr>(V)) {
      typedef VASTExpr::op_iterator op_iterator;
      if (Expr->getOpcode() == Opcode && shouldExprBeFlatten(Expr)) {
        for (op_iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I)
          flattenExpr<Opcode>(I->getAsInlineOperand(), F);

        return;
      }
    }

    F++ = V;
  }

  template<VASTExpr::Opcode Opcode, typename iterator, typename visitor>
  void flattenExpr(iterator begin, iterator end, visitor F) {
    while (begin != end)
      flattenExpr<Opcode>(*begin++, F);
  }
  
  // The helper iterator class to collect all leaf operand of an expression tree.
  template<VASTExpr::Opcode Opcode, class _Container>
  struct op_filler_iterator : public std::iterator<std::output_iterator_tag,
                                                   void, void, void, void> {
    typedef op_filler_iterator<Opcode, _Container> Self;

    VASTExprBuilder &Builder;
    VASTExprOpInfo<Opcode> &OpInfo;
    _Container &C;
    explicit op_filler_iterator(_Container &C, VASTExprOpInfo<Opcode> &OpInfo,
                                VASTExprBuilder &Builder)
      : Builder(Builder), OpInfo(OpInfo), C(C) {}

    Self &operator=(VASTValPtr V) {
      if ((V = OpInfo.analyzeOperand(V)))
        C.push_back(V);

      return *this;
    }

    Self& operator*() {
      // pretend to return designated value
      return (*this);
    }

    Self& operator++() {
      // pretend to preincrement
      return (*this);
    }

    Self operator++(int) {
      // pretend to postincrement
      return (*this);
    }
  };

  template<VASTExpr::Opcode Opcode, class _Container>
  op_filler_iterator<Opcode, _Container> op_filler(_Container &C,
                                                   VASTExprOpInfo<Opcode> &Info)
  {
    return op_filler_iterator<Opcode, _Container>(C, Info, *this);
  }


  VASTValPtr padHeadOrTail(VASTValPtr V, unsigned BitWidth, bool ByOnes,
                           bool PadTail);

protected:
  VASTValPtr padHigherBits(VASTValPtr V, unsigned BitWidth, bool ByOnes) {
    return padHeadOrTail(V, BitWidth, ByOnes, false);
  }

  VASTValPtr padLowerBits(VASTValPtr V, unsigned BitWidth, bool ByOnes) {
    return padHeadOrTail(V, BitWidth, ByOnes, true);
  }

  VASTExprBuilderContext &Context;
public:
  explicit VASTExprBuilder(VASTExprBuilderContext &Context)
    : Context(Context) {}

  // Directly create the expression without optimizations.
  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned UB, unsigned LB = 0) {
    return Context.createExpr(Opc, Ops, UB, LB);
  }

  void replaceAllUseWith(VASTValPtr From, VASTValPtr To) {
    Context.replaceAllUseWith(From, To);
  }

  VASTValPtr createExpr(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS,
                        unsigned UB, unsigned LB = 0) {
    VASTValPtr Ops[] = { LHS, RHS };
    return createExpr(Opc, Ops, UB, LB);
  }

  // Bit mask analyzing, bitmask_collecting_iterator.
  void calculateBitMask(VASTValPtr V, APInt &KnownZeros, APInt &KnownOnes);
  void calculateBitCatBitMask(VASTExprPtr Expr, APInt &KnownZeros,
                              APInt &KnownOnes);
  static bool GetMaskSplitPoints(APInt Mask, unsigned &HiPt, unsigned &LoPt);

  VASTValPtr getBoolImmediate(bool Val) {
    return Context.getOrCreateImmediate(Val, 1);
  }

  VASTImmediate *getOrCreateImmediate(uint64_t Value, int8_t BitWidth) {
    return Context.getOrCreateImmediate(Value, BitWidth);
  }

  VASTImmediate *getOrCreateImmediate(const APInt &Value) {
    return Context.getOrCreateImmediate(Value);
  }

  bool shouldExprBeFlatten(VASTExpr *E) const {
    return Context.shouldExprBeFlatten(E);
  }

  VASTValPtr getOrCreateCommutativeExpr(VASTExpr::Opcode Opc,
                                        SmallVectorImpl<VASTValPtr> &Ops,
                                        unsigned BitWidth);

  VASTValPtr buildExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                       unsigned BitWidth);

  VASTValPtr buildExprByOpBitSlice(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                                   uint8_t UB, uint8_t LB) {
    SmallVector<VASTValPtr, 8> OpBitSlices;
    for (unsigned i = 0; i < Ops.size(); ++i)
      OpBitSlices.push_back(buildBitSliceExpr(Ops[i], UB, LB));

    return buildExpr(Opc, OpBitSlices, UB - LB);
  }

  VASTValPtr buildExpr(VASTExpr::Opcode Opc,VASTValPtr Op, unsigned BitWidth);
  VASTValPtr buildExpr(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS,
                       unsigned BitWidth);
  template<VASTExpr::Opcode Opc>
  static VASTValPtr buildExpr(VASTValPtr LHS, VASTValPtr RHS, unsigned BitWidth,
                              VASTExprBuilder *Builder) {
    return Builder->buildExpr(Opc, LHS, RHS, BitWidth);
  }

  VASTValPtr buildExpr(VASTExpr::Opcode Opc, VASTValPtr Op0, VASTValPtr Op1,
                       VASTValPtr Op2, unsigned BitWidth);
  VASTValPtr buildExpr(VASTExprHelper &Builder) {
    VASTValPtr V = buildExpr(Builder.Opc, Builder.Operands, Builder.BitWidth);

    // If opc is dpAnd and BuildNot is true. It mean Or in And Invert Graph.
    if (Builder.BuildNot) V = buildNotExpr(V);

    return V;
  }

  VASTValPtr buildBitSliceExpr(VASTValPtr U, uint8_t UB, uint8_t LB);

  VASTValPtr getSignBit(VASTValPtr V) {
    return buildBitSliceExpr(V, V->getBitWidth(), V->getBitWidth() - 1);
  }

  VASTValPtr buildBitCatExpr(ArrayRef<VASTValPtr> Ops, unsigned BitWidth);

  VASTValPtr buildAndExpr(ArrayRef<VASTValPtr> Ops, unsigned BitWidth);
  VASTValPtr buildAndExpr(VASTValPtr LHS, VASTValPtr RHS, unsigned BitWidth) {
    VASTValPtr Ops[] = { LHS, RHS };
    return buildAndExpr(Ops, BitWidth);
  }

  VASTValPtr buildSelExpr(VASTValPtr Cnd, VASTValPtr TrueV, VASTValPtr FalseV,
                          unsigned BitWidth);
  VASTValPtr buildMulExpr(ArrayRef<VASTValPtr> Ops, unsigned BitWidth);
  VASTValPtr buildAddExpr(ArrayRef<VASTValPtr> Ops, unsigned BitWidth);
  VASTValPtr buildShiftExpr(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS,
                            unsigned BitWidth);
  VASTValPtr buildReduction(VASTExpr::Opcode Opc, VASTValPtr Op);
  VASTValPtr buildROr(VASTValPtr V) {
    // A | B .. | Z = ~(~A & ~B ... & ~Z).
    return buildNotExpr(buildExpr(VASTExpr::dpRAnd, buildNotExpr(V), 1));
  }

  // For operator !=
  VASTValPtr buildNE(VASTValPtr LHS, VASTValPtr RHS) {
    unsigned Bitwidth = LHS->getBitWidth();
    VASTValPtr Ops[] = { LHS, RHS };
    // Get the bitwise difference by Xor.
    VASTValPtr BitWiseDiff = buildXorExpr(Ops, Bitwidth);
    // If there is any bitwise difference, then LHS and RHS is not equal.
    return buildROr(BitWiseDiff);
  }

  // For operator ==
  VASTValPtr buildEQ(VASTValPtr LHS, VASTValPtr RHS) {
    return buildNotExpr(buildNE(LHS, RHS));
  }

  VASTValPtr buildBitRepeat(VASTValPtr Op, unsigned RepeatTimes);

  VASTValPtr buildNotExpr(VASTValPtr U);

  static VASTValPtr buildOr(VASTValPtr LHS, VASTValPtr RHS, unsigned BitWidth,
                            VASTExprBuilder *Builder) {
    return Builder->buildOrExpr(LHS, RHS, BitWidth);
  }

  // Simulate the "|=" operator.
  VASTValPtr orEqual(VASTValPtr &LHS, VASTValPtr RHS) {
    if (!LHS)  LHS = RHS;
    else       LHS = buildOrExpr(LHS, RHS, RHS->getBitWidth());
    return LHS;
  }

  VASTValPtr buildOrExpr(ArrayRef<VASTValPtr> Ops, unsigned BitWidth);

  VASTValPtr buildOrExpr(VASTValPtr LHS, VASTValPtr RHS, unsigned BitWidth) {
    VASTValPtr Ops[] = { LHS, RHS };
    return buildOrExpr(Ops, BitWidth);
  }

  static VASTValPtr buildXor(VASTValPtr LHS, VASTValPtr RHS, unsigned BitWidth,
                             VASTExprBuilder *Builder) {
    VASTValPtr Ops[] = { LHS, RHS };
    return Builder->buildXorExpr(Ops, BitWidth);
  }

  VASTValPtr buildXorExpr(ArrayRef<VASTValPtr> Ops, unsigned BitWidth);

  VASTValPtr buildZExtExpr(VASTValPtr V, unsigned DstBitWidth);
  VASTValPtr buildSExtExpr(VASTValPtr V, unsigned DstBitWidth);

  VASTValPtr buildZExtExprOrSelf(VASTValPtr V, unsigned DstBitWidth) {
    if (V->getBitWidth() < DstBitWidth) V = buildZExtExpr(V, DstBitWidth);

    return V;
  }

  VASTValPtr buildSExtExprOrSelf(VASTValPtr V, unsigned DstBitWidth) {
    if (V->getBitWidth() < DstBitWidth) V = buildSExtExpr(V, DstBitWidth);

    return V;
  }
};
}

#endif
