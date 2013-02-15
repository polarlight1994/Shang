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

#include "shang/VASTExprBuilder.h"
#include "shang/VASTModule.h"

#include "llvm/IR/Function.h"
#include "llvm/ADT/STLExtras.h"
#define DEBUG_TYPE "shang-schedule-emitter"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
class ScheduleEmitter : public MinimalExprBuilderContext {
  VASTExprBuilder Builder;
  VASTModule &VM;
  ilist<VASTSlot> OldSlots;
  VASTSchedGraph &G;

  std::map<BasicBlock*, VASTSlot*> LandingSlots;

  SUBBMap BBMap;

  void takeOldSlots();

  void clearUp();
  void clearUp(VASTSlot *S);
  void clearUp(VASTSeqValue *V);

  VASTValPtr getValAtSlot(VASTValue *V, VASTSlot *ToSlot);

  VASTValPtr retimeDatapath(VASTValue *V, VASTSlot *ToSlot);

  VASTValPtr retimeDatapath(VASTValPtr V, VASTSlot *ToSlot) {
    VASTValue *Val = V.get();
    VASTValPtr RetimedV = retimeDatapath(Val, ToSlot);
    if (V.isInverted()) RetimedV = Builder.buildNotExpr(RetimedV);
    return RetimedV;
  }

  void addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd,
                   BasicBlock *DstBB = 0) {
    // If the Br is already exist, simply or the conditions together.
    if (VASTSlotCtrl *SlotBr = S->getBrToSucc(NextSlot)) {
      VASTValPtr Pred = SlotBr->getPred();
      SlotBr->getPred().replaceUseBy(Builder.buildOrExpr(Pred, Cnd, 1));
      if (DstBB) SlotBr->annotateValue(DstBB);
    }

    S->addSuccSlot(NextSlot);
    VASTSlotCtrl *SlotBr = VM.createSlotCtrl(NextSlot, S, Cnd);
    if (DstBB) SlotBr->annotateValue(DstBB);
  }

  VASTSeqInst *cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot, VASTValPtr Pred);
  VASTSlotCtrl *cloneSlotCtrl(VASTSlotCtrl *Op, VASTSlot *ToSlot,
                              VASTValPtr Pred);

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

  void initialize();

  void emitSchedule();
};
}

ScheduleEmitter::ScheduleEmitter(VASTModule &VM, VASTSchedGraph &G)
  : MinimalExprBuilderContext(VM), Builder(*this), VM(VM), G(G) {}

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
}

void ScheduleEmitter::clearUp(VASTSeqValue *V) {
  if (!V->use_empty() || V->getValType() != VASTSeqValue::Data) return;

  SmallVector<VASTSeqOp*, 4> DeadOps;
  for (VASTSeqValue::itertor I = V->begin(), E = V->end(); I != E; ++I) {
    VASTSeqUse U = *I;
    DeadOps.push_back(U.Op);
  }

  while (!DeadOps.empty()) {
    VASTSeqOp *Op = DeadOps.pop_back_val();
    Op->removeFromParent();
    VM.eraseSeqOp(Op);
  }

  VM.eraseSeqVal(V);
}

void ScheduleEmitter::clearUp() {
  // Clear up the VASTSeqOp in the old list.
  while (!OldSlots.empty()) {
    VASTSlot *CurSlot = &OldSlots.back();

    clearUp(CurSlot);

    OldSlots.pop_back();
  }

  // Clear up the dead VASTSeqValues.
  typedef VASTModule::seqval_iterator seqval_iterator;
  for (seqval_iterator I = VM.seqval_begin(); I != VM.seqval_end(); /*++I*/)
    clearUp(I++);
}

//===----------------------------------------------------------------------===//
void ScheduleEmitter::takeOldSlots() {
  OldSlots.splice(OldSlots.begin(), VM.getSLotList(),
                  VM.slot_begin(), VM.slot_end());

  // Remove the successors of the start slot, we will reconstruct them.
  VM.createStartSlot();
  VASTSlot *StartSlot = VM.getStartSlot();
  VASTSlot *OldStartSlot = OldSlots.begin();
  StartSlot->unlinkSuccs();

  typedef VASTSlot::op_iterator op_iterator;
  for (op_iterator I = OldStartSlot->op_begin(); I != OldStartSlot->op_end(); ++I) {
    if (VASTSeqInst *SeqOp = dyn_cast<VASTSeqInst>(*I)) {
      if (isa<Argument>(SeqOp->getValue()))
        cloneSeqInst(SeqOp, StartSlot, SeqOp->getPred());
    }
  }

  VASTValPtr StartPort = VM.getPort(VASTModule::Start).getValue();
  addSuccSlot(StartSlot, StartSlot, Builder.buildNotExpr(StartPort));
}


static
int top_sort_schedule(const VASTSchedUnit *LHS, const VASTSchedUnit *RHS) {
  if (LHS->getSchedule() != RHS->getSchedule())
    return LHS->getSchedule() < RHS->getSchedule() ? -1 : 1;

  if (LHS->getIdx() < RHS->getIdx()) return -1;

  if (LHS->getIdx() > RHS->getIdx()) return 1;

  return 0;
}

static int top_sort_schedule_wrapper(const void *LHS, const void *RHS) {
  return top_sort_schedule(*reinterpret_cast<const VASTSchedUnit* const *>(LHS),
                           *reinterpret_cast<const VASTSchedUnit* const *>(RHS));
}

void ScheduleEmitter::initialize() {
  BBMap.buildMap(G);
  BBMap.sortSUs(top_sort_schedule_wrapper);

  takeOldSlots();

  // Assign the landing slot number for each BB.
  Function &F = VM;
  typedef Function::iterator iterator;

  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
  }
}

//===----------------------------------------------------------------------===//
VASTSlotCtrl *ScheduleEmitter::cloneSlotCtrl(VASTSlotCtrl *Op, VASTSlot *ToSlot,
                                             VASTValPtr Pred) {
  // Retime the predicate operand.
  Pred = Builder.buildAndExpr(retimeDatapath(Pred, ToSlot),
                              retimeDatapath(Op->getPred(), ToSlot),
                              1);
  VASTNode *N = Op->getNode();
  Value *V = Op->getValue();

  if (Op->isBranch()) {
    // Point to the slot in the new slotlist.
    if (isa<ReturnInst>(V) || isa<UnreachableInst>(V)) {
      N = VM.getFinishSlot();
      ToSlot->addSuccSlot(VM.getFinishSlot());
    } else {
      // Emit the the SUs in the first slot in the target BB.
      // Connect to the landing slot if not all SU in the target BB emitted to
      // current slot.
      llvm_unreachable("Not implemented!");
    }
  }

  VASTSlotCtrl *NewSlotCtrl = VM.createSlotCtrl(N, ToSlot, Pred);
  NewSlotCtrl->annotateValue(V);
  return NewSlotCtrl;
}

//===----------------------------------------------------------------------===//
VASTSeqInst *ScheduleEmitter::cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot,
                                           VASTValPtr Pred) {
  SmallVector<VASTValPtr, 4> RetimedOperands;
  // Retime all the operand to the specificed slot.
  typedef VASTOperandList::op_iterator iterator;
  for (iterator I = Op->op_begin(), E = Op->op_end(); I != E; ++I)
    RetimedOperands.push_back(retimeDatapath(*I, ToSlot));

  // Also retime the predicate.
  Pred = retimeDatapath(Pred, ToSlot);

  // And the predicate together.
  Pred = Builder.buildAndExpr(RetimedOperands[0], Pred, 1);

  VASTSeqInst *NewInst = VM.lauchInst(ToSlot, Pred, Op->getNumSrcs(),
                                      Op->getValue(), Op->getSeqOpType());
  typedef VASTSeqOp::op_iterator iterator;

  for (unsigned i = 0, e = Op->getNumSrcs(); i < e; ++i)
    NewInst->addSrc(RetimedOperands[1 + i], i, i < Op->getNumDefs(),
                    Op->getSrc(i).getDst());

  return NewInst;
}

VASTValPtr ScheduleEmitter::getValAtSlot(VASTValue *V, VASTSlot *ToSlot) {
  // TODO: Check the predicate of the assignment.
  VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(V);

  if (SeqVal == 0) return V;

  // Try to forward the value which is assigned to SeqVal at the same slot.
  VASTValPtr ForwardedValue = SeqVal;

  typedef VASTSeqValue::itertor iterator;
  for (iterator I = SeqVal->begin(), E = SeqVal->end(); I != E; ++I) {
    VASTSeqUse U = *I;

    // Only retime across the latch operation.
    if (cast<VASTSeqInst>(U.Op)->getSeqOpType() == VASTSeqInst::Latch
        && U.getSlot() == ToSlot) {
      assert(ForwardedValue == SeqVal && "Cannot resolve the source value!");
      ForwardedValue = U;
      assert(ForwardedValue->getBitWidth() == V->getBitWidth()
             && "Bitwidth implicitly changed!");
    }
  }

  return ForwardedValue;
}

VASTValPtr ScheduleEmitter::retimeDatapath(VASTValue *Root, VASTSlot *ToSlot) {
  std::map<VASTValue*, VASTValPtr> RetimedMap;
  std::set<VASTValue*> Visited;

  VASTExpr *RootExpr = dyn_cast<VASTExpr>(Root);

  // The Root is already the leaf of the expr tree.
  if (RootExpr == 0) return getValAtSlot(Root, ToSlot);

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

    if (!Retimed) Retimed = getValAtSlot(ChildNode, ToSlot);
  }

  VASTValPtr RetimedRoot = RetimedMap[RootExpr];
  assert(RetimedRoot && "RootExpr not visited?");
  return RetimedRoot;
}

//===----------------------------------------------------------------------===//
void ScheduleEmitter::handleNewSeqOp(VASTSeqInst *SeqOp) {

}

void ScheduleEmitter::handleNewSeqOp(VASTSlotCtrl *SeqOp) {}

void ScheduleEmitter::emitToSlot(VASTSeqOp *Op, VASTValPtr Pred,
                                 VASTSlot *ToSlot) {
  // Create the new SeqOp.
  switch (Op->getASTType()) {
  case VASTNode::vastSeqInst:
    handleNewSeqOp(cloneSeqInst(cast<VASTSeqInst>(Op), ToSlot, Pred));
    break;
  case VASTNode::vastSeqEnable:
    handleNewSeqOp(cloneSlotCtrl(cast<VASTSlotCtrl>(Op), ToSlot, Pred));
    break;
  default: llvm_unreachable("Unexpected SeqOp type!");
  }
}

bool ScheduleEmitter::emitToFirstSlot(VASTValPtr Pred, VASTSlot *ToSlot,
                                              MutableArrayRef<VASTSchedUnit*> SUs) {
  assert(SUs[0]->isBBEntry() && "BBEntry not placed at the beginning!");
  BasicBlock *ToBB = SUs[0]->getParent();
  unsigned EntrySlot = SUs[0]->getSchedule();

  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *SU = SUs[i];

    // Only emit the SUs in the same slot with the entry.
    if (SU->getSchedule() != EntrySlot) return true;

    emitToSlot(SU->getSeqOp(), Pred, ToSlot);
  }

  return false;
}

void ScheduleEmitter::emitScheduleInBB(MutableArrayRef<VASTSchedUnit*> SUs) {

  assert(SUs[0]->isBBEntry() && "BBEntry not placed at the beginning!");
  unsigned EntrySlot = SUs[0]->getSchedule();
  // All SUs are scheduled to the same slot with the entry, hence they are all
  // folded to the predecessor of this BB.
  if (SUs.back()->getSchedule() == EntrySlot) return;

  BasicBlock *BB = SUs[0]->getParent();

  unsigned LatestSlot = EntrySlot;
  VASTSlot *CurSlot = LandingSlots[BB];
  assert(CurSlot && "Landing Slot not created?");
  unsigned EntrySlotNum = CurSlot->SlotNum;

  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *CurSU = SUs[i];
    LatestSlot = CurSU->getSchedule();

    // Do not emit the scheduling units at the first slot of the BB. They had
    // already folded in the the last slot of its predecessors.
    if (LatestSlot == EntrySlot) continue;

    // Please note that the EntrySlot is actually folded into its predecessor's
    // last slot, hence we need to minus EntrySlot by 1
    unsigned CurSlotNum = EntrySlotNum + LatestSlot - EntrySlot - 1;
    // Create the slot if it is not created.
    while (CurSlotNum != CurSlot->SlotNum) {
      VASTSlot *NextSlot = VM.createSlot(CurSlot->SlotNum + 1, BB);
      addSuccSlot(CurSlot, NextSlot, VASTImmediate::True);
      CurSlot = NextSlot;
    }

    emitToSlot(CurSU->getSeqOp(), VASTImmediate::True, CurSlot);
  }
}


void ScheduleEmitter::emitSchedule() {
  Function &F = VM;

  BasicBlock &Entry = F.getEntryBlock();

  VASTValPtr StartPort = VM.getPort(VASTModule::Start).getValue();
  if (emitToFirstSlot(StartPort, VM.getStartSlot(), BBMap.getSUInBB(&Entry))) {
    // Create the landing slot of entry BB if not all SUs in the Entry BB
    // emitted to the idle slot.
    VASTSlot *S = VM.createSlot(1, &Entry);
    LandingSlots[&Entry] = S;
    // Go to the new slot if the start port is 1.
    addSuccSlot(VM.getStartSlot(), S, StartPort, &Entry);
  }

  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    MutableArrayRef<VASTSchedUnit*> SUs(BBMap.getSUInBB(BB));
    emitScheduleInBB(SUs);
  }
}

//===----------------------------------------------------------------------===//

void VASTSchedGraph::emitSchedule(VASTModule &VM) {
  ScheduleEmitter Emitter(VM, *this);

  Emitter.initialize();

  Emitter.emitSchedule();
}
