//=- LATimingEstimator.h-Estimate Delay with Linear Approximation -*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file datapath define the delay estimator based on linear approximation.
//
//===----------------------------------------------------------------------===//

#ifndef TIMING_ESTIMATOR_LINEAR_APPROXIMATION_H
#define TIMING_ESTIMATOR_LINEAR_APPROXIMATION_H

#include "TimingNetlist.h"
#include "vast/VASTDatapathNodes.h"
#include "vast/VASTSeqValue.h"
#include "vast/Utilities.h"

#include "llvm/Support/ErrorHandling.h"

namespace llvm {
class VASTValue;
class VASTExpr;

/// TimingEstimatorBase - Calculate the datapath delay and fill the PathDelay.
class TimingEstimatorBase {
public:
  typedef TimingNetlist::delay_type delay_type;
  typedef TimingNetlist::SrcDelayInfo SrcDelayInfo;
  typedef TimingNetlist::SrcEntryTy SrcEntryTy;
  typedef TimingNetlist::src_iterator src_iterator;
  typedef TimingNetlist::const_src_iterator const_src_iterator;

  typedef TimingNetlist::PathDelayInfo PathDelayInfo;
  typedef TimingNetlist::PathTy PathTy;
  typedef TimingNetlist::path_iterator path_iterator;
  typedef TimingNetlist::const_path_iterator const_path_iterator;

  typedef TimingNetlist::ModelType ModelType;
protected:
  PathDelayInfo &PathDelay;
  const ModelType T;

  explicit TimingEstimatorBase(PathDelayInfo &PathDelay, ModelType T);

  virtual void accumulateExprDelay(VASTExpr *Expr, unsigned UB, unsigned LB) {}

  bool hasPathInfo(VASTValue *V) const {
    return getPathTo(V) != 0;
  }

  SrcDelayInfo *getPathTo(VASTValue *Dst) {
    path_iterator at = PathDelay.find(Dst);
    return at == PathDelay.end() ? 0 : &at->second;
  }

  const SrcDelayInfo *getPathTo(VASTValue *Dst) const {
    const_path_iterator at = PathDelay.find(Dst);
    return at == PathDelay.end() ? 0 : &at->second;
  }

  delay_type getDelayFrom(VASTSeqValue *Src, const SrcDelayInfo &SrcInfo) const {
    const_src_iterator at = SrcInfo.find(Src);
    return at == SrcInfo.end() ? delay_type() : at->second;
  }

  VASTExpr *getAsUnvisitedExpr(VASTValue *V) const;
public:
  virtual ~TimingEstimatorBase() {}

  void updateDelay(SrcDelayInfo &Info, SrcEntryTy NewValue) {
    delay_type &OldDelay = Info[NewValue.first];
    OldDelay = std::max(OldDelay, NewValue.second);
  }

  // For trivial expressions, the delay is zero.
  static SrcEntryTy AccumulateZeroDelay(VASTValue *Dst, unsigned SrcPos,
                                        uint8_t DstUB, uint8_t DstLB,
                                        const SrcEntryTy &DelayFromSrc) {
    return DelayFromSrc;
  }

  //template<typename DelayAccumulatorTy>
  //void accumulateDelayThu(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
  //                        SrcDelayInfo &CurInfo, DelayAccumulatorTy F) {
  //  accumulateDelayThu<DelayAccumulatorTy>(Thu, Dst, ThuPos, Dst->getBitWidth(),
  //                                         0, CurInfo, F);
  //}

  // Take DelayAccumulatorTy to accumulate the design.
  // The signature of DelayAccumulatorTy should be:
  // SrcEntryTy DelayAccumulatorTy(VASTValue *Dst, unsign SrcPos,
  //                               uint8_t DstUB, uint8_t DstLB,
  //                               SrcEntryTy DelayFromSrc)
  template<typename DelayAccumulatorTy>
  bool accumulateDelayThu(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                          uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo,
                          DelayAccumulatorTy F) {
    // Do not lookup the source across the SeqValue.
    if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(Thu)) {
      delay_type D(0.0f);
      updateDelay(CurInfo, F(Dst, ThuPos, DstUB, DstLB, SrcEntryTy(SV, D)));

      if (isChainingCandidate(SV->getLLVMValue()) && SV->num_fanins() == 1) {
        const VASTLatch &L = SV->getUniqueFanin();
        accumulateDelayThu<DelayAccumulatorTy>(VASTValPtr(L).get(), Dst, ThuPos,
                                               DstUB, DstLB, CurInfo, F);
        accumulateDelayThu<DelayAccumulatorTy>(VASTValPtr(L.getGuard()).get(),
                                               Dst, ThuPos, DstUB, DstLB,
                                               CurInfo, F);
      }

      return true;
    }

    // Lookup the source of the timing path.
    const SrcDelayInfo *SrcInfo = getPathTo(Thu);

    if (SrcInfo == 0) return false;

    bool updated = false;
    assert(!SrcInfo->empty() && "Unexpected empty source delay info!");
    for (src_iterator I = SrcInfo->begin(), E = SrcInfo->end(); I != E; ++I)
      updateDelay(CurInfo, F(Dst, ThuPos, DstUB, DstLB, *I));

    // FIXME: Also add the delay from Src to Dst.
    if (isa<VASTExpr>(Thu) && hasPathInfo(Thu)) {
      delay_type D(0.0f);
      updateDelay(CurInfo, F(Dst, ThuPos, DstUB, DstLB, SrcEntryTy(Thu, D)));
      updated = true;
    }

    return updated;
  }

  void accumulateDelayFrom(VASTValue *Src, VASTValue *Dst) {
    SrcDelayInfo CurInfo;
    bool updated = accumulateDelayThu(Src, Dst, 0, Dst->getBitWidth(), 0,
                                      CurInfo, AccumulateZeroDelay);
    (void) updated;

    if (CurInfo.empty()) {
      assert(!updated && "Unexpected empty source!");
      return;
    }

    // Annotate the arrival time information to the matrix.
    SrcDelayInfo &SrcInfo = PathDelay[Dst];
    for (const_src_iterator I = CurInfo.begin(), E = CurInfo.end(); I != E; ++I) {
      TimingNetlist::delay_type &d = SrcInfo[I->first];
      d = std::max(d, I->second);
    }
  }

  void estimateTimingOnCone(VASTExpr *Root);
};

template<typename SubClass>
class TimingEstimatorImpl : public TimingEstimatorBase {
protected:

  explicit TimingEstimatorImpl(PathDelayInfo &PathDelay, ModelType T)
    : TimingEstimatorBase(PathDelay, T) {}

public:

  void accumulateExprDelay(VASTExpr *Expr, unsigned UB, unsigned LB) {
    SrcDelayInfo CurSrcInfo;
    accumulateDelayTo(Expr, UB, LB, CurSrcInfo);
    // It looks like that all the operands are constant.
    if (CurSrcInfo.empty()) return;

    bool inserted = PathDelay.insert(std::make_pair(Expr, CurSrcInfo)).second;
    assert(inserted && "We are visiting the same Expr twice?");
    (void) inserted;
  }

  void accumulateDelayThuAssign(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                uint8_t DstUB, uint8_t DstLB,
                                SrcDelayInfo &CurInfo) {
    VASTExpr *BitSliceExpr = cast<VASTExpr>(Dst);
    // Translate the (UB, LB] against the bitslice to the (UB, LB] against the
    // Src value.
    uint8_t UB = DstUB + BitSliceExpr->LB, LB = DstLB + BitSliceExpr->LB;
    assert(LB >= BitSliceExpr->LB && UB <= BitSliceExpr->UB && "Bad bitslice!");

    // Handle the trivial case trivially.
    if (VASTExpr *ThuExpr = dyn_cast<VASTExpr>(Thu)) {
      // Accumulate the scaled delay of ThuExpr to the current bitslice expression.
      accumulateDelayTo(ThuExpr, UB, LB, CurInfo);
    }

    // Build the delay from Thu to Dst.
    accumulateDelayThu(Thu, Dst, ThuPos, UB, LB, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayTo(VASTExpr *Expr, unsigned UB, unsigned LB,
                         SrcDelayInfo &CurSrcInfo) {
    typedef VASTExpr::const_op_iterator op_iterator;
    for (unsigned i = 0; i < Expr->size(); ++i) {
      VASTValue *Op = Expr->getOperand(i).getAsLValue<VASTValue>();

      // Do nothing if we are using the zero delay-model.
      if (T == TimingNetlist::ZeroDelay) {
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           AccumulateZeroDelay);
        continue;
      }

      switch (Expr->getOpcode()) {
      case VASTExpr::dpLUT:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           SubClass::AccumulateLUTDelay);
        break;
      case VASTExpr::dpCROM:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           SubClass::AccumulateCROMDelay);
        break;
      case VASTExpr::dpAnd:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           SubClass::AccumulateAndDelay);
        break;
      case VASTExpr::dpRXor:
      case VASTExpr::dpRAnd:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           SubClass::AccumulateRedDelay);
        break;
      case VASTExpr::dpSGT:
      case VASTExpr::dpUGT:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           SubClass::AccumulateCmpDelay);
        break;
      case VASTExpr::dpAdd:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           SubClass::AccumulateAddDelay);
        break;
      case VASTExpr::dpMul:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           SubClass::AccumulateMulDelay);
        break;
      case VASTExpr::dpShl:
      case VASTExpr::dpSRL:
      case VASTExpr::dpSRA:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           SubClass::AccumulateShiftDelay);
        break;
        break;
      case VASTExpr::dpAssign:
        accumulateDelayThuAssign(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpBitCat:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           SubClass::AccumulateBitCatDelay);
        break;
      case VASTExpr::dpBitRepeat:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           SubClass::AccumulateBitRepeatDelay);
        break;
      case VASTExpr::dpKeep:
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           AccumulateZeroDelay);
        break;
      default: llvm_unreachable("Unknown datapath opcode!"); break;
      }
    }
  }
};

// The timing estimator based on the black box model.
class BlackBoxDelayEsitmator : public TimingEstimatorImpl<BlackBoxDelayEsitmator> {
  typedef TimingEstimatorImpl<BlackBoxDelayEsitmator> Base;
  typedef Base::delay_type delay_type;

public:
  static SrcEntryTy AccumulateLUTDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc);

  static SrcEntryTy AccumulateAndDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc);

  static SrcEntryTy AccumulateCROMDelay(VASTValue *Dst, unsigned SrcPos,
                                        uint8_t DstUB, uint8_t DstLB,
                                        const SrcEntryTy &DelayFromSrc);

  static SrcEntryTy AccumulateRedDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc);

  static SrcEntryTy AccumulateCmpDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc);

  template<typename VFUTy>
  static SrcEntryTy AccumulateDelay(VASTValue *Dst, unsigned SrcPos,
                                    uint8_t DstUB, uint8_t DstLB,
                                    const SrcEntryTy &DelayFromSrc) {
    delay_type D = DelayFromSrc.second;
    unsigned FUWidth = std::min(Dst->getBitWidth(), 64u);
    VFUTy *FU = getFUDesc<VFUTy>();
    float Latency = FU->lookupLatency(FUWidth);
    delay_type Inc(Latency);
    return SrcEntryTy(DelayFromSrc.first, D + Inc);
  }

  static SrcEntryTy AccumulateAddDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc) {
    return AccumulateDelay<VFUAddSub>(Dst, SrcPos, DstUB, DstLB, DelayFromSrc);
  }

  static SrcEntryTy AccumulateMulDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc) {
    return AccumulateDelay<VFUMult>(Dst, SrcPos, DstUB, DstLB, DelayFromSrc);
  }

  static SrcEntryTy AccumulateShiftDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc) {
    return AccumulateDelay<VFUShift>(Dst, SrcPos, DstUB, DstLB, DelayFromSrc);
  }

  static SrcEntryTy AccumulateBitCatDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc) {
    return AccumulateZeroDelay(Dst, SrcPos, DstUB, DstLB, DelayFromSrc);
  }
  
  static SrcEntryTy AccumulateBitRepeatDelay(VASTValue *Dst, unsigned SrcPos,
                                             uint8_t DstUB, uint8_t DstLB,
                                             const SrcEntryTy &DelayFromSrc) {
    return AccumulateZeroDelay(Dst, SrcPos, DstUB, DstLB, DelayFromSrc);
  }

  BlackBoxDelayEsitmator(PathDelayInfo &PathDelay)
    : Base(PathDelay, TimingNetlist::BlackBox) {}
};
}

#endif
