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

TimingEstimatorBase::TimingEstimatorBase(PathDelayInfo &PathDelay, ModelType T,
                                         CachedSequashTable *CachedSequash)
  : CachedSequash(CachedSequash), PathDelay(PathDelay), T(T) {}

void TimingEstimatorBase::estimateTimingOnCone(VASTExpr *Root) {
  // The entire tree had been visited or the root is some trivial node.
  unsigned RootID = CachedSequash->getOrCreateSequashID(Root);
  if (hasPathInfo(RootID)) return;

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

    // We had already build the delay information to this node.
    unsigned ChildID = CachedSequash->getOrCreateSequashID(ChildNode);
    if (hasPathInfo(ChildID)) continue;

    if (VASTExpr *ChildExpr = dyn_cast<VASTExpr>(ChildNode))
      VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));
  }
}

//===----------------------------------------------------------------------===//
BitlevelDelayEsitmator::SrcEntryTy
BitlevelDelayEsitmator::AccumulateLUTDelay(VASTValue *Dst, unsigned SrcPos,
                                           uint8_t DstUB, uint8_t DstLB,
                                           const SrcEntryTy &DelayFromSrc) {
  delay_type D = DelayFromSrc.second;
  delay_type Inc(VFUs::LUTDelay);
  return SrcEntryTy(DelayFromSrc.first, D + Inc);
}

BitlevelDelayEsitmator::SrcEntryTy
BitlevelDelayEsitmator::AccumulateAndDelay(VASTValue *Dst, unsigned SrcPos,
                                           uint8_t DstUB, uint8_t DstLB,
                                           const SrcEntryTy &DelayFromSrc) {
  delay_type D = DelayFromSrc.second;
  VASTExpr *AndExpr = cast<VASTExpr>(Dst);
  unsigned NumFanins = AndExpr->size();
  unsigned LL = Log2_32_Ceil(NumFanins) / Log2_32_Ceil(VFUs::MaxLutSize);
  delay_type Inc(LL * VFUs::LUTDelay);
  return SrcEntryTy(DelayFromSrc.first, D + Inc);
}

BitlevelDelayEsitmator::SrcEntryTy
BitlevelDelayEsitmator::AccumulateRedDelay(VASTValue *Dst, unsigned SrcPos,
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

BitlevelDelayEsitmator::SrcEntryTy
BitlevelDelayEsitmator::AccumulateCmpDelay(VASTValue *Dst, unsigned SrcPos,
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

void
BitlevelDelayEsitmator::accumulateDelayThuAssign(VASTValue *Thu, VASTValue *Dst,
                                                 unsigned ThuPos,
                                                 uint8_t DstUB, uint8_t DstLB,
                                                 SrcDelayInfo &CurInfo) {
  VASTExpr *BitSliceExpr = cast<VASTExpr>(Dst);
  // Translate the (UB, LB] against the bitslice to the (UB, LB] against the
  // Src value.
  uint8_t UB = DstUB + BitSliceExpr->LB, LB = DstLB + BitSliceExpr->LB;
  assert(LB >= BitSliceExpr->LB && UB <= BitSliceExpr->UB && "Bad bitslice!");

  // Handle the trivial case trivially.
  if (VASTExpr *ThuExpr = dyn_cast<VASTExpr>(Thu)) {
    // Accumulate the scaled delay of ThuExpr to the current bitslice expression.
    accumulateDelayTo(ThuExpr, UB, LB, CurInfo);
  }

  // Build the delay from Thu to Dst.
  accumulateDelayThu(Thu, Dst, ThuPos, UB, LB, CurInfo, AccumulateZeroDelay);
}
