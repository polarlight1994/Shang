//- LATimingEstimator.cpp-Estimate Delay with Linear Approximation -*- C++ -*-//
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
#include "TimingEstimator.h"
#include "shang/VASTDatapathNodes.h"
#include "shang/VASTSeqValue.h"
#include "shang/FUInfo.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-timing-estimator"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
template<typename SubClass>
class TimingEstimatorImpl : public TimingEstimatorBase {
protected:
  typedef TNLDelay delay_type;
  typedef typename std::map<VASTSeqValue*, delay_type> SrcDelayInfo;
  typedef typename SrcDelayInfo::value_type SrcEntryTy;
  typedef typename SrcDelayInfo::const_iterator src_iterator;
  typedef typename SrcDelayInfo::const_iterator const_src_iterator;

  typedef typename std::map<VASTValue*, SrcDelayInfo> PathDelayInfo;
  typedef typename PathDelayInfo::iterator path_iterator;
  typedef typename PathDelayInfo::const_iterator const_path_iterator;

  PathDelayInfo PathDelay;

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
  void annotateDelay(VASTValue *Dst, TimingNetlist::SrcInfoTy &SrcInfo) const {
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

  bool hasPathInfo(VASTValue *V) const {
    return getPathTo(V) != 0;
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
  ZeroDelayEstimator() {}

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

  static unsigned ComputeOperandSizeInByteLog2Ceil(unsigned SizeInBits) {
    return std::max(Log2_32_Ceil(SizeInBits), 3u) - 3;
  }

  template<unsigned ROWNUM>
  static SrcEntryTy AccumulateWithDelayTable(VASTValue *Dst, unsigned SrcPos,
                                             const SrcEntryTy DelayFromSrc);
public:
  BlackBoxTimingEstimator() {}

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
}

BlackBoxTimingEstimator::SrcEntryTy
BlackBoxTimingEstimator::AccumulateLUTDelay(VASTValue *Dst, unsigned SrcPos,
                                            const SrcEntryTy DelayFromSrc) {
  return SrcEntryTy(DelayFromSrc.first, DelayFromSrc.second + 0.635);
}

template<unsigned ROWNUM>
BlackBoxTimingEstimator::SrcEntryTy
BlackBoxTimingEstimator::AccumulateWithDelayTable(VASTValue *Dst, unsigned SrcPos,
                                                  const SrcEntryTy DelayFromSrc)
{
  // Delay table in nanosecond.
  static double DelayTable[][5] = {
    { 1.430 , 2.615 , 3.260 , 4.556 , 7.099 }, //Add 0
    { 1.191 , 3.338 , 4.415 , 5.150 , 6.428 }, //Shift 1
    { 1.195 , 4.237 , 4.661 , 9.519 , 12.616 }, //Mul 2
    { 1.191 , 2.612 , 3.253 , 4.531 , 7.083 }, //Cmp 3
    { 1.376 , 1.596 , 1.828 , 1.821 , 2.839 }, //Sel 4
    { 0.988 , 1.958 , 2.103 , 2.852 , 3.230 }  //Red 5
  };

  double *CurTable = DelayTable[ROWNUM];

  unsigned BitWidth = Dst->getBitWidth();

  int i = ComputeOperandSizeInByteLog2Ceil(BitWidth);

  double RoundUpLatency = CurTable[i + 1],
         RoundDownLatency = CurTable[i];
  unsigned SizeRoundUpToByteInBits = 8 << i;
  unsigned SizeRoundDownToByteInBits = i ? (8 << (i - 1)) : 0;
  double PerBitLatency =
    RoundUpLatency / (SizeRoundUpToByteInBits - SizeRoundDownToByteInBits) -
    RoundDownLatency / (SizeRoundUpToByteInBits - SizeRoundDownToByteInBits);
  // Scale the latency according to the actually width.
  double Delay =
    (RoundDownLatency + PerBitLatency * (BitWidth - SizeRoundDownToByteInBits));

  return SrcEntryTy(DelayFromSrc.first, DelayFromSrc.second + Delay);
}

namespace {
// Estimate the bit-level delay with linear approximation.
class BitlevelDelayEsitmator : public TimingEstimatorImpl<BitlevelDelayEsitmator> {
  typedef TimingEstimatorImpl<BitlevelDelayEsitmator> Base;
  typedef Base::delay_type delay_type;

public:
  BitlevelDelayEsitmator() {}

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

//===----------------------------------------------------------------------===//
void TimingEstimatorBase::estimateTimingOnTree(VASTValue *Root,
                                               TimingNetlist::SrcInfoTy &SrcInfo)
{
  VASTOperandList *L = VASTOperandList::GetDatapathOperandList(Root);

  assert(L && "Root is not a datapath node!");
  
  // The entire tree had been visited.
  if (hasPathInfo(Root)) {
    // Simply annotate the delay to SrcInfo. This happen when we visit a
    // big datapath tree that contains this Root node before.
    annotateDelay(Root, SrcInfo);
    return;
  }

  typedef VASTValue::dp_dep_it ChildIt;
  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(Root, L->op_begin()));

  while (!VisitStack.empty()) {
    VASTValue *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == VASTOperandList::GetDatapathOperandList(Node)->op_end()) {
      VisitStack.pop_back();

      // Accumulate the delay of the current node from all the source.
      if (VASTExpr *E = dyn_cast<VASTExpr>(Node))
        this->accumulateExprDelay(E);

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    // We had already build the delay information to this node.
    if (hasPathInfo(ChildNode)) continue;

    if (VASTOperandList *L = VASTOperandList::GetDatapathOperandList(ChildNode))
      VisitStack.push_back(std::make_pair(ChildNode, L->op_begin()));
  }

  annotateDelay(Root, SrcInfo);
}

void TimingEstimatorBase::annotateDelay(VASTValue *Dst,
                                        TimingNetlist::SrcInfoTy &SrcInfo) const
{
  llvm_unreachable("annotateDelay not implemented!");
}

TimingEstimatorBase *TimingEstimatorBase::CreateBlackBoxModel() {
  return new BlackBoxTimingEstimator();
}

TimingEstimatorBase *TimingEstimatorBase::CreateZeroDelayModel() {
  return new ZeroDelayEstimator();
}
