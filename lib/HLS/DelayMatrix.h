//=- LATimingEstimator.h-Estimate Delay with Linear Approximation -*- C++ -*-=//
//
//                      The VAST HLS frameowrk                                //
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

#include "vast/LuaI.h"
#include "vast/VASTDatapathNodes.h"
#include "vast/VASTSeqValue.h"
#include "vast/Utilities.h"

#include "llvm/ADT/None.h"
#include "llvm/Support/ErrorHandling.h"

namespace vast {
using namespace llvm;

class VASTValue;
class VASTExpr;
class VASTSeqValue;
class Dataflow;

class DelayMatrix {
public:
  // Data structure that explicitly hold the total delay and cell delay of a
  // datapath. Based on total delay and cell delay we can calculate the
  // corresponding wire delay.
  struct PhysicalDelay {
    float TotalDelay;
    float CellDelay;

    PhysicalDelay() : TotalDelay(0.0f), CellDelay(0.0f) {}
    PhysicalDelay(NoneType)
      : TotalDelay(-1.1e+10f), CellDelay(-1.1e+10f) {}

    bool isNone() const { return TotalDelay < -1e+10f; }

    bool operator < (const PhysicalDelay &RHS) const {
      return TotalDelay < RHS.TotalDelay;
    }
  };

  virtual ~DelayMatrix() {}
  virtual PhysicalDelay getArrivalTime(VASTSelector *To, VASTSeqValue *From) = 0;
  virtual PhysicalDelay getArrivalTime(VASTSelector *To, VASTExpr *Thu,
                                       VASTSeqValue *From) = 0;
  virtual bool isBasicBlockUnreachable(BasicBlock *BB) const = 0;
};

DelayMatrix *createExternalTimingAnalysis(VASTModule &VM, Dataflow &DF);
}

#endif
