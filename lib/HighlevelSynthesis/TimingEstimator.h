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

#include "llvm/Support/ErrorHandling.h"

namespace llvm {
class TimingEstimatorBase {
protected:
  TimingEstimatorBase() {}
public:
  virtual ~TimingEstimatorBase() {}

  virtual void accumulateExprDelay(VASTExpr *Expr) {}
  virtual bool hasPathInfo(VASTValue *V) const { return true; }

  void estimateTimingOnTree(VASTValue *Root);

  static TimingEstimatorBase *CreateBlackBoxModel();
};

}

#endif
