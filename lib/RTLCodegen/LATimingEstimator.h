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

namespace llvm {
template<typename SubClass, typename delay_type>
class TimingEstimatorBase {
protected:
  typedef typename std::map<VASTValue*, delay_type> SrcDelayInfo;
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

  delay_type getDelayFrom(VASTValue *Src, const SrcDelayInfo &SrcInfo) const {
    const_src_iterator at = SrcInfo.find(Src);
    return at == SrcInfo.end() ? delay_type(0) : at->second;
  }

  TimingNetlist &TNL;
public:
  explicit TimingEstimatorBase(TimingNetlist &Netlist) : TNL(Netlist) {}

  void updateDelay(SrcDelayInfo &Info, SrcEntryTy NewValue) {
    delay_type &OldDelay = Info[NewValue.first];
    OldDelay = std::max(OldDelay, NewValue.second);
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
      assert(!isa<VASTExpr>(Thu) && "Not SrcInfo from Src find!");
      updateDelay(CurInfo, F(Dst, ThuPos, SrcEntryTy(Thu, delay_type())));
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
      case VASTExpr::dpSGE:
      case VASTExpr::dpUGT:
      case VASTExpr::dpUGE:
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

  void analysisTimingOnTree(VASTWire *W)  {
    typedef VASTValue::dp_dep_it ChildIt;
    std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;

    VisitStack.push_back(std::make_pair(W, VASTValue::dp_dep_begin(W)));

    while (!VisitStack.empty()) {
      VASTValue *Node = VisitStack.back().first;
      ChildIt It = VisitStack.back().second;

      // We have visited all children of current node.
      if (It == VASTValue::dp_dep_end(Node)) {
        VisitStack.pop_back();

        // Accumulate the delay of the current node from all the source.
        if (VASTExpr *E = dyn_cast<VASTExpr>(Node))
          this->accumulateExprDelay(E);

        continue;
      }

      // Otherwise, remember the node and visit its children first.
      VASTValue *ChildNode = It->getAsLValue<VASTValue>();
      ++VisitStack.back().second;

      // We had already build the delay information to this node.
      if (getPathTo(ChildNode)) continue;

      // Ignore the leaf nodes.
      if (!isa<VASTWire>(ChildNode) && !isa<VASTExpr>(ChildNode)) continue;

      VisitStack.push_back(std::make_pair(ChildNode,
                                          VASTValue::dp_dep_begin(ChildNode)));
    }
  }

  void buildDatapathDelayMatrix() {
    typedef TimingNetlist::FanoutIterator it;

    for (it I = TNL.fanout_begin(), E = TNL.fanout_end(); I != E; ++I)
      analysisTimingOnTree(I->second);
  }

  delay_type getPathDelay(VASTValue *From, VASTValue *To) {
    const SrcDelayInfo *SrcInfo = getPathTo(To);
    assert(SrcInfo && "SrcInfo not available!");
    return getDelayFrom(From, *SrcInfo);
  }

  void runTimingAnalysis() {
    typedef TimingNetlist::FanoutIterator iterator;
    for (iterator I = TNL.fanout_begin(), E = TNL.fanout_end(); I != E; ++I) {
      VASTWire *ExportedWire = I->second;

      analysisTimingOnTree(ExportedWire);

      VASTValue *Dst = ExportedWire->getDriver().get();
      SrcDelayInfo *SrcInfo = getPathTo(Dst);

      for (src_iterator SI = SrcInfo->begin(), SE = SrcInfo->end(); SI != SE; ++SI) {
        if (VASTMachineOperand *Src = dyn_cast<VASTMachineOperand>(SI->first)) {
          TimingNetlist::delay_type delay = SI->second;
          //TimingNetlist::delay_type old_delay = TNL.getDelay(Src, Dst) * VFUs::Period;
          //dbgs() << "DELAY-ESTIMATOR-JSON: { \"ACCURATE\":" << old_delay
          //       << ", \"BLACKBOX\":" << delay << "} \n";
          TNL.annotateDelay(Src, Dst, delay);
        }
      }
    }
  }
};

namespace delay_estimation {
  void estimateDelaysWithBB(TimingNetlist &Netlist);
  void esitmateDelayWithLA(TimingNetlist &Netlist);
}
}

#endif
