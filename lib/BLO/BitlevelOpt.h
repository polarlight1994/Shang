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

class DatapathBLO : public MinimalExprBuilderContext {
  VASTExprBuilder Builder;
  // Do not optimize the same expr twice.
  std::set<VASTExpr*> Visited;

  VASTValPtr optimizeExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                          unsigned BitWidth);

  // Propagate invert flag to the leave of a combinational cone if possible.
  VASTValPtr eliminateInvertFlag(VASTValPtr V);
  VASTValPtr eliminateConstantInvertFlag(VASTValPtr V);
  void eliminateInvertFlag(MutableArrayRef<VASTValPtr> Ops);

  template<VASTExpr::Opcode Opcode, typename T>
  VASTValPtr optimizeNAryExpr(ArrayRef<T> Ops, unsigned BitWidth) {
    SmallVector<VASTValPtr, 8> FlattenOps;
    flattenExpr<Opcode, T>(FlattenOps, Ops);

    switch (Opcode) {
    default: break;
    case VASTExpr::dpBitCat:
      return optimizeBitCatImpl(FlattenOps, BitWidth);
    case VASTExpr::dpAnd:
      return optimizeAndImpl(FlattenOps, BitWidth);
    case VASTExpr::dpAdd:
      break;
    case VASTExpr::dpMul:
      break;
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
  
  VASTValPtr optimizeKeep(VASTValPtr Op);

  VASTValPtr optimizeSignBit(VASTValPtr V) {
    return optimizeBitExtract(V, V->getBitWidth(), V->getBitWidth() - 1);
  }
  VASTValPtr optimizeShift(VASTExpr::Opcode Opc, VASTValPtr LHS, VASTValPtr RHS,
                           unsigned BitWidth);

  template<VASTExpr::Opcode Opcode, typename T>
  void flattenExpr(SmallVectorImpl<VASTValPtr> &Dst, ArrayRef<T> Src) {
    for (unsigned i = 0; i < Src.size(); ++i) {
      // Try to remove the invert flag.
      VASTValPtr V = eliminateInvertFlag(Src[i]);
      if (!V.isInverted()) {
        if (VASTExpr *Expr = dyn_cast<VASTExpr>(V)) {
          // Flatten the expression tree with the same kind of opcode.
          if (Expr->getOpcode() == Opcode) {
            flattenExpr<Opcode, VASTUse>(Dst, Expr->getOperands());
            continue;
          }
        }
      }

      Dst.push_back(V);
    }
  }

  VASTValPtr optimizeExpr(VASTExpr *Expr);
  bool replaceIfNotEqual(VASTValPtr From, VASTValPtr To);

  template<VASTExpr::Opcode Opcode>
  static void VerifyOpcode(VASTExpr *Expr) {
    assert(Expr->getOpcode() == Opcode && "Unexpected opcode!");
  }

public:
  explicit DatapathBLO(DatapathContainer &Datapath);
  ~DatapathBLO();

  bool optimizeAndReplace(VASTValPtr V);

  void resetForNextIteration();
};
}
#endif
