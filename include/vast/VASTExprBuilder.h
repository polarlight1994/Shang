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

  virtual
  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                            unsigned Bitwidth);
  virtual
  VASTValPtr createBitExtract(VASTValPtr Op, unsigned UB, unsigned LB);
  virtual
  VASTValPtr createROMLookUp(VASTValPtr Addr, VASTMemoryBank *Bank,
                             unsigned BitWidth);

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
                        unsigned Bitwidth);
  VASTValPtr createBitExtract(VASTValPtr Op, unsigned UB, unsigned LB);
  VASTValPtr createROMLookUp(VASTValPtr Addr, VASTMemoryBank *Bank,
                             unsigned BitWidth);

  void replaceAllUseWith(VASTValPtr From, VASTValPtr To);
};

class VASTExprBuilder {
  void operator=(const VASTExprBuilder &RHS) LLVM_DELETED_FUNCTION;
  VASTExprBuilder(const VASTExprBuilder &RHS) LLVM_DELETED_FUNCTION;

protected:
  VASTExprBuilderContext &Context;

public:
  explicit VASTExprBuilder(VASTExprBuilderContext &Context)
    : Context(Context) {}

  void replaceAllUseWith(VASTValPtr From, VASTValPtr To) {
    Context.replaceAllUseWith(From, To);
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

  VASTValPtr buildBitExtractExpr(VASTValPtr U, unsigned UB, unsigned LB);

  VASTValPtr copyExpr(VASTExpr *Expr, ArrayRef<VASTValPtr> Ops);

  VASTValPtr getSignBit(VASTValPtr V) {
    return buildBitExtractExpr(V, V->getBitWidth(), V->getBitWidth() - 1);
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
  VASTValPtr buildROMLookUp(VASTValPtr Addr, VASTMemoryBank *Bank,
                            unsigned Bitwidth);
};
}

#endif
