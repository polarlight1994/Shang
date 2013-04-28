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

#include "shang/VASTModulePass.h"
#include "shang/VASTExprBuilder.h"
#include "shang/VASTModule.h"

#include "llvm/IR/Function.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-schedule-emitter"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumBBByPassed, "Number of BasicBlock Bypassed by the CFG folding");
STATISTIC(NumRetimed, "Number of Retimed Value during Schedule Emitting");
STATISTIC(NumRejectedRetiming,
          "Number of Reject Retiming because the predicates are not compatible");
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

  // return true if the guarding condition of LHS is compatible with the
  // guarding condition of RHS, false otherwise.
  static bool isReachable(VASTSlot *LHS, VASTSlot *RHS) {
    typedef df_iterator<VASTSlot*> slot_df_iterator;
    for (slot_df_iterator DI = df_begin(LHS), DE = df_end(LHS);
         DI != DE; /*++DI*/) {
      VASTSlot *Child = *DI;

      // Skip all children when we reach a non-virtual slot, because we cannot
      // share the signal with them.
      if (!Child->IsVirtual && Child != LHS) {
        DI.skipChildren();
        continue;
      }

      // The guarding condition is compatible if LHS could reach RHS based on
      // the guarding condition relationship.
      if (Child == RHS) return true;

      ++DI;
    }

    return false;
  }

  VASTValPtr retimeValToSlot(VASTValue *V, VASTSlot *ToSlot);

  VASTValPtr retimeDatapath(VASTValue *V, VASTSlot *ToSlot);

  VASTValPtr retimeDatapath(VASTValPtr V, VASTSlot *ToSlot) {
    VASTValue *Val = V.get();
    VASTValPtr RetimedV = retimeDatapath(Val, ToSlot);
    if (V.isInverted()) RetimedV = Builder.buildNotExpr(RetimedV);
    return RetimedV;
  }

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
      assert(SubGrp->getPred() == Cnd && "Inconsistent guarding conditions!");
      return SubGrp;
    }

    // Create the subgroup if it does not exist.
    ++NumSubGropus;
    SubGrp = VM.createSlot(++CurrentSlotNum, BB, Cnd, true);
    S->addSuccSlot(SubGrp);
    return SubGrp;
  }

  VASTSlotCtrl *addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd,
                            Value *V = 0);

  VASTSeqInst *cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot, VASTValPtr Pred);

  VASTSlotCtrl *cloneSlotCtrl(VASTSlotCtrl *Op, VASTSlot *ToSlot, VASTValPtr Pred);

  /// Emit the scheduling units in the same BB.
  ///
  void emitScheduleInBB(MutableArrayRef<VASTSchedUnit*> SUs);

  bool emitToFirstSlot(VASTValPtr Pred, VASTSlot *ToSlot,
                       MutableArrayRef<VASTSchedUnit*> SUs);

  void emitToSlot(VASTSeqOp *Op, VASTValPtr Pred, VASTSlot *ToSlot);

  void handleNewSeqOp(VASTSeqInst *SeqOp);
  void handleNewSeqOp(VASTSlotCtrl *SeqOp);
public:
  ScheduleEmitter(VASTModule &VM, VASTSchedGraph &G);
  ~ScheduleEmitter() { clearUp(); }

  void emitSchedule();
};
}

ScheduleEmitter::ScheduleEmitter(VASTModule &VM, VASTSchedGraph &G)
  : MinimalExprBuilderContext(VM), Builder(*this), VM(VM), G(G),
    CurrentSlotNum(0) {}

//===----------------------------------------------------------------------===//
void ScheduleEmitter::clearUp(VASTSlot *S) {
  typedef VASTSlot::op_iterator op_iterator;
  for (op_iterator I = S->op_begin(); I != S->op_end(); ++I) {
    VASTSeqOp *SeqOp = *I;

    // Delete the dead Exprs used by this SeqOp.
    for (unsigned i = 0, e = SeqOp->size(); i != e; ++i) {
      VASTValue *V = SeqOp->getOperand(i).unwrap().get();
      SeqOp->getOperand(i).unlinkUseFromUser();

      if (!V->use_empty()) continue;

      if (VASTExpr *Child = dyn_cast<VASTExpr>(V))
        VM->recursivelyDeleteTriviallyDeadExprs(Child);
    }

    SeqOp->clearParent();
    VM.eraseSeqOp(*I);
  }

  // Drop the reference to the guarding condition.
  S->getPred().unlinkUseFromUser();
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

  S->addSuccSlot(NextSlot);
  VASTSlotCtrl *SlotBr = VM.createSlotCtrl(NextSlot, S, Cnd);
  if (V) SlotBr->annotateValue(V);

  return SlotBr;
}

VASTSlotCtrl *ScheduleEmitter::cloneSlotCtrl(VASTSlotCtrl *Op, VASTSlot *ToSlot,
                                             VASTValPtr Pred) {
  // Retime the predicate operand.
  Pred = Builder.buildAndExpr(retimeDatapath(Op->getPred(), ToSlot), Pred, 1);

  // Some times we may even try to fold the BB through a 'false path' ... such
  // folding can be safely skipped.
  if (Pred == VASTImmediate::False) {
    ++NumFalsePathSkip;
    return 0;
  }

  Value *V = Op->getValue();

  // Handle the trivial case
  if (!Op->isBranch()) {
    VASTSlotCtrl *NewSlotCtrl
      = VM.createSlotCtrl(Op->getNode(), ToSlot, Pred);
    NewSlotCtrl->annotateValue(V);
    return NewSlotCtrl;
  }

  if (isa<ReturnInst>(V) || isa<UnreachableInst>(V))
    return addSuccSlot(ToSlot, VM.getFinishSlot(), Pred, V);

  BasicBlock *TargetBB = Op->getTargetSlot()->getParent();
  VASTSlot *SubGrp = getOrCreateSubGroup(TargetBB, Pred, ToSlot);
  // Emit the the SUs in the first slot in the target BB.
  // Connect to the landing slot if not all SU in the target BB emitted to
  // current slot.
  if (emitToFirstSlot(Pred, SubGrp, G.getSUInBB(TargetBB))) {
    // There is some SeqOp need to be emitted to TargetBB, build the control
    // flow.
    VASTSlot *LandingSlot = getOrCreateLandingSlot(TargetBB);
    return addSuccSlot(SubGrp, LandingSlot, Pred, V);
  }

  // Else all scheduling unit of target block are emitted to current slot
  // do not emit the SlotCtrl because it is not needed.
  ++NumBBByPassed;
  return 0;
}

//===----------------------------------------------------------------------===//
VASTSeqInst *ScheduleEmitter::cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot,
                                           VASTValPtr Pred) {
  SmallVector<VASTValPtr, 4> RetimedOperands;

  // Retime the predicate operand.
  Pred = Builder.buildAndExpr(retimeDatapath(Op->getPred(), ToSlot), Pred, 1);

  // Some times we may even try to fold the BB through a 'false path' ... such
  // folding can be safely skipped.
  if (Pred == VASTImmediate::False) {
    ++NumFalsePathSkip;
    return 0;
  }

  // Retime all the operand to the specificed slot.
  typedef VASTOperandList::op_iterator iterator;
  for (iterator I = Op->src_begin(), E = Op->src_end(); I != E; ++I)
    RetimedOperands.push_back(retimeDatapath(*I, ToSlot));

  // Find the subgroup for the PHI node. It is supposed to be existed because we
  // expected the branch operation is emitted prior to the PNI node.
  if (PHINode *PN = dyn_cast<PHINode>(Op->getValue()))
    ToSlot = getOrCreateSubGroup(PN->getParent(), Pred, ToSlot);

  VASTSeqInst *NewInst = VM.lauchInst(ToSlot, Pred, Op->getNumSrcs(),
                                      Op->getValue(), Op->getSeqOpType());
  typedef VASTSeqOp::op_iterator iterator;

  for (unsigned i = 0, e = Op->getNumSrcs(); i < e; ++i) {
    VASTSeqValue *Dst = Op->getSrc(i).getDst();
    VASTValPtr Src = RetimedOperands[i];
    // Create a wrapper wire to break the cycle.
    if (Src == Dst) {
      unsigned BitWidth = Src->getBitWidth();
      Src = VM.createWrapperWire(Dst->getName(), BitWidth, Src);
    }

    NewInst->addSrc(Src, i, i < Op->getNumDefs(), Dst);
  }

#ifdef XDEBUG
  // Verify if we have the CFGPred conflict.
  std::set<std::pair<CFGPred*, VASTSlot*> > UniqueCFGPreds;
  if (NewInst->getNumDefs()) {
    assert(NewInst->getNumDefs() == 1 && "Unexpected multi-define SeqOp!");
    VASTSeqValue *S = NewInst->getDef(0);

    for (VASTSeqValue::iterator I = S->begin(), E = S->end(); I != E; ++I) {
      VASTLatch U = *I;
      std::map<VASTSeqInst*, CFGPred*>::iterator at
        = PredMap.find(cast<VASTSeqInst>(U.Op));
      if (at == PredMap.end()) continue;
      CFGPred *Pred = at->second;
      Pred->dump();
      assert(UniqueCFGPreds.insert(std::make_pair(at->second, U.getSlot())).second
             && "CFGPred conflict detected!");
    }
  }
#endif

  return NewInst;
}

VASTValPtr ScheduleEmitter::retimeValToSlot(VASTValue *V, VASTSlot *ToSlot) {
  // TODO: Check the predicate of the assignment.
  VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(V);

  if (SeqVal == 0) return V;

  // Try to forward the value which is assigned to SeqVal at the same slot.
  VASTValPtr ForwardedValue = SeqVal;

  typedef VASTSeqValue::iterator iterator;
  for (iterator I = SeqVal->begin(), E = SeqVal->end(); I != E; ++I) {
    VASTLatch U = *I;

    // Only retime across the latch operation.
    if (cast<VASTSeqInst>(U.Op)->getSeqOpType() != VASTSeqInst::Latch)
      continue;

    // Wrong slot to retime?
    if (!isReachable(U.getSlot(), ToSlot)) {
      ++NumRejectedRetiming;
      continue;
    }

    Value *Val = U.Op->getValue();

    DEBUG(dbgs() << "Goning to forward " /*<< VASTValPtr(U) << ", "*/
                 << *U.Op->getValue());

    assert (ForwardedValue == SeqVal && "Unexpected multiple compatible source!");
    ForwardedValue = VASTValPtr(U);

    assert(ForwardedValue->getBitWidth() == V->getBitWidth()
           && "Bitwidth implicitly changed!");
    ++NumRetimed;
  }

#ifndef NDEBUG
  if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(ForwardedValue.get())) {
    bool AnySrcEmitted = SV->empty();

    for (iterator I = SV->begin(), E = SV->end(); I != E; ++I) {
      AnySrcEmitted |= !(*I).getSlot()->isDead();
    }

    assert((AnySrcEmitted || SV->getValType() == VASTSeqValue::StaticRegister)
           && "Retiming performed before source value emitted!");
  }
#endif

  return ForwardedValue;
}

VASTValPtr ScheduleEmitter::retimeDatapath(VASTValue *Root, VASTSlot *ToSlot) {
  std::map<VASTValue*, VASTValPtr> RetimedMap;
  std::set<VASTValue*> Visited;

  VASTExpr *RootExpr = dyn_cast<VASTExpr>(Root);

  // The Root is already the leaf of the expr tree.
  if (RootExpr == 0) return retimeValToSlot(Root, ToSlot);

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
        RetimedExpr = Builder.buildExpr(Node->getOpcode(), RetimedOperands,
                                        Node->UB, Node->LB);

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
    if (!Retimed) Retimed = retimeValToSlot(ChildNode, ToSlot);
  }

  VASTValPtr RetimedRoot = RetimedMap[RootExpr];
  assert(RetimedRoot && "RootExpr not visited?");
  return RetimedRoot;
}

//===----------------------------------------------------------------------===//
void ScheduleEmitter::handleNewSeqOp(VASTSeqInst *SeqOp) {

}

void ScheduleEmitter::handleNewSeqOp(VASTSlotCtrl *SeqOp) {}

void
ScheduleEmitter::emitToSlot(VASTSeqOp *Op, VASTValPtr Pred, VASTSlot *ToSlot) {
  // Create the new SeqOp.
  switch (Op->getASTType()) {
  case VASTNode::vastSeqInst:
    cloneSeqInst(cast<VASTSeqInst>(Op), ToSlot, Pred);
    break;
  case VASTNode::vastSlotCtrl:
    cloneSlotCtrl(cast<VASTSlotCtrl>(Op), ToSlot, Pred);
    break;
  default: llvm_unreachable("Unexpected SeqOp type!");
  }
}

bool ScheduleEmitter::emitToFirstSlot(VASTValPtr Pred, VASTSlot *ToSlot,
                                      MutableArrayRef<VASTSchedUnit*> SUs) {
  assert(SUs[0]->isBBEntry() && "BBEntry not placed at the beginning!");
  unsigned EntrySlot = SUs[0]->getSchedule();

  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *SU = SUs[i];

    // Only emit the SUs in the same slot with the entry.
    if (SU->getSchedule() != EntrySlot) return true;

    // Ignore the pseudo scheduling units.
    if (SU->isPHI()) continue;

    emitToSlot(SU->getSeqOp(), Pred, ToSlot);
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

    emitToSlot(CurSU->getSeqOp(), VASTImmediate::True, CurSlot);
  }
}


void ScheduleEmitter::emitSchedule() {
  Function &F = VM;
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
  VASTValue *StartPort = VM.getPort(VASTModule::Start).getValue();
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
        cloneSeqInst(SeqOp, EntryGrp, SeqOp->getPred());

#ifndef NDEBUG
  // Mark the old slots.
  for (ilist<VASTSlot>::iterator I = OldSlots.begin(), E = OldSlots.end();
    I != E; ++I)
    I->setDead();
#endif

  MutableArrayRef<VASTSchedUnit*> EntrySUs = G.getSUInBB(&Entry);

  if (emitToFirstSlot(StartPort, EntryGrp, EntrySUs)) {
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

void VASTSchedGraph::emitSchedule(VASTModule &VM) {
  ScheduleEmitter Emitter(VM, *this);

  Emitter.emitSchedule();
}
