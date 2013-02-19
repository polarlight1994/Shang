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

unsigned
BlackBoxTimingEstimator::ComputeOperandSizeInByteLog2Ceil(unsigned SizeInBits) {
  return std::max(Log2_32_Ceil(SizeInBits), 3u) - 3;
}
