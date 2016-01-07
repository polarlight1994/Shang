//===--------------------- SIRSTGDistance.cpp -------------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the SIRSTGDistance pass which compute the shortest path
// distance between the states in the State-Transition-Graph.
//
//===----------------------------------------------------------------------===//

#include "sir/SIRSTGDistance.h"

#include "vast/LuaI.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallVector.h"

#include <set>

using namespace llvm;
using namespace vast;

char SIRSTGDistance::ID = 0;

INITIALIZE_PASS_BEGIN(SIRSTGDistance,
                      "SIR-STG-Distance",
                      "Compute the shortest path distance between states in STG",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(SIRTimingAnalysis)
  INITIALIZE_PASS_DEPENDENCY(SIRScheduling)
  INITIALIZE_PASS_DEPENDENCY(SIRRegisterSynthesisForCodeGen)
INITIALIZE_PASS_END(SIRSTGDistance,
                    "SIR-STG-Distance",
                    "Compute the shortest path distance between states in STG",
                    false, true)

bool SIRSTGDistance::existPath(Value *Src, Value *Dst) {
  if (Src == Dst)
    return true;

  Instruction *Root = dyn_cast<Instruction>(Dst);

  if (!Root) return false;

  typedef Instruction::op_iterator ChildIt;
  std::vector<std::pair<Instruction *, ChildIt> > VisitStack;
  VisitStack.push_back(std::make_pair(Root, Root->op_begin()));

  while (!VisitStack.empty()) {
    Instruction *Inst = VisitStack.back().first;
    ChildIt &It = VisitStack.back().second;
    ChildIt EndIt = Inst->op_end();

    if (It == EndIt) {
      if (*It == Src)
        return true;

      VisitStack.pop_back();
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    Value *ChildVal = *It;

    if (ChildVal == Src)
      return true;

    if (It != EndIt)
      ++It;

    if (Instruction *ChildInst = dyn_cast<Instruction>(ChildVal)) {
      if (!ChildInst)
        continue;

      if (IntrinsicInst *ChildII = dyn_cast<IntrinsicInst>(ChildInst))
        if (ChildII->getIntrinsicID() == Intrinsic::shang_reg_assign) {
          SIRRegister *Reg = SM->lookupSIRReg(ChildVal);
          continue;
        }

      VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
    }
  }

  return false;
}

unsigned SIRSTGDistance::getIntervalFromSrc(SIRRegister *Reg, SIRSlot *ReadSlot) {
  unsigned PathInterval = UINT16_MAX;

  typedef SIRRegister::faninslots_iterator iterator;
  for (iterator I = Reg->faninslots_begin(), E = Reg->faninslots_end(); I != E; ++I) {
    SIRSlot *DefSlot = *I;

    if (DefSlot->getSlotNum() >= ReadSlot->getSlotNum())
      continue;

    unsigned Interval = getIntervalFromSrc(DefSlot, ReadSlot);
    PathInterval = std::min(PathInterval, Interval);
  }

  return PathInterval;
}

unsigned SIRSTGDistance::getIntervalFromSrc(SIRSlot *DefSlot, SIRSlot *ReadSlot) {
  assert(DefSlot->getSlotNum() < ReadSlot->getSlotNum() && "Unexpected Backedge!");

  unsigned PathInterval = UINT16_MAX;
  unsigned ReadSlotNum = ReadSlot->getSlotNum();

  // Perform Depth-First-Search to reach the "next slot" of DefSlot which we
  // read on.
  std::set<SIRSlot *> Visited;
  std::vector<std::pair<SIRSlot *, SIRSlot::succ_iterator> > VisitStack;
  VisitStack.push_back(std::make_pair(DefSlot, DefSlot->succ_begin()));

  while (!VisitStack.empty()) {
    SIRSlot *S = VisitStack.back().first;
    SIRSlot::succ_iterator ChildIt = VisitStack.back().second;

    if (ChildIt == S->succ_end()) {
      VisitStack.pop_back();
      continue;
    }

    SIRSlot::EdgePtr Edge = *ChildIt;
    SIRSlot *ChildSlot = Edge;
    ++VisitStack.back().second;

    if (Edge.getDistance()) {
      unsigned NextSlotNum = ChildSlot->getSlotNum();

      if (NextSlotNum == ReadSlotNum)
        return 1;

      unsigned Interval = getDistance(NextSlotNum, ReadSlotNum);

      PathInterval = std::min(PathInterval, Interval);

      // Skip the children of the current node as we had already reach the leave.
      continue;
    }

    // Do not visit a node twice.
    if (!Visited.count(ChildSlot)) continue;

    Visited.insert(ChildSlot);
    VisitStack.push_back(std::make_pair(ChildSlot, ChildSlot->succ_begin()));
  }

  return std::min(PathInterval + 1u, 65535u);
}

unsigned SIRSTGDistance::getIntervalFromSrc(SIRRegister *SrcReg, SIRRegister *DstReg, ArrayRef<SIRSlot *> ReadSlots) {
  unsigned PathInterval = UINT16_MAX;

  SmallVector<SIRSlot *, 4> RealReadSlots;

  typedef ArrayRef<SIRSlot *>::iterator iterator;
  for (iterator I = ReadSlots.begin(), E = ReadSlots.end(); I != E; ++I) {
    SIRSlot *S = *I;

    typedef SIRSlot::op_iterator op_iterator;
    for (op_iterator OI = S->op_begin(), OE = S->op_end(); OI != OE; ++OI) {
      SIRSeqOp *SeqOp = *OI;

      if (SeqOp->getDst() != DstReg)
        continue;

      Value *SrcVal = SeqOp->getSrc();
      Value *GuardVal = SeqOp->getGuard();
      Value *SlotVal = SeqOp->getSlot()->getGuardValue();

      SmallVector<Value *, 4> Roots;
      Roots.push_back(SrcVal);
      Roots.push_back(GuardVal);
      Roots.push_back(SlotVal);

      for (int i = 0; i < Roots.size(); ++i) {
        Value *Src = Roots[i];

        if (existPath(SrcReg->getLLVMValue(), Src))
          RealReadSlots.push_back(S);
      }
    }
  }

  assert (RealReadSlots.size() && "Unexpected no read slot!");

  for (int i = 0; i < RealReadSlots.size(); ++i) {
    SIRSlot *ReadSlot = RealReadSlots[i];

    unsigned Interval = getIntervalFromSrc(SrcReg, ReadSlot);
    PathInterval = std::min(PathInterval, Interval);
  }

  return PathInterval;
}

unsigned SIRSTGDistance::getDistance(unsigned SrcSlotNum, unsigned DstSlotNum) {
  std::map<unsigned, std::map<unsigned, unsigned> >::iterator
    dst_at = DistanceMatrix.find(DstSlotNum);

  if (dst_at == DistanceMatrix.end())
    return UINT16_MAX;

  std::map<unsigned, unsigned>::iterator src_at = dst_at->second.find(SrcSlotNum);

  if (src_at == dst_at->second.end())
    return UINT16_MAX;

  return src_at->second;
}

bool SIRSTGDistance::updateDistance(unsigned Distance, unsigned SrcSlotNum,
                                    unsigned DstSlotNum) {
  unsigned Src2DstDistance = getDistance(SrcSlotNum, DstSlotNum);
  // Update if the Distance is smaller than origin Distance.
  if (Distance < Src2DstDistance) {
    DistanceMatrix[DstSlotNum][SrcSlotNum] = Distance;
    return true;
  }

  return false;
}

bool SIRSTGDistance::runOnSIR(SIR &SM) {
  this->SM = &SM;

  typedef SIR::slot_iterator slot_iterator;
  for (slot_iterator I = SM.slot_begin(), E = SM.slot_end(); I != E; ++I) {
    SIRSlot *Src = I;

    typedef SIRSlot::succ_iterator succ_iterator;
    for (succ_iterator SI = Src->succ_begin(), SE = Src->succ_end(); SI != SE; ++SI) {
      SIRSlot *Dst = *SI;

      if (Src == Dst)
        continue;

      DistanceMatrix[Dst->getSlotNum()][Src->getSlotNum()] = SI->getDistance();
    }
  }

  ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> > RPO(SM.slot_begin());

  typedef
    ReversePostOrderTraversal<SIRSlot *, GraphTraits<SIRSlot *> >::rpo_iterator iterator;

  bool changed = true;
  while (changed) {
    changed = false;

    // Use the Floyd-Warshal algorithm to compute the shortest path.
    for (iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
      SIRSlot *Dst = *I;
      unsigned DstNum = Dst->getSlotNum();

      typedef SIRSlot::pred_iterator pred_iterator;
      for (pred_iterator PI = Dst->pred_begin(), PE = Dst->pred_end(); PI != PE; ++PI) {
        SIRSlot *Thu = *PI;
        unsigned ThuNum = Thu->getSlotNum();

        unsigned Thu2DstDistance = PI->getDistance();

        std::map<unsigned, unsigned> &Srcs = DistanceMatrix[ThuNum];
        typedef std::map<unsigned, unsigned>::iterator src_iterator;
        for (src_iterator SI = Srcs.begin(), SE = Srcs.end(); SI != SE; ++SI) {
          unsigned SrcNum = SI->first;

          if (SrcNum == DstNum) continue;

          unsigned Src2ThuDistance = SI->second;
          unsigned Src2DstDistance = Src2ThuDistance + Thu2DstDistance;

          changed |= updateDistance(Src2DstDistance, SrcNum, DstNum);
        }
      }
    }
  }

  return false;
}