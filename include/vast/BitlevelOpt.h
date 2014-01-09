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

  using MinimalExprBuilderContext::replaceUseOf;

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

  template<typename T>
  VASTValPtr optimizeAdd(ArrayRef<T> Ops, unsigned BitWidth) {
    return optimizeNAryExpr<VASTExpr::dpAdd, T>(Ops, BitWidth);
  }

  template<typename T>
  VASTValPtr optimizeMul(ArrayRef<T> Ops, unsigned BitWidth) {
    return optimizeNAryExpr<VASTExpr::dpMul, T>(Ops, BitWidth);
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

  // Optimizations for bitwise operations, bit manipulations
  VASTValPtr optimizeBitCatImpl(MutableArrayRef<VASTValPtr> Ops,
                                unsigned BitWidth);
  VASTValPtr optimizeBitRepeat(VASTValPtr Pattern, unsigned Times);
  VASTValPtr optimizeBitExtract(VASTValPtr V, unsigned UB, unsigned LB);

  VASTValPtr optimizeAndImpl(MutableArrayRef<VASTValPtr>  Ops,
                             unsigned BitWidth);

  // Optimize the and expressions with patial known bits in its operand.
  VASTValPtr optimizeAndPatialKnowns(MutableArrayRef<VASTValPtr>  Ops,
                                     unsigned BitWidth);

  VASTValPtr optimizeRAnd(VASTValPtr Op);
  
  VASTValPtr optimizeAnnotation(VASTExpr::Opcode Opcode, VASTValPtr Op);

  VASTValPtr optimizeSignBit(VASTValPtr V) {
    return optimizeBitExtract(V, V->getBitWidth(), V->getBitWidth() - 1);
  }

  VASTValPtr optimizeShift(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS,
                           unsigned BitWidth);

  // Optimizations for carry chain related operations.
  VASTValPtr optimizeAddImpl(MutableArrayRef<VASTValPtr>  Ops,
                             unsigned BitWidth);

  VASTValPtr optimizeMulImpl(MutableArrayRef<VASTValPtr>  Ops,
                             unsigned BitWidth);

  VASTValPtr optimizeMulWithConst(MutableArrayRef<VASTValPtr>  Ops,
                                  APInt C, unsigned BitWidth);

  VASTValPtr optimizeCarryChain(VASTExpr::Opcode Opcode,
                                MutableArrayRef<VASTValPtr>  Ops,
                                unsigned BitWidth);

  // Optimize the comparisons.
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

  template<typename T>
  void extractOperandKnownBits(SmallVectorImpl<VASTValPtr> &Dst, ArrayRef<T> Src,
                               bool FineGrain) {
    for (unsigned i = 0; i < Src.size(); ++i)
      Dst.push_back(replaceKnownBits(Src[i], FineGrain));
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

  // Knwon bits replacement
  VASTValPtr replaceKnownAllBits(VASTValPtr V);
  VASTValPtr replaceKnownBitsFromMask(VASTValPtr V, VASTBitMask Mask,
                                      bool FineGrain);
  VASTValPtr replaceKnownBits(VASTValPtr V, bool FineGrain);

  // Extract the bit position to split the knwon bits and unknwon bits.
  static
  void extractSplitPositions(APInt Mask, SmallVectorImpl<unsigned> &SplitPos);

  template<VASTExpr::Opcode Opcode, typename T>
  VASTValPtr splitAndConCat(ArrayRef<T> Ops, ArrayRef<unsigned> SplitPos) {
    unsigned NumSegments = SplitPos.size() - 1;
    SmallVector<VASTValPtr, 8> Bits(NumSegments, None);
    unsigned LB = SplitPos[0];

    for (unsigned i = 0; i < NumSegments; ++i) {
      unsigned UB = SplitPos[i + 1];

      SmallVector<VASTValPtr, 8> Operands;

      // Build the And for the current segment.
      for (unsigned j = 0, e = Ops.size(); j < e; ++j) {
        VASTValPtr V = Ops[j];
        VASTBitMask CurMask = V;

        if (CurMask.isAllBitKnown(UB, LB))
          Operands.push_back(getConstant(CurMask.getKnownValues(UB, LB)));
        else
          Operands.push_back(optimizeBitExtract(V, UB, LB));
      }

      // Put the segments from MSB to LSB, which is required by the BitCat
      // expression.
      Bits[NumSegments - i - 1]
        = optimizeNAryExpr<Opcode, VASTValPtr>(Operands, UB - LB);

      LB = UB;
    }

    return optimizedpBitCat<VASTValPtr>(Bits, SplitPos.back());
  }

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

  //===--------------------------------------------------------------------===//
  static bool isMask(APInt Value);
  static bool isShiftedMask(APInt Value);
  static bool hasEnoughKnownbits(APInt KnownBits, bool FineGrain);
};
}
#endif
