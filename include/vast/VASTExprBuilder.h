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

#include "vast/VASTDatapathNodes.h"
#include "llvm/Support/Allocator.h"

namespace llvm {
class VASTExprBuilderContext {
public:
  virtual ~VASTExprBuilderContext() {}

  VASTConstant *getConstant(uint64_t Value, int8_t BitWidth) {
    return getConstant(APInt(BitWidth, Value));
  }

  virtual VASTConstant *getConstant(const APInt &Value);

  virtual VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                                unsigned UB, unsigned LB);

  virtual void replaceAllUseWith(VASTValPtr From, VASTValPtr To);

  virtual void deleteContenxt(VASTValue *V);
};

class DatapathContainer;
class DataLayout;

class MinimalExprBuilderContext : public VASTExprBuilderContext {
protected:
  DatapathContainer &Datapath;

public:
  explicit MinimalExprBuilderContext(DatapathContainer &Datapath);
  virtual ~MinimalExprBuilderContext();

  using VASTExprBuilderContext::getConstant;

  VASTConstant *getConstant(const APInt &Value);

  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned UB, unsigned LB);

  void replaceAllUseWith(VASTValPtr From, VASTValPtr To);
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
  void operator=(const VASTExprBuilder &RHS) LLVM_DELETED_FUNCTION;
  VASTExprBuilder(const VASTExprBuilder &RHS) LLVM_DELETED_FUNCTION;

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

  VASTValPtr getBoolConstant(bool Val) {
    return Context.getConstant(Val, 1);
  }

  VASTConstant *getConstant(uint64_t Value, int8_t BitWidth) {
    return Context.getConstant(Value, BitWidth);
  }

  VASTConstant *getConstant(const APInt &Value) {
    return Context.getConstant(Value);
  }

  VASTValPtr buildCommutativeExpr(VASTExpr::Opcode Opc,
                                  MutableArrayRef<VASTValPtr> Ops,
                                  unsigned BitWidth);

  VASTValPtr buildExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                       unsigned BitWidth);

  VASTValPtr buildExpr(VASTExpr::Opcode Opc,VASTValPtr Op, unsigned BitWidth);
  VASTValPtr buildExpr(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS,
                       unsigned BitWidth);

  VASTValPtr buildExpr(VASTExpr::Opcode Opc, VASTValPtr Op0, VASTValPtr Op1,
                       VASTValPtr Op2, unsigned BitWidth);

  VASTValPtr buildBitSliceExpr(VASTValPtr U, uint8_t UB, uint8_t LB);

  VASTValPtr buildExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                       uint8_t UB, uint8_t LB) {
    switch (Opc) {
    default: break;
    case VASTExpr::dpAssign:
      assert(Ops.size() == 1 && "Wrong operand number!");
      return buildBitSliceExpr(Ops[0], UB, LB);
    case VASTExpr::dpBitRepeat:
      assert(Ops.size() == 1 && "Wrong operand number!");
      return buildBitRepeat(Ops[0], UB - LB);
    }

    return buildExpr(Opc, Ops, UB - LB);
  }

  VASTValPtr copyExpr(VASTExpr *Expr, ArrayRef<VASTValPtr> Ops);

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

  VASTValPtr buildMulExpr(VASTValPtr LHS, VASTValPtr RHS, unsigned BitWidth) {
    VASTValPtr Ops[] = { LHS, RHS };
    return buildMulExpr(Ops, BitWidth);
  }
  VASTValPtr buildMulExpr(ArrayRef<VASTValPtr> Ops, unsigned BitWidth);

  VASTValPtr buildAddExpr(VASTValPtr LHS, VASTValPtr RHS, unsigned BitWidth) {
    VASTValPtr Ops[] = { LHS, RHS };
    return buildAddExpr(Ops, BitWidth);
  }
  VASTValPtr buildAddExpr(ArrayRef<VASTValPtr> Ops, unsigned BitWidth);
  // -A = 0 - A = ~A + 1
  VASTValPtr buildNegative(VASTValPtr Op);

  VASTValPtr buildShiftExpr(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS,
                            unsigned BitWidth);
  VASTValPtr buildReduction(VASTExpr::Opcode Opc, VASTValPtr Op);
  VASTValPtr buildROr(VASTValPtr V) {
    // A | B .. | Z = ~(~A & ~B ... & ~Z).
    return buildNotExpr(buildExpr(VASTExpr::dpRAnd, buildNotExpr(V), 1));
  }

  VASTValPtr buildICmpExpr(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS);
  VASTValPtr buildICmpOrEqExpr(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS);

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

  // Simulate the "|=" operator.
  VASTValPtr orEqual(VASTValPtr &LHS, VASTValPtr RHS) {
    if (LHS == None)
      return (LHS = RHS);

    return (LHS = buildOrExpr(LHS, RHS, RHS->getBitWidth()));
  }

  // Simulate the "&=" operator.
  VASTValPtr andEqual(VASTValPtr &LHS, VASTValPtr RHS) {
    if (LHS == None)
      return (LHS = RHS);

    return (LHS = buildAndExpr(LHS, RHS, RHS->getBitWidth()));
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
  VASTValPtr buildXorExpr(VASTValPtr LHS, VASTValPtr RHS, unsigned BitWidth) {
    VASTValPtr Ops[] = { LHS, RHS };
    return buildXorExpr(Ops, BitWidth);
  }

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

  VASTValPtr buildKeep(VASTValPtr V);
  VASTValPtr buildCROM(VASTValPtr Addr, VASTValPtr Table, unsigned Bitwidth);
};
}

#endif
