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

  const TimingNetlist &Netlist;
public:
  explicit TimingEstimatorBase(const TimingNetlist &Netlist)
    : Netlist(Netlist) {}

  void updateDelay(SrcDelayInfo &Info, SrcEntryTy NewValue) {
    delay_type &OldDelay = Info[NewValue.first];
    OldDelay = std::max(OldDelay, NewValue.second);
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
          reinterpret_cast<SubClass*>(this)->accumulateExprDelay(E);

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

    for (it I = Netlist.fanout_begin(), E = Netlist.fanout_end(); I != E; ++I)
      analysisTimingOnTree(I->second);
  }

  delay_type getPathDelay(VASTValue *From, VASTValue *To) {
    const SrcDelayInfo *SrcInfo = getPathTo(To);
    assert(SrcInfo && "SrcInfo not available!");
    return getDelayFrom(From, *SrcInfo);
  }
};
}

#endif
