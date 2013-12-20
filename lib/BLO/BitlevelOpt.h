//==------------- BitlevelOpt.h - Bit-level Optimization ----------*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
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

namespace llvm {
struct BitMasks {
  APInt KnownZeros, KnownOnes;
  explicit BitMasks(unsigned Size)
    : KnownZeros(APInt::getNullValue(Size)),
      KnownOnes(APInt::getNullValue(Size))
  {}

  BitMasks(APInt KnownZeros = APInt(), APInt KnownOnes = APInt())
    : KnownZeros(KnownZeros), KnownOnes(KnownOnes) {
    assert(KnownOnes.getBitWidth() == KnownZeros.getBitWidth()
      && "Bitwidths are not agreed!");
  }

  APInt getKnownBits() const;
  // Return true if the known bits in the current mask is a subset of the known
  // bits in RHS.
  bool isSubSetOf(const BitMasks &RHS) const;

  void dump() const;
};

class BitMaskContext {
protected:
  typedef std::map<VASTValue*, BitMasks> BitMaskCacheTy;
  BitMaskCacheTy BitMaskCache;
  // Simple bit mask calculation functions.
  BitMasks calculateBitCatBitMask(VASTExpr *Expr);
  BitMasks calculateAssignBitMask(VASTExpr *Expr);
  BitMasks calculateAndBitMask(VASTExpr *Expr);
  BitMasks calculateConstantBitMask(VASTConstant *C);
public:
  inline BitMasks setBitMask(VASTValue *V, const BitMasks &Masks) {
    std::pair<BitMaskCacheTy::iterator, bool> Pair
      = BitMaskCache.insert(std::make_pair(V, Masks));

    if (!Pair.second)
      Pair.first->second = Masks;

    return Masks;
  }

  // Bit mask analyzing, bitmask_collecting_iterator.
  BitMasks calculateBitMask(VASTValue *V);
  BitMasks calculateBitMask(VASTValPtr V);
};

class DatapathBLO : public MinimalExprBuilderContext, public BitMaskContext {
  VASTExprBuilder Builder;
  // Do not optimize the same expr twice.
  std::set<VASTExpr*> Visited;

  VASTValPtr optimizeExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                          unsigned BitWidth);

  // Propagate invert flag to the leave of a combinational cone if possible.
  VASTValPtr propagateInvertFlag(VASTValPtr V);
  VASTValPtr eliminateConstantInvertFlag(VASTValPtr V);

  VASTValPtr optimizeBitCat(ArrayRef<VASTValPtr> Ops, unsigned Bitwidth);
  VASTValPtr optimizeBitRepeat(VASTValPtr Pattern, unsigned Times);
  VASTValPtr optimizeAssign(VASTValPtr V, unsigned UB, unsigned LB);

  VASTValPtr optimizeExpr(VASTExpr *Expr);
  bool replaceIfNotEqual(VASTValPtr From, VASTValPtr To);
  // Override some hook for the ExprBUuilder
  virtual void deleteContenxt(VASTValue *V);

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
