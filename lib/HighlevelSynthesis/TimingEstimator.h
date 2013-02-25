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

#include "llvm/Support/ErrorHandling.h"

namespace llvm {
class VASTValue;
class VASTExpr;

/// TimingEstimatorBase - Calculate the datapath delay and fill the PathDelay.
class TimingEstimatorBase {
public:
  typedef TNLDelay delay_type;
  typedef TimingNetlist::SrcDelayInfo SrcDelayInfo;
  typedef TimingNetlist::SrcEntryTy SrcEntryTy;
  typedef TimingNetlist::src_iterator src_iterator;
  typedef TimingNetlist::const_src_iterator const_src_iterator;

  typedef TimingNetlist::PathDelayInfo PathDelayInfo;
  typedef TimingNetlist::PathTy PathTy;
  typedef TimingNetlist::path_iterator path_iterator;
  typedef TimingNetlist::const_path_iterator const_path_iterator;

  enum ModelType {
    ZeroDelay, BlackBox, Bitlevel
  };

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
    return at == SrcInfo.end() ? delay_type(0) : at->second;
  }
public:
  virtual ~TimingEstimatorBase() {}

  void annotateDelay(VASTValue *Dst, SrcDelayInfo &SrcInfo) const {
    const SrcDelayInfo *Srcs = getPathTo(Dst);
    // It maybe not path to Dst.
    if (Srcs == 0) return;

    for (const_src_iterator I = Srcs->begin(), E = Srcs->end(); I != E; ++I) {
      TimingNetlist::delay_type &d = SrcInfo[I->first];
      d = TNLDelay::max(d, I->second);
    }
  }

  void updateDelay(SrcDelayInfo &Info, SrcEntryTy NewValue) {
    // If we force the BlackBox Module, we synchronize the MSB_LL and LSB_LL
    // every time before we put it into the path delay table.
    if (T == BlackBox) NewValue.second.syncLL();

    delay_type &OldDelay = Info[NewValue.first];
    OldDelay = TNLDelay::max(OldDelay, NewValue.second);
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
  void accumulateDelayThu(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                          uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo,
                          DelayAccumulatorTy F) {
    // Do not lookup the source across the SeqValue.
    if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(Thu)) {
      assert(!isa<VASTExpr>(Thu) && "Not SrcInfo from Src find!");
      unsigned BitWidth = SeqVal->getBitWidth();
      delay_type D(0, 0);
      updateDelay(CurInfo, F(Dst, ThuPos, DstUB, DstLB, SrcEntryTy(SeqVal, D)));
      return;
    }

    // Lookup the source of the timing path.
    const SrcDelayInfo *SrcInfo = getPathTo(Thu);

    if (SrcInfo == 0) return;

    for (src_iterator I = SrcInfo->begin(), E = SrcInfo->end(); I != E; ++I)
      updateDelay(CurInfo, F(Dst, ThuPos, DstUB, DstLB, *I));

    // FIXME: Also add the delay from Src to Dst.
    if (VASTOperandList::GetOperandList(Thu) && hasPathInfo(Thu)) {
      unsigned BitWidth = Thu->getBitWidth();
      delay_type D(0, 0);
      updateDelay(CurInfo, F(Dst, ThuPos, DstUB, DstLB, SrcEntryTy(Thu, D)));
    }
  }

  void accumulateDelayFrom(VASTValue *Src, VASTValue *Dst) {
    SrcDelayInfo &CurInfo = PathDelay[Dst];
    accumulateDelayThu(Src, Dst, 0, Dst->getBitWidth(), 0, CurInfo,
                       AccumulateZeroDelay);
  }

  void estimateTimingOnTree(VASTValue *Root);
};

template<typename SubClass>
class TimingEstimatorImpl : public TimingEstimatorBase {
protected:

  explicit TimingEstimatorImpl(PathDelayInfo &PathDelay, ModelType T)
    : TimingEstimatorBase(PathDelay, T) {}

public:

  void accumulateExprDelay(VASTExpr *Expr, unsigned UB, unsigned LB) {
    SrcDelayInfo &CurSrcInfo = PathDelay[Expr];
    assert(CurSrcInfo.empty() && "We are visiting the same Expr twice?");
    accumulateDelayTo(Expr, UB, LB, CurSrcInfo);
  }

  void accumulateDelayTo(VASTExpr *Expr, unsigned UB, unsigned LB,
                         SrcDelayInfo &CurSrcInfo ) {
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
      case VASTExpr::dpSel:
        SCThis->accumulateDelayThuSel(Op, Expr, i, UB, LB, CurSrcInfo);
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
    TNLDelay D = DelayFromSrc.second;
    VASTExpr *Expr = cast<VASTExpr>(Dst);
    // TODO: Get sourcewidth from delay from src?
    unsigned SrcWidth = Expr->getOperand(SrcPos)->getBitWidth();
    VFUTy *FU = getFUDesc<VFUTy>();
    unsigned LL = FU->lookupLogicLevels(SrcWidth);
    // The pre-bit logic level increment for add/mult is 1;
    unsigned LLPreBitx1024 = LL * 1024 / SrcWidth;
    unsigned LLPreBit = ((LLPreBitx1024 - 1) / 1024) + 1;
    TNLDelay Inc(LL, LLPreBit);
    unsigned ScaledLSB_LLx1024 = Inc.LSB_LLx1024 + DstLB * LLPreBitx1024;
    unsigned ScaledMSB_LLx1024 = Inc.LSB_LLx1024 + DstUB * LLPreBitx1024;
    unsigned ScaledLSB_LL = ((ScaledLSB_LLx1024 - 1) / 1024) + 1;
    unsigned ScaledMSB_LL = ((ScaledMSB_LLx1024 - 1) / 1024) + 1;

    D.addLLLSB2MSB(ScaledMSB_LL, ScaledLSB_LL, LLPreBit);
    return SrcEntryTy(DelayFromSrc.first, D);
  }

  template<typename VFUTy>
  static SrcEntryTy AccumulateBlackBoxDelay(VASTValue *Dst, unsigned SrcPos,
                                            uint8_t DstUB, uint8_t DstLB,
                                            const SrcEntryTy &DelayFromSrc) {
    TNLDelay D = DelayFromSrc.second;
    VASTExpr *CmpExpr = cast<VASTExpr>(Dst);
    unsigned SrcWidth = CmpExpr->getOperand(SrcPos)->getBitWidth();
    VFUTy *FU = getFUDesc<VFUTy>();
    unsigned LL = FU->lookupLogicLevels(SrcWidth);
    D.syncLL().addLLParallel(LL, LL);
    return SrcEntryTy(DelayFromSrc.first, D);
  }

public:
  BitlevelDelayEsitmator(PathDelayInfo &PathDelay, ModelType T)
    : Base(PathDelay, T) {}

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

  void accumulateDelayThuSel(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             uint8_t DstUB, uint8_t DstLB, SrcDelayInfo &CurInfo)
  {
    accumulateDelayThu(Thu, Dst, ThuPos, DstUB, DstLB, CurInfo,
                       AccumulateAndDelay);
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
