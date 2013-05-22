//===-- RegisterFolding.cpp - Fold register assignments in the same state -===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
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

#include "shang/Passes.h"
#include "shang/VASTExprBuilder.h"
#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"

#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-register-folding"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumRetimed, "Number of retimed leaf node");
STATISTIC(NumRejectedRetiming,
          "Number of reject retiming because the predicates are not compatible");

namespace {
struct RegisterFolding : public VASTModulePass {
  static char ID;
  VASTExprBuilder *Builder;

  RegisterFolding() : VASTModulePass(ID) {
    initializeRegisterFoldingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    // Perform the control-logic synthesis before this pass, we need the value
    // of the slot ready from the overlapped slot.
    // AU.addRequiredID(ControlLogicSynthesisID);
    // AU.addPreservedID(ControlLogicSynthesisID);
    // Also preserve the STG analysis.
  }

  bool runOnVASTModule(VASTModule &VM);
  bool retimeFannin(VASTUse &U, VASTSlot *S);

  VASTValPtr retimeLeaf(VASTValue *V, VASTSlot *S);
  VASTValPtr retimeExpr(VASTValue *V, VASTSlot *S);
  VASTValPtr retimeExprPtr(VASTValPtr V, VASTSlot *S) {
    VASTValue *Val = V.get();
    VASTValPtr RetimedV = retimeExpr(Val, S);
    if (V.isInverted()) RetimedV = Builder->buildNotExpr(RetimedV);
    return RetimedV;
  }
};
}

char RegisterFolding::ID = 0;

INITIALIZE_PASS_BEGIN(RegisterFolding, "shang-register-folding",
                      "Register Assignments Folding", false, true)
INITIALIZE_PASS_END(RegisterFolding, "shang-register-folding",
                    "Register Assignments Folding", false, true)

Pass *llvm::createRegisterFoldingPass() {
  return new RegisterFolding();
}

static VASTSlot *getParentGroup(VASTSlot *S) {
  // Get the parent group of the current subgroup, to prevent us from
  // unnecessary retiming.
  assert(S->IsSubGrp && "Unexpected parent slot of conditional operation!");
  assert(S->pred_size() == 1 && "Unexpected parent state size for"
                                " subgroup ofPN!");
  return *S->pred_begin();
}

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
  return getParentGroup(S);
}

static bool replaceIfDifferent(VASTUse &U, VASTValPtr V) {
  if (U == V) return false;

  U.replaceUseBy(V);
  return true;
}

bool RegisterFolding::runOnVASTModule(VASTModule &VM) {
  bool AnyFolding = false;
  MinimalExprBuilderContext Cntxt(VM);
  Builder = new VASTExprBuilder(Cntxt);

  typedef VASTModule::slot_iterator slot_iterator;
  // Retime the guarding conditions.
  for (slot_iterator I = VM.slot_begin(), E = VM.slot_end(); I != E; ++I) {
    VASTSlot *S = I;

    if (S->IsSubGrp) continue;

    assert(S->getGuard() == VASTImmediate::True && "Unexpected guarded state!");

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
      VASTSlot *ParentGrp = getParentGroup(Child);

      // Retime the guarding condition of the current subgroup by using the
      // values produced by the parent subgroups.
      VASTValPtr NewGuard = retimeExprPtr(Guard, ParentGrp);
      // AND the conditions together to create the guarding condition to guard
      // the operation in the whole state.
      NewGuard = Builder->buildAndExpr(NewGuard, ParentGrp->getGuard(), 1);
      AnyFolding |= replaceIfDifferent(Guard, NewGuard);
    }
  }

  // Retime the Operands.
  typedef VASTModule::seqop_iterator iterator;
  for (iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I) {
    VASTSeqOp *Op = I;
    VASTUse &Guard = Op->getGuard();
    VASTValPtr NewGuard = Op->getSlot()->getGuard();
    AnyFolding |= replaceIfDifferent(Guard, NewGuard);

    VASTSlot *DstSlot = getRetimingSlot(Op);

    for (unsigned i = 0, e = Op->num_srcs(); i != e; ++i) {
      VASTLatch L = Op->getSrc(i);
      if (VASTSelector *Sel = L.getSelector()) {
        if (Sel->isTrivialFannin(L)) continue;

        // Retime the fannins of the selectors.
        AnyFolding |= retimeFannin(L, DstSlot);
      }
    }
  }

  delete Builder;

  return AnyFolding;
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
  if (RootExpr == 0) return retimeLeaf(Root, S);

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
          RetimedOperand = Builder->buildNotExpr(RetimedOperand);
        AnyOperandRetimed |= RetimedOperand != Operand;

        RetimedOperands.push_back(RetimedOperand);
      }

      // Rebuild the expression if any of its operand retimed.
      VASTValPtr RetimedExpr = Node;
      if (AnyOperandRetimed)
        RetimedExpr = Builder->buildExpr(Node->getOpcode(), RetimedOperands,
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
  VASTValPtr ForwardedValue = SeqVal;

  typedef VASTSeqValue::fanin_iterator iterator;
  for (iterator I = SeqVal->fanin_begin(), E = SeqVal->fanin_end(); I != E; ++I) {
    const VASTLatch &U = *I;

    // Avoid infinite retiming ...
    // if (U.Op == Op) continue;

    // Only retime across the latch operation.
    if (cast<VASTSeqInst>(U.Op)->getSeqOpType() != VASTSeqInst::Latch)
      continue;

    // Wrong slot to retime?
    if (!isReachable(U.getSlot(), S)) {
      ++NumRejectedRetiming;
      continue;
    }

    DEBUG(dbgs() << "Goning to forward " /*<< VASTValPtr(U) << ", "*/
                 << *U.Op->getValue());

    assert (ForwardedValue == SeqVal && "Unexpected multiple compatible source!");
    ForwardedValue = VASTValPtr(U);

    assert(ForwardedValue->getBitWidth() == V->getBitWidth()
           && "Bitwidth implicitly changed!");
    ++NumRetimed;
  }

  return ForwardedValue;
}
