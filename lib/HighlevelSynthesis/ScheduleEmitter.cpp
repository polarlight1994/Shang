//===------ ScheduleEmitter.cpp - Emit the Schedule -------------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
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
#include "STGDistances.h"

#include "shang/VASTModulePass.h"
#include "shang/VASTExprBuilder.h"
#include "shang/VASTModule.h"

#include "llvm/IR/Function.h"
#include "llvm/Analysis/Dominators.h"
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

namespace {
class ScheduleEmitter : public MinimalExprBuilderContext {
  VASTExprBuilder Builder;
  VASTModule &VM;
  ilist<VASTSlot> OldSlots;
  VASTSchedGraph &G;
  std::map<BasicBlock*, VASTSlot*> LandingSlots;
  unsigned CurrentSlotNum;

  void clearUp();
  void clearUp(VASTSlot *S);

  VASTSlot *createSlot(BasicBlock *BB) {
    ++NumSlots;
    return VM.createSlot(++CurrentSlotNum, BB);
  }

  VASTSlot *getOrCreateLandingSlot(BasicBlock *BB) {
    VASTSlot *&S = LandingSlots[BB];
    if (S == 0) S = createSlot(BB);
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
    SubGrp = VM.createSlot(++CurrentSlotNum, BB, Cnd, true);
    // The subgroups are not actually the successors of S in the control flow.
    S->addSuccSlot(SubGrp, VASTSlot::SubGrp);
    return SubGrp;
  }

  VASTSlotCtrl *addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd,
                            Value *V = 0);

  VASTSeqInst *cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot);

  VASTSlotCtrl *cloneSlotCtrl(VASTSlotCtrl *Op, VASTSlot *ToSlot);

  /// Emit the scheduling units in the same BB.
  ///
  void emitScheduleInBB(MutableArrayRef<VASTSchedUnit*> SUs);

  bool emitToFirstSlot(VASTSlot *ToSlot, MutableArrayRef<VASTSchedUnit*> SUs);

  void emitToSlot(VASTSeqOp *Op, VASTSlot *ToSlot);

  void handleNewSeqOp(VASTSeqInst *SeqOp);
  void handleNewSeqOp(VASTSlotCtrl *SeqOp);
public:
  ScheduleEmitter(VASTModule &VM, VASTSchedGraph &G);
  ~ScheduleEmitter() { clearUp(); }

  void emitSchedule();
};
}

//===----------------------------------------------------------------------===//
ScheduleEmitter::ScheduleEmitter(VASTModule &VM, VASTSchedGraph &G)
  : MinimalExprBuilderContext(VM), Builder(*this), VM(VM), G(G),
    CurrentSlotNum(0) {}

void ScheduleEmitter::clearUp(VASTSlot *S) {
  typedef VASTSlot::op_iterator op_iterator;
  for (op_iterator I = S->op_begin(); I != S->op_end(); /*++I*/) {
    VASTSeqOp *SeqOp = *I;

    // Delete the dead Exprs used by this SeqOp.
    for (unsigned i = 0, e = SeqOp->size(); i != e; ++i) {
      VASTValue *V = SeqOp->getOperand(i).unwrap().get();

      SeqOp->getOperand(i).unlinkUseFromUser();

      if (!V->use_empty()) continue;

      if (VASTExpr *Child = dyn_cast<VASTExpr>(V))
        VM->recursivelyDeleteTriviallyDeadExprs(Child);
    }

    I = S->removeOp(I);
    VM.eraseSeqOp(SeqOp);
  }
}

void ScheduleEmitter::clearUp() {
  // Clear up the VASTSeqOp in the old list.
  while (!OldSlots.empty()) {
    VASTSlot *CurSlot = &OldSlots.back();

    clearUp(CurSlot);

    OldSlots.pop_back();
  }

  // Release the dead objects now.
  VM.gc();
}

//===----------------------------------------------------------------------===//
static
int top_sort_schedule(const VASTSchedUnit *LHS, const VASTSchedUnit *RHS) {
  if (LHS->getSchedule() != RHS->getSchedule())
    return LHS->getSchedule() < RHS->getSchedule() ? -1 : 1;

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

static int top_sort_schedule_wrapper(const void *LHS, const void *RHS) {
  return top_sort_schedule(*reinterpret_cast<const VASTSchedUnit* const *>(LHS),
                           *reinterpret_cast<const VASTSchedUnit* const *>(RHS));
}

//===----------------------------------------------------------------------===//
VASTSlotCtrl *ScheduleEmitter::addSuccSlot(VASTSlot *S, VASTSlot *NextSlot,
                                           VASTValPtr Cnd, Value *V) {
  // If the Br already exist, simply or the conditions together.
  assert(!S->hasNextSlot(NextSlot) && "Edge had already existed!");
  assert((S->getParent() == NextSlot->getParent()
          || NextSlot == VM.getFinishSlot())
        && "Cannot change Slot and BB at the same time!");

  assert(!NextSlot->IsSubGrp && "Unexpected subgroup!");
  S->addSuccSlot(NextSlot, VASTSlot::Sucessor);
  VASTSlotCtrl *SlotBr = VM.createSlotCtrl(NextSlot, S, Cnd);
  if (V) SlotBr->annotateValue(V);

  return SlotBr;
}

VASTSlotCtrl *
ScheduleEmitter::cloneSlotCtrl(VASTSlotCtrl *Op, VASTSlot *ToSlot) {
  // Combine the guarding conditions
  VASTValPtr Cnd = Op->getGuard();

  // Some times we may even try to fold the BB through a 'false path' ... such
  // folding can be safely skipped.
  if (Cnd == VASTImmediate::False) {
    ++NumFalsePathSkip;
    return 0;
  }

  Value *V = Op->getValue();

  // Handle the trivial case
  if (!Op->isBranch()) {
    VASTSlotCtrl *NewSlotCtrl
      = VM.createSlotCtrl(Op->getNode(), ToSlot, Cnd);
    NewSlotCtrl->annotateValue(V);
    return NewSlotCtrl;
  }

  if (isa<ReturnInst>(V) || isa<UnreachableInst>(V))
    return addSuccSlot(ToSlot, VM.getFinishSlot(), Cnd, V);

  BasicBlock *TargetBB = Op->getTargetSlot()->getParent();
  VASTSlot *SubGrp = getOrCreateSubGroup(TargetBB, Cnd, ToSlot);
  // Emit the the SUs in the first slot in the target BB.
  // Connect to the landing slot if not all SU in the target BB emitted to
  // current slot.
  if (emitToFirstSlot(SubGrp, G.getSUInBB(TargetBB))) {
    // There is some SeqOp need to be emitted to TargetBB, build the control
    // flow.
    VASTSlot *LandingSlot = getOrCreateLandingSlot(TargetBB);
    return addSuccSlot(SubGrp, LandingSlot, Cnd, V);
  }

  // Else all scheduling unit of target block are emitted to current slot
  // do not emit the SlotCtrl because it is not needed.
  ++NumBBByPassed;
  return 0;
}

//===----------------------------------------------------------------------===//
VASTSeqInst *ScheduleEmitter::cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot) {
  SmallVector<VASTValPtr, 4> RetimedOperands;

  // Combine the guarding conditions
  VASTValPtr Cnd = Op->getGuard();

  // Some times we may even try to fold the BB through a 'false path' ... such
  // folding can be safely skipped.
  if (Cnd == VASTImmediate::False) {
    ++NumFalsePathSkip;
    return 0;
  }

  // Find the subgroup for the PHI node. It is supposed to be existed because we
  // expected the branch operation is emitted prior to the PNI node.
  if (PHINode *PN = dyn_cast<PHINode>(Op->getValue()))
    ToSlot = getOrCreateSubGroup(PN->getParent(), Cnd, ToSlot);

  VASTSeqInst *NewInst = VM.lauchInst(ToSlot, Cnd, Op->num_srcs(),
                                      Op->getValue(), Op->getSeqOpType());
  typedef VASTSeqOp::op_iterator iterator;

  for (unsigned i = 0, e = Op->num_srcs(); i < e; ++i) {
    const VASTLatch &L = Op->getSrc(i);
    VASTSeqValue *Dst = L.getDst();
    VASTValPtr Src = L;
    NewInst->addSrc(Src, i, L.getSelector(), Dst);
  }

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
    if (SU->getSchedule() != EntrySlot) return true;

    // Ignore the pseudo scheduling units.
    if (SU->isPHI()) continue;

    emitToSlot(SU->getSeqOp(), ToSlot);
  }

  return false;
}

void ScheduleEmitter::emitScheduleInBB(MutableArrayRef<VASTSchedUnit*> SUs) {
  assert(SUs[0]->isBBEntry() && "BBEntry not placed at the beginning!");
  unsigned EntrySchedSlot = SUs[0]->getSchedule();
  // All SUs are scheduled to the same slot with the entry, hence they are all
  // folded to the predecessor of this BB.
  if (SUs.back()->getSchedule() == EntrySchedSlot) return;

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
      VASTSlot *NextSlot = createSlot(BB);
      ++EmittedSlotNum;
      addSuccSlot(CurSlot, NextSlot, VASTImmediate::True);
      CurSlot = NextSlot;
    }

    emitToSlot(CurSU->getSeqOp(), CurSlot);
  }
}


void ScheduleEmitter::emitSchedule() {
  Function &F = VM.getLLVMFunction();
  BasicBlock &Entry = F.getEntryBlock();

  G.sortSUs(top_sort_schedule_wrapper);

  OldSlots.splice(OldSlots.begin(), VM.getSLotList(),
                  VM.slot_begin(), VM.slot_end());
  // Remove the successors of the start slot, we will reconstruct them.
  VM.createStartSlot();
  VASTSlot *StartSlot = VM.getStartSlot();
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
    VASTSlot *S = getOrCreateLandingSlot(&Entry);
    // Go to the new slot if the start port is true.
    addSuccSlot(EntryGrp, S, StartPort);
  }

  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    MutableArrayRef<VASTSchedUnit*> SUs(G.getSUInBB(BB));
    emitScheduleInBB(SUs);
  }
}

//===----------------------------------------------------------------------===//
namespace {
class ImplicitFlowBuilder {
  DominatorTree *DT;
  VASTModule &VM;
  STGDistanceBase ShortesPaths;
  std::map<VASTSlot*, std::set<VASTSlot*> > ImplicitEdges;

  void buildImplicitFlow(VASTSlot *S);
  void buildImplicitFlow(VASTSlot *S, ArrayRef<VASTSlot*> StraightFlow);

public:
  ImplicitFlowBuilder(DominatorTree *DT, VASTModule &VM) : DT(DT), VM(VM),
    ShortesPaths(STGDistanceBase::CalculateShortestPathDistance(VM)) {}

  void run();
};
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

    unsigned SPDistance = ShortesPaths.getDistance(S->SlotNum, Child->SlotNum);

    // Ignore the slots that not overlap with any of the slot in StraightFlow.
    if (SPDistance >= StraightFlow.size()) {
      DI.skipChildren();
      continue;
    }

    ++DI;

    if (Child->IsSubGrp) continue;

    assert(SPDistance && "Bad distance to child state!");
    VASTSlot *Src = StraightFlow[SPDistance];

    if (!DT->dominates(Src->getParent(), Child->getParent())) continue;

    ImplicitEdges[Src].insert(Child);

    DEBUG(dbgs() << "Overlap: #" << Src->SlotNum << " -> #"
                 << Child->SlotNum << " distance: " << SPDistance << '\n');
    // ++NumOverlappeds;
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

  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator I = VM.slot_begin(), E = VM.slot_end(); I != E; ++I) {
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

void VASTSchedGraph::emitSchedule(VASTModule &VM, DominatorTree *DT) {
  ScheduleEmitter(VM, *this).emitSchedule();
  ImplicitFlowBuilder(DT, VM).run();
}
