//===------ ScheduleEmitter.cpp - Emit the Schedule -------------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Schedule emitter, which reimplement the
// state-transition graph according to the scheduling results. It also re-time
// the data-path if necessary.
//
//===----------------------------------------------------------------------===//
//
#include "VASTScheduling.h"

#include "vast/VASTModulePass.h"
#include "vast/VASTExprBuilder.h"
#include "vast/VASTMemoryBank.h"
#include "vast/VASTModule.h"
#include "vast/STGDistances.h"

#include "llvm/IR/Function.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-schedule-emitter"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumBBByPassed, "Number of BasicBlock Bypassed by the CFG folding");
STATISTIC(NumFalsePathSkip,
          "Number of False Paths skipped during the CFG folding");
STATISTIC(NumSlots, "Number of States created in the State-transition Graph");
STATISTIC(NumSubGropus, "Number of guarding condition equivalent state groups");
STATISTIC(NumRetimed, "Number of retimed leaf node");
STATISTIC(NumRejectedRetiming,
          "Number of reject retiming because the predicates are not compatible");
STATISTIC(NumMultRetimeIncoming,
          "Number of retiming with multiple incoming values");

namespace {
class NaiveMemoryPortReservationTable {
  // The memory port that will assign to next memory access.
  DenseMap<VASTMemoryBank*, unsigned> PortForNextAccess;
  // Map the instruction to the corresponding memory port, the map is actually
  // mapping the instruction to PortNum + 1, so when we try to lookup the map
  // and get a zero, we know the instruction is not bound to a port yet.
  DenseMap<Instruction*, unsigned> PortMapping;
  DenseMap<Instruction*, VASTMemoryBank*> BusMapping;
public:
  unsigned getOrCreateMapping(VASTSeqInst *Op) {
    Instruction *Inst = cast<Instruction>(Op->getValue());
    assert(isLoadStore(Inst) && "Unexpected Inst type!");
    // Lookup the mapping, please note that we will get PortNum + 1 if the
    // mapping exists.
    unsigned &PortNum = PortMapping[Inst];
    if (PortNum) return PortNum - 1;

    VASTSelector *Addr = Op->getSrc(0).getSelector();
    VASTMemoryBank *Bus = dyn_cast<VASTMemoryBank>(Addr->getParent());

    // Ignore the default memory bus or the signle port Bus.
    if (Bus == 0 || !Bus->isDualPort()) return 0;

    BusMapping[Inst] = Bus;
    unsigned &NextPort = PortForNextAccess[Bus];
    // Store Port + 1 to the mapping.
    PortNum = NextPort + 1;
    // Flip the port number of the next query.
    NextPort ^= 0x1;

    return PortNum - 1;
  }

  VASTMemoryBank *getMemBus(Instruction *Inst) const {
    return BusMapping.lookup(Inst);
  }
};

class ScheduleEmitter : public MinimalExprBuilderContext {
  VASTExprBuilder Builder;
  VASTModule &VM;
  VASTCtrlRgn &R;
  NaiveMemoryPortReservationTable PRT;
  ilist<VASTSlot> OldSlots;
  VASTSchedGraph &G;
  std::map<BasicBlock*, VASTSlot*> LandingSlots;
  unsigned CurrentSlotNum;

  void clearUp();

  VASTSlot *createSlot(BasicBlock *BB, unsigned Schedule) {
    ++NumSlots;
    return R.createSlot(++CurrentSlotNum, BB, Schedule);
  }

  VASTSlot *getOrCreateLandingSlot(BasicBlock *BB, unsigned Schedule) {
    VASTSlot *&S = LandingSlots[BB];
    if (S == 0) S = createSlot(BB, Schedule);
    return S;
  }

  VASTSlot *getOrCreateSubGroup(BasicBlock *BB, VASTValPtr Cnd, VASTSlot *S) {
    VASTSlot *SubGrp = S->getSubGroup(BB);

    if (SubGrp) {
      assert(SubGrp->getGuard() == Cnd && "Inconsistent guarding conditions!");
      return SubGrp;
    }

    // Create the subgroup if it does not exist.
    ++NumSubGropus;
    SubGrp = R.createSlot(++CurrentSlotNum, BB, S->Schedule, Cnd, true);
    // The subgroups are not actually the successors of S in the control flow.
    S->addSuccSlot(SubGrp, VASTSlot::SubGrp);
    return SubGrp;
  }

  VASTSlotCtrl *addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd,
                            Value *V = 0);

  VASTSeqInst *cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot);
  // We may need to remap the memory port for the memory accesses, if dual port
  // RAM is enabled.
  VASTSeqInst *remapToPort1(VASTSeqInst *Op, VASTSlot *ToSlot);

  VASTSlotCtrl *cloneSlotCtrl(VASTSlotCtrl *Op, VASTSlot *ToSlot);

  /// Emit the scheduling units in the same BB.
  ///
  void emitScheduleInBB(MutableArrayRef<VASTSchedUnit*> SUs);

  bool emitToFirstSlot(VASTSlot *ToSlot, MutableArrayRef<VASTSchedUnit*> SUs);

  void emitToSlot(VASTSeqOp *Op, VASTSlot *ToSlot);

  void handleNewSeqOp(VASTSeqInst *SeqOp);
  void handleNewSeqOp(VASTSlotCtrl *SeqOp);
public:
  ScheduleEmitter(VASTModule &VM, VASTCtrlRgn &R, VASTSchedGraph &G);
  ~ScheduleEmitter() { clearUp(); }

  void emitSchedule();
};
}

//===----------------------------------------------------------------------===//
ScheduleEmitter::ScheduleEmitter(VASTModule &VM, VASTCtrlRgn &R,
                                 VASTSchedGraph &G)
  : MinimalExprBuilderContext(VM), Builder(*this), VM(VM), R(R), G(G),
    CurrentSlotNum(0) {}

void ScheduleEmitter::clearUp() {
  // Clear up the VASTSeqOp in the old list.
  OldSlots.clear();

  // Release the dead objects now.
  VM.gc();
}

//===----------------------------------------------------------------------===//
static
int top_sort_schedule(const VASTSchedUnit *LHS, const VASTSchedUnit *RHS) {
  if (LHS->getSchedule() != RHS->getSchedule())
    return LHS->getSchedule() < RHS->getSchedule() ? -1 : 1;

  //if (LHS->isBBEntry()) return -1;
  //if (RHS->isBBEntry()) return 1;

  VASTSeqOp *LHSOp = LHS->getSeqOp(), *RHSOp = RHS->getSeqOp();

  /// Put the pseudo Scheduling Unit before the normal Scheduling Unit.
  if (LHSOp == 0 && RHSOp != 0) return -1;
  if (LHSOp != 0 && RHSOp == 0) return 1;

  // Make sure we emit all VASTSeqInsts before emitting the VASTSlotCtrls.
  // Because we will emit the SUs in the first slot of the BB that pointed by
  // the VASTCtrls, and we may need to perform retiming based on the newly
  // emitted VASTSeqInsts.
  if (LHSOp && RHSOp && LHSOp->getASTType() != RHSOp->getASTType()) {
    if (LHSOp->getASTType() == VASTNode::vastSeqInst) return -1;
    if (RHSOp->getASTType() == VASTNode::vastSeqInst) return 1;
  }

  // Now LHSOp and RHSOp have the same ASTType.
  // Put the latch before lanch.
  if (LHS->isLatch() && !RHS->isLatch()) return -1;
  if (!LHS->isLatch() && RHS->isLatch()) return 1;

  if (LHS->getIdx() < RHS->getIdx()) return -1;
  if (LHS->getIdx() > RHS->getIdx()) return 1;

  return 0;
}

static int top_sort_schedule_wrapper(VASTSchedUnit *const *LHS,
                                     VASTSchedUnit *const *RHS) {
  return top_sort_schedule(*LHS, *RHS);
}

//===----------------------------------------------------------------------===//
VASTSlotCtrl *ScheduleEmitter::addSuccSlot(VASTSlot *S, VASTSlot *NextSlot,
                                           VASTValPtr Cnd, Value *V) {
  // If the Br already exist, simply or the conditions together.
  assert(!S->hasNextSlot(NextSlot) && "Edge had already existed!");
  assert((S->getParent() == NextSlot->getParent()
          || NextSlot == R.getStartSlot())
        && "Cannot change Slot and BB at the same time!");

  assert(!NextSlot->IsSubGrp && "Unexpected subgroup!");
  S->addSuccSlot(NextSlot, VASTSlot::Sucessor);
  VASTSlotCtrl *SlotBr = R.createStateTransition(NextSlot, S, Cnd);
  if (V) SlotBr->annotateValue(V);

  return SlotBr;
}

VASTSlotCtrl *
ScheduleEmitter::cloneSlotCtrl(VASTSlotCtrl *Op, VASTSlot *ToSlot) {
  // Combine the guarding conditions
  VASTValPtr Cnd = Op->getGuard();

  // Some times we may even try to fold the BB through a 'false path' ... such
  // folding can be safely skipped.
  if (Cnd == VASTConstant::False) {
    ++NumFalsePathSkip;
    return 0;
  }

  Value *V = Op->getValue();

  // Handle the trivial case
  if (!Op->isBranch()) {
    VASTSlotCtrl *NewSlotCtrl
      = R.createStateTransition(Op->getNode(), ToSlot, Cnd);
    NewSlotCtrl->annotateValue(V);
    return NewSlotCtrl;
  }

  if (isa<ReturnInst>(V) || isa<UnreachableInst>(V)) {
    VASTSlot *SubGrp = getOrCreateSubGroup(NULL, Cnd, ToSlot);
    return addSuccSlot(SubGrp, R.getStartSlot(), Cnd, V);
  }

  BasicBlock *TargetBB = Op->getTargetSlot()->getParent();
  VASTSlot *SubGrp = getOrCreateSubGroup(TargetBB, Cnd, ToSlot);
  // Emit the the SUs in the first slot in the target BB.
  // Connect to the landing slot if not all SU in the target BB emitted to
  // current slot.
  MutableArrayRef<VASTSchedUnit*> SUs(G.getSUInBB(TargetBB));
  if (emitToFirstSlot(SubGrp, SUs)) {
    assert(SUs[0]->isBBEntry() && "BBEntry not placed at the beginning!");
    unsigned EntrySchedSlot = SUs[0]->getSchedule();
    // There is some SeqOp need to be emitted to TargetBB, build the control
    // flow.
    VASTSlot *LandingSlot = getOrCreateLandingSlot(TargetBB, EntrySchedSlot + 1);
    return addSuccSlot(SubGrp, LandingSlot, Cnd, V);
  }

  // Else all scheduling unit of target block are emitted to current slot
  // do not emit the SlotCtrl because it is not needed.
  ++NumBBByPassed;
  return 0;
}
//===----------------------------------------------------------------------===//
VASTSeqInst *ScheduleEmitter::remapToPort1(VASTSeqInst *Op, VASTSlot *ToSlot) {
  if (Op->isLatch()) {
    Instruction *Inst = cast<Instruction>(Op->getValue());
    VASTMemoryBank *Bus = PRT.getMemBus(Inst);
    assert(Bus && "Port reservation table is broken?");
    VASTLatch RData = Op->getSrc(0);
    VASTValPtr TimedRData = VM.createSeqValue(Bus->getRData(1), 0, Inst);

    VASTSeqValue *Dst = RData.getDst();
    VASTValPtr V = Builder.buildBitExtractExpr(TimedRData, Dst->getBitWidth(), 0);
    return R.latchValue(Dst, V, ToSlot, Op->getGuard(), Inst,
                         Op->getCyclesFromLaunch());
  }

  unsigned CurSrcIdx = 0;
  unsigned NumSrcs = Op->num_srcs();

  VASTSeqInst *NewInst = R.lauchInst(ToSlot, Op->getGuard(), NumSrcs,
                                     Op->getValue(), Op->isLatch());
  VASTLatch Addr = Op->getSrc(CurSrcIdx);
  VASTSelector *AddrPort = Addr.getSelector();
  VASTMemoryBank *Bus = cast<VASTMemoryBank>(AddrPort->getParent());
  NewInst->addSrc(Addr, CurSrcIdx++, Bus->getAddr(1));

  if (isa<StoreInst>(Op->getValue())) {
    VASTValPtr Data = Op->getSrc(CurSrcIdx);
    Data = Builder.buildZExtExprOrSelf(Data, Bus->getDataWidth());
    NewInst->addSrc(Data, CurSrcIdx++, Bus->getWData(1));
  }

  if (Bus->requireByteEnable()) {
    // Remap the byte enable and the enable.
    VASTValPtr ByteEn = Op->getSrc(CurSrcIdx);
    NewInst->addSrc(ByteEn, CurSrcIdx++, Bus->getByteEn(1));
  }

  return NewInst;
}

//===----------------------------------------------------------------------===//
VASTSeqInst *ScheduleEmitter::cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot) {
  SmallVector<VASTValPtr, 4> RetimedOperands;

  // Combine the guarding conditions
  VASTValPtr Cnd = Op->getGuard();

  // Some times we may even try to fold the BB through a 'false path' ... such
  // folding can be safely skipped.
  if (Cnd == VASTConstant::False) {
    ++NumFalsePathSkip;
    return 0;
  }

  Value *V = Op->getValue();

  if (Instruction *Inst = dyn_cast<Instruction>(V)) {
    if (isLoadStore(Inst) && PRT.getOrCreateMapping(Op))
      return remapToPort1(Op, ToSlot);

    // Find the subgroup for the PHI node. It is supposed to be existed because we
    // expected the branch operation is emitted prior to the PNI node.
    if (PHINode *PN = dyn_cast<PHINode>(V))
      ToSlot = getOrCreateSubGroup(PN->getParent(), Cnd, ToSlot);
  }

  VASTSeqInst *NewInst = R.lauchInst(ToSlot, Cnd, Op->num_srcs(),
                                      Op->getValue(), Op->isLatch());
  typedef VASTSeqOp::op_iterator iterator;

  for (unsigned i = 0, e = Op->num_srcs(); i < e; ++i) {
    const VASTLatch &L = Op->getSrc(i);
    VASTSeqValue *Dst = L.getDst();
    VASTValPtr Src = L;
    VASTSelector *Sel = L.getSelector();
    NewInst->addSrc(Src, i, Sel, Dst);
  }

  if (NewInst->isLatch())
    NewInst->setCyclesFromLaunch(Op->getCyclesFromLaunch());

  return NewInst;
}

//===----------------------------------------------------------------------===//
void ScheduleEmitter::handleNewSeqOp(VASTSeqInst *SeqOp) {}

void ScheduleEmitter::handleNewSeqOp(VASTSlotCtrl *SeqOp) {}

void
ScheduleEmitter::emitToSlot(VASTSeqOp *Op, VASTSlot *ToSlot) {
  // Create the new SeqOp.
  switch (Op->getASTType()) {
  case VASTNode::vastSeqInst:
    cloneSeqInst(cast<VASTSeqInst>(Op), ToSlot);
    break;
  case VASTNode::vastSlotCtrl:
    cloneSlotCtrl(cast<VASTSlotCtrl>(Op), ToSlot);
    break;
  default: llvm_unreachable("Unexpected SeqOp type!");
  }
}

bool ScheduleEmitter::emitToFirstSlot(VASTSlot *ToSlot,
                                      MutableArrayRef<VASTSchedUnit*> SUs) {
  assert(SUs[0]->isBBEntry() && "BBEntry not placed at the beginning!");
  unsigned EntrySlot = SUs[0]->getSchedule();

  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *SU = SUs[i];

    // Only emit the SUs in the same slot with the entry.
    if (SU->getSchedule() != EntrySlot)
      return true;

    emitToSlot(SU->getSeqOp(), ToSlot);
  }

  return false;
}

void ScheduleEmitter::emitScheduleInBB(MutableArrayRef<VASTSchedUnit*> SUs) {
  assert(SUs[0]->isBBEntry() && "BBEntry not placed at the beginning!");
  unsigned EntrySchedSlot = SUs[0]->getSchedule();
  // All SUs are scheduled to the same slot with the entry, hence they are all
  // folded to the predecessor of this BB.
  if (SUs.back()->getSchedule() == EntrySchedSlot)
    return;

  BasicBlock *BB = SUs[0]->getParent();

  VASTSlot *CurSlot = LandingSlots[BB];
  assert(CurSlot && "Landing Slot not created?");
  unsigned EntrySlotNum = CurSlot->SlotNum;
  unsigned EmittedSlotNum = EntrySlotNum;
  
  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *CurSU = SUs[i];

    unsigned CurSchedSlot = CurSU->getSchedule();

    // Do not emit the scheduling units at the first slot of the BB. They had
    // already folded in the the last slot of its predecessors.
    if (CurSchedSlot == EntrySchedSlot) continue;

    // Please note that the EntrySlot is actually folded into its predecessor's
    // last slot, hence we need to minus EntrySlot by 1
    unsigned CurSlotNum = EntrySlotNum + CurSchedSlot - EntrySchedSlot - 1;
    // Create the slot if it is not created.
    while (CurSlotNum != EmittedSlotNum) {
      ++EmittedSlotNum;
      VASTSlot *NextSlot = createSlot(BB, EntrySchedSlot + EmittedSlotNum - EntrySlotNum + 1);
      addSuccSlot(CurSlot, NextSlot, VASTConstant::True);
      CurSlot = NextSlot;
    }

    assert(CurSlot->Schedule == CurSchedSlot && "Schedule not match!");
    emitToSlot(CurSU->getSeqOp(), CurSlot);
  }
}


void ScheduleEmitter::emitSchedule() {
  Function &F = VM.getLLVMFunction(); //*R.getFunction();
  BasicBlock &Entry = F.getEntryBlock();
  G.sortSUs(top_sort_schedule_wrapper);

  OldSlots.splice(OldSlots.begin(), R.getSLotList(),
                  R.slot_begin(), R.slot_end());
  // Remove the successors of the start slot, we will reconstruct them.
  R.createLandingSlot();
  VASTSlot *StartSlot = R.getStartSlot();
  StartSlot->unlinkSuccs();

  VASTSlot *OldStart = OldSlots.begin();

  // Create the virtual slot representing the idle loop.
  VASTValue *StartPort
    = cast<VASTInPort>(VM.getPort(VASTModule::Start)).getValue();
  VASTSlot *IdleSlotGrp
    = getOrCreateSubGroup(0, Builder.buildNotExpr(StartPort), StartSlot);
  addSuccSlot(IdleSlotGrp, StartSlot, Builder.buildNotExpr(StartPort));

  // Create the virtual slot represent the entry of the CFG.
  VASTSlot *EntryGrp = getOrCreateSubGroup(&Entry, StartPort, StartSlot);

  // Emit the copy operation for argument registers.
  VASTSlot *OldEntryGrp = OldStart->getSubGroup(&Entry);
  typedef VASTSlot::op_iterator op_iterator;
  for (op_iterator I = OldEntryGrp->op_begin(); I != OldEntryGrp->op_end(); ++I)
    if (VASTSeqInst *SeqOp = dyn_cast<VASTSeqInst>(*I))
      if (isa<Argument>(SeqOp->getValue()))
        cloneSeqInst(SeqOp, EntryGrp);

#ifndef NDEBUG
  // Mark the old slots.
  for (ilist<VASTSlot>::iterator I = OldSlots.begin(), E = OldSlots.end();
    I != E; ++I)
    I->setDead();
#endif

  MutableArrayRef<VASTSchedUnit*> EntrySUs = G.getSUInBB(&Entry);

  if (emitToFirstSlot(EntryGrp, EntrySUs)) {
    // Create the landing slot of entry BB if not all SUs in the Entry BB
    // emitted to the idle slot.
    unsigned EntrySchedule = EntrySUs.front()->getSchedule();
    VASTSlot *S = getOrCreateLandingSlot(&Entry, EntrySchedule + 1);
    // Go to the new slot if the start port is true.
    addSuccSlot(EntryGrp, S, StartPort);
  }
  // Visit the basic block in topological order.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    BasicBlock *BB = *I;

    if (!G.isBBReachable(BB))
      continue;

    MutableArrayRef<VASTSchedUnit*> SUs(G.getSUInBB(BB));
    emitScheduleInBB(SUs);
  }
}

//===----------------------------------------------------------------------===//
namespace {
class ImplicitFlowBuilder {
  VASTCtrlRgn &R;
  STGDistanceBase *ShortesPaths;

  std::map<VASTSlot*, std::set<VASTSlot*> > ImplicitEdges;

  void buildImplicitFlow(VASTSlot *S);
  void buildImplicitFlow(VASTSlot *S, ArrayRef<VASTSlot*> StraightFlow);

public:
  explicit ImplicitFlowBuilder(VASTCtrlRgn &R) : R(R),
    ShortesPaths(STGDistanceBase::CalculateShortestPathDistance(R)) {}

  ~ImplicitFlowBuilder() { delete ShortesPaths; }

  void run();
};
}

static bool isPathMatch(VASTSlot *LHS, VASTSlot *RHS) {
  do {
    LHS = LHS->getParentGroup();
    RHS = RHS->getParentGroup();

    if (LHS->getParent() != RHS->getParent())
      return false;

  } while (LHS->IsSubGrp && RHS->IsSubGrp);

  return true;
}

static bool IsGuardCompatible(VASTSlot *LHS, VASTSlot *RHS) {
  // Only a SubGrp is guarded by something.
  if (!LHS->IsSubGrp) return false;

  // Else the State is the first State reachable from this SubGrp via the
  // predecessors tree.
  VASTSlot *S = LHS;
  while (S->pred_size() == 1 && S->IsSubGrp) {
    // Ok, we reach BB bottom-up in the subgrp path, so the subgrp is guarded
    // by the same predicate that guard BB.
    if (S->getParent() == RHS->getParent())
      return isPathMatch(S, RHS);

    VASTSlot *PredSlot = S->getParentGroup();
    S = PredSlot;
  }

  return false;
}

void ImplicitFlowBuilder::buildImplicitFlow(VASTSlot *S,
                                            ArrayRef<VASTSlot*> StraightFlow) {
  DEBUG(dbgs() << "Starting from #" << S->SlotNum << '\n');
  assert(S->IsSubGrp && "Expect subgroup as root!");

  typedef df_iterator<VASTSlot*> slot_df_iterator;
  for (slot_df_iterator DI = df_begin(S), DE = df_end(S); DI != DE; /*++DI*/) {
    VASTSlot *Child = *DI;

    if (Child == S) {
      ++DI;
      continue;
    }

    unsigned SPDistance = ShortesPaths->getDistance(S->SlotNum, Child->SlotNum);

    // Ignore the slots that not overlap with any of the slot in StraightFlow.
    if (SPDistance >= StraightFlow.size()) {
      DI.skipChildren();
      continue;
    }

    ++DI;

    if (Child->IsSubGrp) continue;

    assert(SPDistance && "Bad distance to child state!");
    VASTSlot *Src = StraightFlow[SPDistance];

    // FIXME: Assert that the parent BB of Src can reach the parent BB of Child
    // in the control flow.
    typedef VASTSlot::subgrp_iterator subgrp_iterator;
    for (subgrp_iterator SI = Src->subgrp_begin(), SE = Src->subgrp_end();
          SI != SE; ++SI) {
      VASTSlot *SubGrp = *SI;

      if (SubGrp == Src || IsGuardCompatible(S, SubGrp))
        ImplicitEdges[SubGrp].insert(Child);
    }

    DEBUG(dbgs() << "Overlap: #" << Src->SlotNum << " -> #"
                 << Child->SlotNum << " distance: " << SPDistance << '\n');
  }
}

void ImplicitFlowBuilder::buildImplicitFlow(VASTSlot *S) {
  // Build the straight line control flow.
  SmallVector<VASTSlot*, 8> StraightFlow;
  SmallVector<VASTSlot*, 8> ConditionalBroundaries;

  DEBUG(dbgs() << "Handling #" << S->SlotNum << '\n';);

  typedef df_iterator<VASTSlot*> slot_df_iterator;
  for (slot_df_iterator DI = df_begin(S), DE = df_end(S); DI != DE; /*++DI*/) {
    VASTSlot *Child = *DI;

    // The edges to the successors of a subgroup are all conditional.
    // TODO: Do not skip the unconditional subgroup.
    if (Child->IsSubGrp && Child != S) {
      // Collect the (conditional) successor of S.
      if (DI.getPathLength() == 2)
        ConditionalBroundaries.push_back(Child);

      DI.skipChildren();
      continue;
    }

    // Now child is a part of the straight line control flow.
    StraightFlow.push_back(Child);
    ++DI;
  }

  DEBUG(for (unsigned i = 0, e = StraightFlow.size(); i != e; ++i)
    dbgs() << "  Straight: " << (i + 1) << " #"
           << StraightFlow[i]->SlotNum << '\n';);

  // Build the overlapped map from the target of side branch.
  while (!ConditionalBroundaries.empty())
    buildImplicitFlow(ConditionalBroundaries.pop_back_val(), StraightFlow);

  DEBUG(dbgs() << "\n\n");
}

static bool HasSideBranch(VASTSlot *S) {
  bool HasNonVirtualSucc = false;
  bool HasVirtualSucc = false;
  typedef VASTSlot::succ_iterator iterator;
  for (iterator I = S->succ_begin(), E = S->succ_end(); I != E; ++I) {
    VASTSlot *Succ = *I;
    // FIXME: Assert Non-virtual edges must have a non-zero distance!
    HasVirtualSucc |= Succ->IsSubGrp;
    HasNonVirtualSucc |= !Succ->IsSubGrp;
  }

  return HasNonVirtualSucc && HasVirtualSucc;
}

void ImplicitFlowBuilder::run() {
  // 1. Find all side-branching slot like this:
  //  S1
  //  | \
  //  S2 S3
  // Where edge (S1, S2) is unconditional while edge (S1, S2) is conditional.

  typedef VASTCtrlRgn::slot_iterator slot_iterator;
  for (slot_iterator I = R.slot_begin(), E = R.slot_end(); I != E; ++I) {
    VASTSlot *S = I;

    if (HasSideBranch(S)) buildImplicitFlow(S);
  }

  typedef std::map<VASTSlot*, std::set<VASTSlot*> >::iterator iterator;
  for (iterator I = ImplicitEdges.begin(), E = ImplicitEdges.end(); I != E; ++I)
  {
    VASTSlot *Src = I->first;
    std::set<VASTSlot*> &Dsts = I->second;
    typedef std::set<VASTSlot*>::iterator dst_iterator;
    for (dst_iterator DI = Dsts.begin(), DE = Dsts.end(); DI != DE; ++DI)
      Src->addSuccSlot(*DI, VASTSlot::ImplicitFlow);
  }
}

//===----------------------------------------------------------------------===//
//
// If there is only trivial path (the path with zero delay) between two register
// assignments, the scheduler may schedule them to the same state, e.g. :
//   R1 <= F1
//   R2 <= R1
// In this case, we need to fold these two assignment into one:
//   R2 <= F1
//
// When the fine-grain CFG scheduling is enabled, the register folding become
// complicated: R1 <= F1 and R2 <= R1 may schedule to two different states that
// *may* overlap. We should also handle this case.
//
//===----------------------------------------------------------------------===//

namespace {
struct RegisterFolding : public MinimalExprBuilderContext {
  VASTCtrlRgn &R;
  DominatorTree *DT;
  VASTExprBuilder Builder;

  RegisterFolding(VASTModule &VM, VASTCtrlRgn &R, DominatorTree *DT)
    : MinimalExprBuilderContext(VM), R(R), DT(DT), Builder(*this) {}
  ~RegisterFolding() { }

  void run();
  bool retimeFannin(VASTUse &U, VASTSlot *S);

  VASTValPtr retimeLeaf(VASTValue *V, VASTSlot *S);
  VASTValPtr retimeExpr(VASTValue *V, VASTSlot *S);
  VASTValPtr retimeExprPtr(VASTValPtr V, VASTSlot *S) {
    VASTValue *Val = V.get();
    VASTValPtr RetimedV = retimeExpr(Val, S);
    if (V.isInverted()) RetimedV = Builder.buildNotExpr(RetimedV);
    return RetimedV;
  }
};
}

//===----------------------------------------------------------------------===//
static VASTSlot *getRetimingSlot(VASTSeqOp *Op) {
  VASTSlot *S = Op->getSlot();

  Value *V = Op->getValue();

  if (V == 0) return S;

  // The operands of branching operations and PHI latching operations should be
  // retimed against the parent state of the subgroup that they are assigned to.
  // Because only these two kind of operations are actually "conditional"
  // execution, while other conditional operations are just a side-effect of
  // CFG folding.
  if (VASTSlotCtrl *Ctrl = dyn_cast<VASTSlotCtrl>(Op)) {
    // Ignore the trivial case.
    if (!Ctrl->isBranch() || isa<ReturnInst>(V) || isa<UnreachableInst>(V))
      return S;
  } else if (!isa<PHINode>(V)) {
    // Ignore the trivial case for the VASTSeqInst.
    return S;
  }

  // Get the parent group of the current subgroup, to prevent us from
  // unnecessary retiming.
  return S->getParentGroup();
}

static bool replaceIfDifferent(VASTUse &U, VASTValPtr V) {
  if (U == V) return false;

  U.replaceUseBy(V);
  return true;
}

static bool operandMaybeChained(VASTSeqOp *Op) {
  if (!isChainingCandidate(Op->getValue()))
    return false;

  if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(Op))
    return SeqInst->isLaunch();

  return false;
}

static VASTSlot *GetUnquineSuccessor(VASTSlot *S) {
  assert(S->succ_size() == 1 && "Unexpected successor number!");
  return *S->succ_begin();
}

void RegisterFolding::run() {
  typedef VASTCtrlRgn::slot_iterator slot_iterator;
  // Retime the guarding conditions.
  for (slot_iterator I = R.slot_begin(), E = R.slot_end(); I != E; ++I) {
    VASTSlot *S = I;

    if (S->IsSubGrp) continue;

    assert(S->getGuard() == VASTConstant::True && "Unexpected guarded state!");

    // Share the signal to the virtual slots, because the virtual slot reachable
    // from this slot without visiting any non-virtual slots are sharing the
    // same state in the STG with the current slot.
    typedef VASTSlot::subgrp_iterator subgrp_iterator;
    for (subgrp_iterator SI = S->subgrp_begin(), SE = S->subgrp_end();
         SI != SE; ++SI) {
      VASTSlot *Child = *SI;
      if (Child == S) continue;

      VASTUse &Guard = Child->getGuard();
      // The retiming target group is the parent group of the current group,
      // instead of the current group. This prevent us from retiming the
      // guarding of the current group across the assignments in the current
      // subgroup, which themselves is guarded by the current guarding condition
      // ... Hence, there is logically a infinite loop if we do so ...
      VASTSlot *ParentGrp = Child->getParentGroup();

      // Retime the guarding condition of the current subgroup by using the
      // values produced by the parent subgroups.
      VASTValPtr NewGuard = retimeExprPtr(Guard, ParentGrp);
      // AND the conditions together to create the guarding condition to guard
      // the operation in the whole state.
      NewGuard = Builder.buildAndExpr(NewGuard, ParentGrp->getGuard(), 1);
      replaceIfDifferent(Guard, NewGuard);
    }
  }

  // Retime the Operands.
  typedef VASTCtrlRgn::seqop_iterator iterator;
  for (iterator I = R.seqop_begin(), E = R.seqop_end(); I != E; ++I) {
    VASTSeqOp *Op = I;
    VASTUse &Guard = Op->getGuard();
    VASTValPtr NewGuard = Op->getSlot()->getGuard();
    replaceIfDifferent(Guard, NewGuard);

    VASTSlot *DstSlot = getRetimingSlot(Op);
    bool OperandMaybeChained = operandMaybeChained(Op);

    for (unsigned i = 0, e = Op->num_srcs(); i != e; ++i) {
      VASTLatch L = Op->getSrc(i);
      if (VASTSelector *Sel = L.getSelector()) {
        if (Sel->isTrivialFannin(L))
          continue;

        // Replace chained operand.
        if (OperandMaybeChained) {
          assert(Sel->isFUInput() && "Unexpected operand type!");
          VASTSlot *Succ = GetUnquineSuccessor(DstSlot);
          VASTValPtr CurrentOperand = L;
          VASTValPtr ChainedOperand = retimeExprPtr(CurrentOperand, Succ);
          if (CurrentOperand != ChainedOperand) {
            Builder.replaceAllUseWith(L.getDst(), ChainedOperand);
            continue;
          }
        }

        // Retime the fannins of the selectors.
        retimeFannin(L, DstSlot);
      }
    }
  }
}

bool RegisterFolding::retimeFannin(VASTUse &U, VASTSlot *S) {
  VASTValPtr NewVal = retimeExprPtr(U, S);

  return replaceIfDifferent(U, NewVal);
}

VASTValPtr RegisterFolding::retimeExpr(VASTValue *Root, VASTSlot *S) {
  std::map<VASTValue*, VASTValPtr> RetimedMap;
  std::set<VASTValue*> Visited;

  VASTExpr *RootExpr = dyn_cast<VASTExpr>(Root);

  // The Root is already the leaf of the expr tree.
  if (RootExpr == 0)
    return retimeLeaf(Root, S);

  typedef VASTOperandList::op_iterator ChildIt;
  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(RootExpr, RootExpr->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == Node->op_end()) {
      VisitStack.pop_back();

      bool AnyOperandRetimed = false;
      SmallVector<VASTValPtr, 8> RetimedOperands;

      // Collect the possible retimed operands.
      for (ChildIt I = Node->op_begin(), E = Node->op_end(); I != E; ++I) {
        VASTValPtr Operand = *I;
        VASTValPtr RetimedOperand = RetimedMap[Operand.get()];
        if (Operand.isInverted())
          RetimedOperand = Builder.buildNotExpr(RetimedOperand);
        AnyOperandRetimed |= RetimedOperand != Operand;

        RetimedOperands.push_back(RetimedOperand);
      }

      // Rebuild the expression if any of its operand retimed.
      VASTValPtr RetimedExpr = Node;
      if (AnyOperandRetimed)
        RetimedExpr = Builder.copyExpr(Node, RetimedOperands);

      bool inserted
        = RetimedMap.insert(std::make_pair(Node, RetimedExpr)).second;
      assert(inserted && "Expr had already retimed?");
      (void) inserted;

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTExpr *ChildExpr = dyn_cast<VASTExpr>(ChildNode)) {
      // ChildNode has a name means we had already visited it.
      if (!Visited.insert(ChildExpr).second) continue;

      VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));
      continue;
    }

    // Retime the leaf if it is not retimed yet.
    VASTValPtr &Retimed = RetimedMap[ChildNode];
    if (!Retimed) Retimed = retimeLeaf(ChildNode, S);
  }

  VASTValPtr RetimedRoot = RetimedMap[RootExpr];
  assert(RetimedRoot && "RootExpr not visited?");
  return RetimedRoot;
}

// return true if the guarding condition of LHS is compatible with the
// guarding condition of RHS, false otherwise.
static bool isReachable(VASTSlot *LHS, VASTSlot *RHS) {
  // Handle the trivial case trivially.
  if (LHS == RHS) return true;

  // Perform depth first search to check if we can reach RHS from LHS with
  // 0-distance edges.
  SmallPtrSet<VASTSlot*, 8> Visited;
  SmallVector<std::pair<VASTSlot*, VASTSlot::succ_iterator>, 4> WorkStack;
  WorkStack.push_back(std::make_pair(LHS, LHS->succ_begin()));

  while (!WorkStack.empty()) {
    VASTSlot *S = WorkStack.back().first;
    VASTSlot::succ_iterator ChildIt = WorkStack.back().second;

    if (ChildIt == S->succ_end()) {
      WorkStack.pop_back();
      continue;
    }

    VASTSlot::EdgePtr Edge = *ChildIt;
    ++WorkStack.back().second;
    VASTSlot *Child = Edge;

    // Skip the children require 1-distance edges to be reached.
    if (Edge.getDistance()) continue;

    // Now we reach RHS!
    if (Child == RHS) return true;

    // Do not visit a node twice.
    if (!Visited.insert(Child)) continue;

    WorkStack.push_back(std::make_pair(Child, Child->succ_begin()));
  }

  return false;
}

VASTValPtr RegisterFolding::retimeLeaf(VASTValue *V, VASTSlot *S) {
  // TODO: Check the predicate of the assignment.
  VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(V);

  if (SeqVal == 0) return V;

  // Try to forward the value which is assigned to SeqVal at the same slot.
  SmallVector<VASTLatch, 4> Incomings;

  typedef VASTSeqValue::fanin_iterator iterator;
  for (iterator I = SeqVal->fanin_begin(), E = SeqVal->fanin_end();
       I != E; ++I) {
    const VASTLatch &U = *I;

    // Avoid infinite retiming ...
    // if (U.Op == Op) continue;

    // Only retime across the latch operation.
    if (cast<VASTSeqInst>(U.Op)->isLaunch()) continue;

    // Wrong slot to retime?
    if (!isReachable(U.getSlot(), S)) {
      ++NumRejectedRetiming;
      continue;
    }

    assert(DT->dominates(U.getSlot()->getParent(), S->getParent())
           && "Reg def does not dominate its use!");

    DEBUG(dbgs() << "Goning to forward " /*<< VASTValPtr(U) << ", "*/
                 << *U.Op->getValue());

    Incomings.push_back(U);

    assert(U->getBitWidth() == V->getBitWidth()
           && "Bitwidth implicitly changed!");
    ++NumRetimed;
  }

  if (Incomings.empty())
    return V;

  if (Incomings.size() == 1)
    return Incomings.front();

  ++NumMultRetimeIncoming;
  // Else build the selection logic to select the incoming value based on the
  // guarding condition.
  SmallVector<VASTValPtr, 4> GuardedIncomings;
  unsigned Bitwidth = V->getBitWidth();

  typedef SmallVectorImpl<VASTLatch>::iterator incoming_iterator;
  for (incoming_iterator I = Incomings.begin(), E = Incomings.end();
       I != E; ++I) {
    const VASTLatch &L = *I;

    // In case of multiple incoming, we also need the guarding conditions
    VASTValPtr FIMask = Builder.buildBitRepeat(L.getGuard(), Bitwidth);

    // Push the incoming value itself.
    VASTValPtr GuardedFIVal = Builder.buildAndExpr(L, FIMask, Bitwidth);
    GuardedIncomings.push_back(GuardedFIVal);
  }

  return Builder.buildOrExpr(GuardedIncomings, Bitwidth);
}

//===----------------------------------------------------------------------===//

void VASTScheduling::emitSchedule() {
  G->finalizeScheduling(DT);

  // The selectors will become invalid after we regenerate the STG, drop them
  // now.
  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM->selector_begin(), E = VM->selector_end(); I != E; ++I)
    I->dropMux();

  ScheduleEmitter(*VM, *VM, *G).emitSchedule();
  ImplicitFlowBuilder(*VM).run();
  RegisterFolding(*VM, *VM, DT).run();
}
