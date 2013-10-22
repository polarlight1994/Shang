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

TimingEstimatorBase::TimingEstimatorBase(PathDelayInfo &PathDelay, ModelType T)
  : PathDelay(PathDelay), T(T) {}

VASTExpr *TimingEstimatorBase::getAsUnvisitedExpr(VASTValue *V) const {
  if (hasPathInfo(V))
    return 0;

  return dyn_cast<VASTExpr>(V);
}

void TimingEstimatorBase::estimateTimingOnCone(VASTExpr *Root) {
  // The entire tree had been visited or the root is some trivial node..
  if (hasPathInfo(Root)) return;

  typedef VASTOperandList::op_iterator ChildIt;
  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(Root, Root->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == Node->op_end()) {
      VisitStack.pop_back();

      // Accumulate the delay of the current node from all the source.
      if (VASTExpr *E = dyn_cast<VASTExpr>(Node)) {
        this->accumulateExprDelay(E, E->getBitWidth(), 0);
        continue;
      }

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(ChildNode)) {
      if (!isChainingCandidate(SV->getLLVMValue()) || SV->num_fanins() != 1)
        continue;

      const VASTLatch &L = SV->getUniqueFanin();

      if (VASTExpr *Expr = getAsUnvisitedExpr(VASTValPtr(L).get()))
        VisitStack.push_back(std::make_pair(Expr, Expr->op_begin()));

      if (VASTExpr *Expr = getAsUnvisitedExpr(VASTValPtr(L.getGuard()).get()))
        VisitStack.push_back(std::make_pair(Expr, Expr->op_begin()));

      continue;
    }

    if (VASTExpr *ChildExpr = getAsUnvisitedExpr(ChildNode))
      VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));
  }
}

//===----------------------------------------------------------------------===//
BlackBoxDelayEsitmator::SrcEntryTy
BlackBoxDelayEsitmator::AccumulateLUTDelay(VASTValue *Dst, unsigned SrcPos,
                                           uint8_t DstUB, uint8_t DstLB,
                                           const SrcEntryTy &DelayFromSrc) {
  delay_type D = DelayFromSrc.second;
  delay_type Inc(VFUs::LUTDelay);
  return SrcEntryTy(DelayFromSrc.first, D + Inc);
}

BlackBoxDelayEsitmator::SrcEntryTy
BlackBoxDelayEsitmator::AccumulateAndDelay(VASTValue *Dst, unsigned SrcPos,
                                           uint8_t DstUB, uint8_t DstLB,
                                           const SrcEntryTy &DelayFromSrc) {
  delay_type D = DelayFromSrc.second;
  VASTExpr *AndExpr = cast<VASTExpr>(Dst);
  unsigned NumFanins = AndExpr->size();
  unsigned LL = Log2_32_Ceil(NumFanins) / Log2_32_Ceil(VFUs::MaxLutSize);
  delay_type Inc(LL * VFUs::LUTDelay);
  return SrcEntryTy(DelayFromSrc.first, D + Inc);
}

BlackBoxDelayEsitmator::SrcEntryTy
BlackBoxDelayEsitmator::AccumulateCROMDelay(VASTValue *Dst, unsigned SrcPos,
                                            uint8_t DstUB, uint8_t DstLB,
                                            const SrcEntryTy &DelayFromSrc) {
  delay_type D = DelayFromSrc.second;
  VASTExpr *CROMExpr = cast<VASTExpr>(Dst);
  // Calculate the delay of a combinational ROM based on the number of logic
  // level. The way to calculate the
  unsigned AddrWidth = CROMExpr->getOperand(0)->getBitWidth();
  unsigned LL = Log2_32_Ceil(AddrWidth) / Log2_32_Ceil(VFUs::MaxLutSize);
  delay_type Inc(LL * VFUs::LUTDelay);
  return SrcEntryTy(DelayFromSrc.first, D + Inc);
}

BlackBoxDelayEsitmator::SrcEntryTy
BlackBoxDelayEsitmator::AccumulateRedDelay(VASTValue *Dst, unsigned SrcPos,
                                           uint8_t DstUB, uint8_t DstLB,
                                           const SrcEntryTy &DelayFromSrc) {
  assert(DstUB == 1 && DstLB == 0 && "Bad UB and LB!");
  delay_type D = DelayFromSrc.second;
  VASTExpr *RedExpr = cast<VASTExpr>(Dst);
  unsigned FUWidth = RedExpr->getOperand(0)->getBitWidth();
  unsigned LL = Log2_32_Ceil(FUWidth) / Log2_32_Ceil(VFUs::MaxLutSize);
  delay_type Inc(LL * VFUs::LUTDelay);
  return SrcEntryTy(DelayFromSrc.first, D + Inc);
}

BlackBoxDelayEsitmator::SrcEntryTy
BlackBoxDelayEsitmator::AccumulateCmpDelay(VASTValue *Dst, unsigned SrcPos,
                                           uint8_t DstUB, uint8_t DstLB,
                                           const SrcEntryTy &DelayFromSrc) {
  assert(DstUB == 1 && DstLB == 0 && "Bad UB and LB!");
  delay_type D = DelayFromSrc.second;
  VASTExpr *CmpExpr = cast<VASTExpr>(Dst);
  unsigned FUWidth = CmpExpr->getOperand(SrcPos)->getBitWidth();
  VFUICmp *Cmp = getFUDesc<VFUICmp>();
  float Latency = Cmp->lookupLatency(FUWidth);
  delay_type Inc(Latency);
  return SrcEntryTy(DelayFromSrc.first, D + Inc);
}
