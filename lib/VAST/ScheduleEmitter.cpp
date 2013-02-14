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

#include "MinimalDatapathContext.h"

#include "shang/ScheduleEmitter.h"
#include "shang/VASTModule.h"

#include "llvm/IR/Function.h"
#include "llvm/ADT/STLExtras.h"
#define DEBUG_TYPE "shang-schedule-emitter"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace llvm {

struct EmitterBuilderContext : public VASTExprBuilderContext {
  VASTModule &VM;

  EmitterBuilderContext(VASTModule &VM) : VM(VM) {}
  ~EmitterBuilderContext() {

  }

  using VASTExprBuilderContext::getOrCreateImmediate;

  VASTImmediate *getOrCreateImmediate(const APInt &Value) {
    return VM->getOrCreateImmediateImpl(Value);
  }

  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned UB, unsigned LB) {
    return VM->createExprImpl(Opc, Ops, UB, LB);
  }

  void replaceAllUseWith(VASTValPtr From, VASTValPtr To) {
    VM->replaceAllUseWithImpl(From, To);

    if (VASTExpr *E = dyn_cast<VASTExpr>(From.get()))
      VM->eraseExpr(E);
  }
};

class ScheduleEmitterImpl : public EmitterBuilderContext {
  VASTExprBuilder Builder;

  ilist<VASTSlot> OldSlots;
public:
  explicit ScheduleEmitterImpl(VASTModule &VM);

  void takeOldSlots();

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
    if (VASTSeqSlotCtrl *SlotBr = S->getBrToSucc(NextSlot)) {
      VASTValPtr Pred = SlotBr->getPred();
      SlotBr->getPred().replaceUseBy(Builder.buildOrExpr(Pred, Cnd, 1));
      if (DstBB) SlotBr->annotateValue(DstBB);
    }

    S->addSuccSlot(NextSlot);
    VASTSeqSlotCtrl *SlotBr = VM.createSlotCtrl(NextSlot->getValue(), S, Cnd,
                                                VASTSeqSlotCtrl::SlotBr);
    if (DstBB) SlotBr->annotateValue(DstBB);
  }

  VASTSeqInst *cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot, VASTValPtr Pred);
};
}


ScheduleEmitterImpl::ScheduleEmitterImpl(VASTModule &VM)
  : EmitterBuilderContext(VM), Builder(*this) {}

void ScheduleEmitterImpl::takeOldSlots() {
  OldSlots.splice(OldSlots.begin(), VM.getSLotList(),
                  llvm::next(VM.slot_begin()), VM.slot_end());

  // Remove the successors of the start slot, we will reconstruct them.
  VASTSlot *StartSlot = VM.getStartSlot();
  StartSlot->unlinkSuccs();
  VASTValue *StartSeqVal = StartSlot->getValue();

  typedef VASTSlot::op_iterator op_iterator;
  for (op_iterator I = StartSlot->op_begin(); I != StartSlot->op_end(); /*++I*/) {
    if (VASTSeqSlotCtrl *SeqOp = dyn_cast<VASTSeqSlotCtrl>(*I)) {
      VASTValue *CtrlSignal = SeqOp->getCtrlSignal();
      VASTSeqSlotCtrl::Type T = SeqOp->getCtrlType();
      // Erase the SlotBr to other slots.
      if (T == VASTSeqSlotCtrl::SlotBr && CtrlSignal != StartSeqVal) {
        I = StartSlot->removeOp(I);
        VM.eraseSeqOp(SeqOp);
        continue;
      }
    }

    ++I;
  }

  //Create the new Finish Slot.
  VM.getSLotList().push_back(new VASTSlot(-1, StartSlot));
}

VASTSeqInst *ScheduleEmitterImpl::cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot,
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

VASTValPtr ScheduleEmitterImpl::getValAtSlot(VASTValue *V, VASTSlot *ToSlot) {
  // TODO: Check the predicate of the assignment.
  VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(V);

  if (SeqVal == 0) return V;

  // Try to forward the value which is assigned to SeqVal at the same slot.
  VASTValPtr ForwardedValue = SeqVal;

  typedef VASTSeqValue::itertor iterator;
  for (iterator I = SeqVal->begin(), E = SeqVal->end(); I != E; ++I) {
    VASTSeqUse U = *I;

    if (U.getSlot() == ToSlot) {
      assert(ForwardedValue == SeqVal && "Cannot resolve the source value!");
      ForwardedValue = U;
    }
  }

  return ForwardedValue;
}

VASTValPtr ScheduleEmitterImpl::retimeDatapath(VASTValue *Root, VASTSlot *ToSlot) {
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

ScheduleEmitter::ScheduleEmitter(VASTModule &VM)
  : Impl(new ScheduleEmitterImpl(VM))  {}

ScheduleEmitter::~ScheduleEmitter() {
  delete Impl;
}

VASTSeqOp *ScheduleEmitter::emitToSlot(VASTSeqOp *Op, VASTValPtr Pred,
                                       VASTSlot *ToSlot) {
  // Create the new SeqOp.
  switch (Op->getASTType()) {
  case VASTNode::vastSeqInst:
    return Impl->cloneSeqInst(cast<VASTSeqInst>(Op), ToSlot, Pred);
  default: llvm_unreachable("Unexpected SeqOp type!");
  }
}

void ScheduleEmitter::takeOldSlots() {
  Impl->takeOldSlots();
}

void ScheduleEmitter::addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd,
                                  BasicBlock *DstBB) {
  Impl->addSuccSlot(S, NextSlot, Cnd, DstBB);
}
