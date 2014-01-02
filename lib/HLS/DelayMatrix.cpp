//- LATimingEstimator.cpp-Estimate Delay with Linear Approximation -*- C++ -*-//
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
#include "DelayMatrix.h"

#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-timing-estimator"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace vast;

DelayMatrix::PhysicalDelay DelayMatrix::getArrivalTime(VASTSelector *To,
                                                       VASTSeqValue *From) {
  llvm_unreachable("getArrivalTime is not implemented in the subclass!");
  return None;
}

DelayMatrix::PhysicalDelay DelayMatrix::getArrivalTime(VASTSelector *To,
                                                       VASTExpr *Thu,
                                                       VASTSeqValue *From) {
  llvm_unreachable("getArrivalTime is not implemented in the subclass!");
  return None;
}

bool DelayMatrix::isBasicBlockUnreachable(BasicBlock *BB) const {
  llvm_unreachable("isBasicBlockUnreachable is not implemented in the subclass!");
  return None;
}
