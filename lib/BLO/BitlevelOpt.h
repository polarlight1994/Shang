//==------------- BitlevelOpt.h - Bit-level Optimization ----------*- C++ -*-=//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the BitLevelOpt pass.
// The BitLevelOpt pass perform the bit-level optimizations iteratively until
// the bit-level optimization do not optimize the Module any further.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_BIT_LEVEL_OPTIMIZATION_H
#define VAST_BIT_LEVEL_OPTIMIZATION_H

#include "vast/VASTExprBuilder.h"
#include "vast/VASTBitMask.h"

namespace vast {
using namespace llvm;

class DatapathBLO : private MinimalExprBuilderContext {
  VASTExprBuilder Builder;
  // Do not optimize the same expr twice.
  std::set<VASTExpr*> Visited;

public:
  // Allow user to access the builder to build expression.
  VASTExprBuilder *operator->() { return &Builder; }

  VASTValPtr optimizeExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                          unsigned BitWidth);

  // Propagate invert flag to the leave of a combinational cone if possible.
  VASTValPtr eliminateInvertFlag(VASTValPtr V);
  void eliminateInvertFlag(MutableArrayRef<VASTValPtr> Ops);

  // Construct !V, then try to optimize it.
  VASTValPtr optimizeNot(VASTValPtr V) {
    return eliminateInvertFlag(Builder.buildNotExpr(V));
  }

  template<typename T>
  VASTValPtr optimizeOR(ArrayRef<T> Ops, unsigned BitWidth) {
    assert(Ops.size() >= 1 && "There should be more than one operand!!");

    if (Ops.size() == 1)
      return Ops[0];

    SmallVector<VASTValPtr, 4> NotExprs;
    // Build the operands of Or operation into not Expr.
    for (unsigned i = 0; i < Ops.size(); ++i) {
      VASTValPtr V = optimizeNot(Ops[i]);
      NotExprs.push_back(V);
    }

    // Build Or operation with the And Inverter Graph (AIG).
    VASTValPtr V = optimizeAnd<VASTValPtr>(NotExprs, BitWidth);
    return optimizeNot(V);
  }

  template<typename T>
  VASTValPtr optimizeAnd(ArrayRef<T> Ops, unsigned BitWidth) {
    return optimizeNAryExpr<VASTExpr::dpAnd, T>(Ops, BitWidth);
  }

  template<typename T>
  VASTValPtr optimizedpBitCat(ArrayRef<T> Ops, unsigned BitWidth) {
    return optimizeNAryExpr<VASTExpr::dpBitCat, T>(Ops, BitWidth);
  }

  template<VASTExpr::Opcode Opcode, typename T>
  VASTValPtr optimizeNAryExpr(ArrayRef<T> Ops, unsigned BitWidth) {
    SmallVector<VASTValPtr, 8> FlattenOps;
    flattenExpr<Opcode, T>(FlattenOps, Ops);

    switch (Opcode) {
    default: break;
    case VASTExpr::dpBitCat:
      return optimizeBitCatImpl(FlattenOps, BitWidth);
    case VASTExpr::dpAnd:
    // Optimize the bitmask expression with the same function that optimize
    // AND
    case VASTExpr::dpBitMask:
      return optimizeAndImpl(FlattenOps, BitWidth);
    case VASTExpr::dpAdd:
      return optimizeAddImpl(FlattenOps, BitWidth);
    case VASTExpr::dpMul:
      return optimizeMulImpl(FlattenOps, BitWidth);
    }

    return Builder.buildExpr(Opcode, FlattenOps, BitWidth);
  }

  VASTValPtr optimizeBitCatImpl(MutableArrayRef<VASTValPtr> Ops,
                                unsigned BitWidth);
  VASTValPtr optimizeBitRepeat(VASTValPtr Pattern, unsigned Times);
  VASTValPtr optimizeBitExtract(VASTValPtr V, unsigned UB, unsigned LB);

  VASTValPtr optimizeAndImpl(MutableArrayRef<VASTValPtr>  Ops,
                             unsigned BitWidth);

  VASTValPtr optimizeReduction(VASTExpr::Opcode Opc, VASTValPtr Op);
  
  VASTValPtr optimizeAnnotation(VASTExpr::Opcode Opcode, VASTValPtr Op);

  VASTValPtr optimizeSignBit(VASTValPtr V) {
    return optimizeBitExtract(V, V->getBitWidth(), V->getBitWidth() - 1);
  }


  VASTValPtr optimizeAddImpl(MutableArrayRef<VASTValPtr>  Ops,
                             unsigned BitWidth);

  VASTValPtr optimizeMulImpl(MutableArrayRef<VASTValPtr>  Ops,
                             unsigned BitWidth);

  VASTValPtr optimizeCarryChain(VASTExpr::Opcode Opcode,
                                MutableArrayRef<VASTValPtr>  Ops,
                                unsigned BitWidth);

  VASTValPtr optimizeShift(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS,
                           unsigned BitWidth);

  VASTValPtr optimizeSGT(VASTValPtr LHS, VASTValPtr RHS);
  VASTValPtr optimizeUGT(VASTValPtr LHS, VASTValPtr RHS);
  VASTValPtr optimizeCmpWithConst(VASTExpr::Opcode Opcode, VASTValPtr X,
                                  const APInt &Const, bool VarAtLHS);

  template<VASTExpr::Opcode Opcode>
  static bool IsOpcodeEquivalent(VASTExpr::Opcode Other) {
    if (Opcode == Other)
      return true;

    if (Opcode == VASTExpr::dpAnd)
      return Other == VASTExpr::dpBitMask;

    if (Opcode == VASTExpr::dpBitMask)
      return Other == VASTExpr::dpAnd;

    return false;
  }

  template<VASTExpr::Opcode Opcode, typename T>
  void flattenExpr(SmallVectorImpl<VASTValPtr> &Dst, ArrayRef<T> Src) {
    for (unsigned i = 0; i < Src.size(); ++i) {
      // Try to remove the invert flag.
      VASTValPtr V = eliminateInvertFlag(Src[i]);
      if (!V.isInverted()) {
        if (VASTExpr *Expr = dyn_cast<VASTExpr>(V)) {
          // Flatten the expression tree with the same kind of opcode.
          if (IsOpcodeEquivalent<Opcode>(Expr->getOpcode())) {
            flattenExpr<Opcode, VASTUse>(Dst, Expr->getOperands());
            continue;
          }
        }
      }

      Dst.push_back(V);
    }
  }

  VASTValPtr optimizeExpr(VASTExpr *Expr);
  VASTValPtr replaceKnownBits(VASTValPtr V);
  bool replaceIfNotEqual(VASTValPtr From, VASTValPtr To);

  template<VASTExpr::Opcode Opcode>
  static void VerifyOpcode(VASTExpr *Expr) {
    assert(Expr->getOpcode() == Opcode && "Unexpected opcode!");
  }

  explicit DatapathBLO(DatapathContainer &Datapath);
  ~DatapathBLO();

  bool optimizeAndReplace(VASTValPtr V);

  bool optimizeSelector(VASTSelector *Sel);

  bool optimizeForward(VASTModule &VM);
  bool performLUTMapping();
  bool shrink(VASTModule &VM);

  void resetForNextIteration();
};
}
#endif
