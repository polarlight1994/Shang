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

#include "shang/VASTDatapathNodes.h"
#include "shang/VASTSeqValue.h"
#include "shang/Strash.h"

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

  enum ModelType {
    ZeroDelay, BlackBox, Bitlevel, External
  };

protected:
  CachedSequashTable *CachedSequash;
  PathDelayInfo &PathDelay;
  const ModelType T;

  explicit TimingEstimatorBase(PathDelayInfo &PathDelay, ModelType T,
                               CachedSequashTable *CachedSequash);

  virtual void accumulateExprDelay(VASTExpr *Expr, unsigned UB, unsigned LB) {}

  bool hasPathInfo(unsigned NodeID) const {
    return getPathTo(NodeID) != 0;
  }

  SrcDelayInfo *getPathTo(unsigned DstID) {
    path_iterator at = PathDelay.find(DstID);
    return at == PathDelay.end() ? 0 : &at->second;
  }

  const SrcDelayInfo *getPathTo(unsigned DstID) const {
    const_path_iterator at = PathDelay.find(DstID);
    return at == PathDelay.end() ? 0 : &at->second;
  }

  delay_type getDelayFrom(unsigned SrcID, const SrcDelayInfo &SrcInfo) const {
    const_src_iterator at = SrcInfo.find(SrcID);
    return at == SrcInfo.end() ? delay_type() : at->second;
  }
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
    if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(Thu)) {
      assert(!isa<VASTExpr>(Thu) && "Not SrcInfo from Src find!");
      delay_type D(0.0f);
      unsigned SrcID = CachedSequash->getOrCreateSequashID(SeqVal);
      updateDelay(CurInfo, F(Dst, ThuPos, DstUB, DstLB, SrcEntryTy(SrcID, D)));
      return true;
    }

    // Lookup the source of the timing path.
    unsigned ThuID = CachedSequash->getOrCreateSequashID(Thu);
    const SrcDelayInfo *SrcInfo = getPathTo(ThuID);

    if (SrcInfo == 0) return false;

    bool updated = false;
    assert(!SrcInfo->empty() && "Unexpected empty source delay info!");
    for (src_iterator I = SrcInfo->begin(), E = SrcInfo->end(); I != E; ++I)
      updateDelay(CurInfo, F(Dst, ThuPos, DstUB, DstLB, *I));

    // FIXME: Also add the delay from Src to Dst.
    if (VASTOperandList::GetOperandList(Thu) && hasPathInfo(ThuID)) {
      delay_type D(0.0f);
      updateDelay(CurInfo, F(Dst, ThuPos, DstUB, DstLB, SrcEntryTy(ThuID, D)));
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
    unsigned DstID = CachedSequash->getOrCreateSequashID(Dst);
    SrcDelayInfo &SrcInfo = PathDelay[DstID];
    for (const_src_iterator I = CurInfo.begin(), E = CurInfo.end(); I != E; ++I) {
      TimingNetlist::delay_type &d = SrcInfo[I->first];
      d = std::max(d, I->second);
    }
  }

  void estimateTimingOnTree(VASTValue *Root);
};

template<typename SubClass>
class TimingEstimatorImpl : public TimingEstimatorBase {
protected:

  explicit TimingEstimatorImpl(PathDelayInfo &PathDelay, ModelType T,
                               CachedSequashTable *CachedSequash)
    : TimingEstimatorBase(PathDelay, T, CachedSequash) {}

public:

  void accumulateExprDelay(VASTExpr *Expr, unsigned UB, unsigned LB) {
    SrcDelayInfo CurSrcInfo;
    accumulateDelayTo(Expr, UB, LB, CurSrcInfo);
    // It looks like that all the operands are constant.
    if (CurSrcInfo.empty()) return;

    unsigned ExprID = CachedSequash->getOrCreateSequashID(Expr);
    bool inserted = PathDelay.insert(std::make_pair(ExprID, CurSrcInfo)).second;
    assert(inserted && "We are visiting the same Expr twice?");
    (void) inserted;
  }

  void accumulateDelayTo(VASTExpr *Expr, unsigned UB, unsigned LB,
                         SrcDelayInfo &CurSrcInfo) {
    SubClass *SCThis = reinterpret_cast<SubClass*>(this);

    typedef VASTExpr::const_op_iterator op_iterator;
    for (unsigned i = 0; i < Expr->size(); ++i) {
      VASTValue *Op = Expr->getOperand(i).getAsLValue<VASTValue>();

      // Do nothing if we are using the zero delay-model.
      if (T == TimingEstimatorBase::ZeroDelay) {
        accumulateDelayThu(Op, Expr, i, UB, LB, CurSrcInfo,
                           AccumulateZeroDelay);
        continue;
      }

      switch (Expr->getOpcode()) {
      case VASTExpr::dpLUT:
        SCThis->accumulateDelayThuLUT(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpAnd:
        SCThis->accumulateDelayThuAnd(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpRAnd:
        SCThis->accumulateDelayThuRAnd(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpRXor:
        SCThis->accumulateDelayThuRXor(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpSGT:
      case VASTExpr::dpUGT:
        SCThis->accumulateDelayThuCmp(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpAdd:
        SCThis->accumulateDelayThuAdd(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpMul:
        SCThis->accumulateDelayThuMul(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpShl:
        SCThis->accumulateDelayThuShl(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpSRL:
        SCThis->accumulateDelayThuSRL(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpSRA:
        SCThis->accumulateDelayThuSRA(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
        break;
      case VASTExpr::dpAssign:
        SCThis->accumulateDelayThuAssign(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpBitCat:
        SCThis->accumulateDelayThuBitCat(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      case VASTExpr::dpBitRepeat:
        SCThis->accumulateDelayBitRepeat(Op, Expr, i, UB, LB, CurSrcInfo);
        break;
      default: llvm_unreachable("Unknown datapath opcode!"); break;
      }
    }
  }

  delay_type getPathDelay(VASTValue *From, VASTValue *To) {
    const SrcDelayInfo *SrcInfo = getPathTo(To);
    assert(SrcInfo && "SrcInfo not available!");
    return getDelayFrom(From, *SrcInfo);
  }
};

// Estimate the bit-level delay with linear approximation.
class BitlevelDelayEsitmator : public TimingEstimatorImpl<BitlevelDelayEsitmator> {
  typedef TimingEstimatorImpl<BitlevelDelayEsitmator> Base;
  typedef Base::delay_type delay_type;

  static SrcEntryTy AccumulateLUTDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc);
  static SrcEntryTy AccumulateAndDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc);
  static SrcEntryTy AccumulateRedDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc);

  static SrcEntryTy AccumulateCmpDelay(VASTValue *Dst, unsigned SrcPos,
                                       uint8_t DstUB, uint8_t DstLB,
                                       const SrcEntryTy &DelayFromSrc);

  template<typename VFUTy>
  static SrcEntryTy AccumulateAddMulDelay(VASTValue *Dst, unsigned SrcPos,
                                          uint8_t DstUB, uint8_t DstLB,
                                          const SrcEntryTy &DelayFromSrc) {
    delay_type D = DelayFromSrc.second;
    VFUTy *FU = getFUDesc<VFUTy>();
    float MSBLatency = FU->lookupLatency(DstUB);
    float LSBLatency = FU->lookupLatency(DstLB);
    delay_type Inc(std::max(MSBLatency, LSBLatency));
    return SrcEntryTy(DelayFromSrc.first, D + Inc);
  }

  template<typename VFUTy>
  static SrcEntryTy AccumulateBlackBoxDelay(VASTValue *Dst, unsigned SrcPos,
                                            uint8_t DstUB, uint8_t DstLB,
                                            const SrcEntryTy &DelayFromSrc) {
    delay_type D = DelayFromSrc.second;
    unsigned FUWidth = Dst->getBitWidth();
    VFUTy *FU = getFUDesc<VFUTy>();
    float Latency = FU->lookupLatency(FUWidth);
    delay_type Inc(Latency);
    return SrcEntryTy(DelayFromSrc.first, D + Inc);
  }

public:
  BitlevelDelayEsitmator(PathDelayInfo &PathDelay, ModelType T,
                         CachedSequashTable *CachedSequash)
    : Base(PathDelay, T, CachedSequash) {}

  void accumulateDelayThuLUT(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo, AccumulateLUTDelay);
  }

  void accumulateDelayThuAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo, AccumulateAndDelay);
  }

  void accumulateDelayThuRAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo, AccumulateRedDelay);
  }

  void accumulateDelayThuRXor(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo, AccumulateRedDelay);
  }

  void accumulateDelayThuCmp(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo, AccumulateCmpDelay);
  }

  void accumulateDelayThuAdd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo,
                       AccumulateAddMulDelay<VFUAddSub>);
  }

  void accumulateDelayThuMul(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo,
                       AccumulateAddMulDelay<VFUMult>);
  }

  void accumulateDelayThuShl(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo,
                       AccumulateBlackBoxDelay<VFUShift>);
  }

  void accumulateDelayThuSRL(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo,
                       AccumulateBlackBoxDelay<VFUShift>);
  }

  void accumulateDelayThuSRA(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo,
                       AccumulateBlackBoxDelay<VFUShift>);
  }

  void accumulateDelayThuAssign(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo);

  void accumulateDelayThuBitCat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo,
                       AccumulateZeroDelay);
  }

  void accumulateDelayBitRepeat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo,
                       AccumulateZeroDelay);
  }
};
}

#endif
