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
class VASTValue;
class VASTExpr;

class TimingEstimatorBase {
protected:
  TimingEstimatorBase() {}

  virtual void annotateDelay(VASTValue *Dst,
                             TimingNetlist::SrcInfoTy &SrcInfo) const;

  virtual void accumulateExprDelay(VASTExpr *Expr) {}

  virtual bool hasPathInfo(VASTValue *V) const { return true; }
public:
  virtual ~TimingEstimatorBase() {}


  void estimateTimingOnTree(VASTValue *Root, TimingNetlist::SrcInfoTy &SrcInfo);

  static TimingEstimatorBase *CreateBlackBoxModel();
  static TimingEstimatorBase *CreateZeroDelayModel();
};

}

#endif
