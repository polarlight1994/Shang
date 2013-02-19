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
protected:
  PathDelayInfo &PathDelay;

  explicit TimingEstimatorBase(PathDelayInfo &PathDelay);

  virtual void accumulateExprDelay(VASTExpr *Expr) {}

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


  void estimateTimingOnTree(VASTValue *Root);
};

template<typename SubClass>
class TimingEstimatorImpl : public TimingEstimatorBase {
protected:

  explicit TimingEstimatorImpl(PathDelayInfo &PathDelay)
    : TimingEstimatorBase(PathDelay) {}

public:
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
    delay_type &OldDelay = Info[NewValue.first];
    OldDelay = TNLDelay::max(OldDelay, NewValue.second);
  }

  // For trivial expressions, the delay is zero.
  static SrcEntryTy AccumulateZeroDelay(VASTValue *Dst, unsigned SrcPos,
                                        const SrcEntryTy DelayFromSrc) {
    return DelayFromSrc;
  }

  // Take DelayAccumulatorTy to accumulate the design.
  // The signature of DelayAccumulatorTy should be:
  // SrcEntryTy DelayAccumulatorTy(VASTValue *Dst, unsign SrcPos,
  //                               SrcEntryTy DelayFromSrc)
  template<typename DelayAccumulatorTy>
  void accumulateDelayThu(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                          SrcDelayInfo &CurInfo, DelayAccumulatorTy F) {
    const SrcDelayInfo *SrcInfo = getPathTo(Thu);
    if (SrcInfo == 0) {
      if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(Thu)) {
        assert(!isa<VASTExpr>(Thu) && "Not SrcInfo from Src find!");
        updateDelay(CurInfo, F(Dst, ThuPos, SrcEntryTy(SeqVal, delay_type())));
      }
      return;
    }

    for (src_iterator I = SrcInfo->begin(), E = SrcInfo->end(); I != E; ++I)
      updateDelay(CurInfo, F(Dst, ThuPos, *I));

    // FIXME: Also add the delay from Src to Dst.
  }

  void accumulateExprDelay(VASTExpr *Expr) {
    SrcDelayInfo &CurSrcInfo = PathDelay[Expr];
    assert(CurSrcInfo.empty() && "We are visiting the same Expr twice?");
    SubClass *SCThis = reinterpret_cast<SubClass*>(this);

    typedef VASTExpr::const_op_iterator op_iterator;
    for (unsigned i = 0; i < Expr->size(); ++i) {
      VASTValPtr Operand = Expr->getOperand(i);
      switch (Expr->getOpcode()) {
      case VASTExpr::dpLUT:
        SCThis->accumulateDelayThuLUT(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpAnd:
        SCThis->accumulateDelayThuAnd(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpRAnd:
        SCThis->accumulateDelayThuRAnd(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpRXor:
        SCThis->accumulateDelayThuRXor(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpSGT:
      case VASTExpr::dpUGT:
        SCThis->accumulateDelayThuCmp(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpAdd:
        SCThis->accumulateDelayThuAdd(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpMul:
        SCThis->accumulateDelayThuMul(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpShl:
        SCThis->accumulateDelayThuShl(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpSRL:
        SCThis->accumulateDelayThuSRL(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpSRA:
        SCThis->accumulateDelayThuSRA(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpSel:
        SCThis->accumulateDelayThuSel(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpAssign:
        SCThis->accumulateDelayThuAssign(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpBitCat:
        SCThis->accumulateDelayThuRBitCat(Operand.get(), Expr, i, CurSrcInfo);
        break;
      case VASTExpr::dpBitRepeat:
        SCThis->accumulateDelayBitRepeat(Operand.get(), Expr, i, CurSrcInfo);
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

// Set all datapath delay to zero.
class ZeroDelayEstimator : public TimingEstimatorImpl<ZeroDelayEstimator> {
  typedef TimingEstimatorImpl<ZeroDelayEstimator> Base;
  typedef Base::delay_type delay_type;

public:
  explicit ZeroDelayEstimator(PathDelayInfo &PathDelay) : Base(PathDelay) {}

  void accumulateDelayThuLUT(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRXor(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuCmp(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuAdd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuMul(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuShl(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuSRL(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuSRA(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuSel(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuAssign(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRBitCat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                 SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayBitRepeat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }
};

// Accumulating the delay according to the blackbox model.
// This is the baseline delay estimate model.
class BlackBoxTimingEstimator
  : public TimingEstimatorImpl<BlackBoxTimingEstimator> {
  typedef TimingEstimatorImpl<BlackBoxTimingEstimator> Base;
  typedef Base::delay_type delay_type;

  static SrcEntryTy AccumulateLUTDelay(VASTValue *Dst, unsigned SrcPos,
                                       const SrcEntryTy DelayFromSrc);

  static unsigned ComputeOperandSizeInByteLog2Ceil(unsigned SizeInBits);

  template<unsigned ROWNUM>
  static SrcEntryTy AccumulateWithDelayTable(VASTValue *Dst, unsigned SrcPos,
                                             const SrcEntryTy DelayFromSrc);
public:
  explicit BlackBoxTimingEstimator(PathDelayInfo &PathDelay) : Base(PathDelay) {}

  void accumulateDelayThuLUT(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateLUTDelay);
  }

  void accumulateDelayThuAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateLUTDelay);
  }

  void accumulateDelayThuRAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<5>);
  }

  void accumulateDelayThuRXor(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<5>);
  }

  void accumulateDelayThuCmp(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<3>);
  }

  void accumulateDelayThuAdd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<0>);
  }

  void accumulateDelayThuMul(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<2>);
  }

  void accumulateDelayThuShl(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<1>);
  }

  void accumulateDelayThuSRL(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<1>);
  }

  void accumulateDelayThuSRA(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<1>);
  }

  void accumulateDelayThuSel(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateWithDelayTable<4>);
  }

  void accumulateDelayThuAssign(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRBitCat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                 SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayBitRepeat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }
};

// Estimate the bit-level delay with linear approximation.
class BitlevelDelayEsitmator : public TimingEstimatorImpl<BitlevelDelayEsitmator> {
  typedef TimingEstimatorImpl<BitlevelDelayEsitmator> Base;
  typedef Base::delay_type delay_type;

public:
  explicit BitlevelDelayEsitmator(PathDelayInfo &PathDelay) : Base(PathDelay) {}

  void accumulateDelayThuLUT(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRAnd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRXor(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuCmp(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuAdd(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuMul(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                             SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuShl(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuSRL(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuSRA(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuSel(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuAssign(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayThuRBitCat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                                 SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }

  void accumulateDelayBitRepeat(VASTValue *Thu, VASTValue *Dst, unsigned ThuPos,
                              SrcDelayInfo &CurInfo) {
    accumulateDelayThu(Thu, Dst, ThuPos, CurInfo, AccumulateZeroDelay);
  }
};
}

#endif
