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
#include "shang/FUInfo.h"

#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-timing-estimator"
#include "llvm/Support/Debug.h"

using namespace llvm;

TimingEstimatorBase::TimingEstimatorBase(PathDelayInfo &PathDelay)
  : PathDelay(PathDelay) {}

void TimingEstimatorBase::estimateTimingOnTree(VASTValue *Root) {
  VASTOperandList *L = VASTOperandList::GetDatapathOperandList(Root);

  assert(L && "Root is not a datapath node!");

  // The entire tree had been visited or the root is some trivial node..
  if (hasPathInfo(Root)) return;

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
      else {
        VASTWire *W = cast<VASTWire>(Node);
        if (VASTValPtr V= W->getDriver())
          accumulateDelayFrom(V.get(), W);
      }

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
}


//===----------------------------------------------------------------------===//
BlackBoxTimingEstimator::SrcEntryTy
BlackBoxTimingEstimator::AccumulateLUTDelay(VASTValue *Dst, unsigned SrcPos,
                                            const SrcEntryTy DelayFromSrc) {
  return SrcEntryTy(DelayFromSrc.first, DelayFromSrc.second + 0.635);
}

unsigned
BlackBoxTimingEstimator::ComputeOperandSizeInByteLog2Ceil(unsigned SizeInBits) {
  return std::max(Log2_32_Ceil(SizeInBits), 3u) - 3;
}
