//===-- ControlLogicSynthesis.h - Implement the Control Logic ---*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the control logic synthesis pass.
// The control logic synthesis pass implement (or, synthesize) the
// state-transition graph (STG). Specifically, it create a 1 bit register to
// represent each state in the STG and synthesize the transition conditions.
// The transition conditions are further decompose to two part:
//   1) Waiting condition.
//   The condition that the current state is finish (for example we may have a
//   state wait the divider finish) and it is ready to transit to the next state.
//   2) Branching condition.
//   The condition that select the correct next state.
//
// The waiting conditions are represented by the waiting VASTSlotCtrls while
// the Branching conditions are represented by the branching VASTSlotCtrls.
// During the control logic synthesis process, the waiting condition of a state
// is the OR of all waiting signal of VASTSlotCtrls in that state.
// At the same time, the branching condition to a given next state is the
// guarding condition of the branching VASTSlotCtrl that targeting the specified
// next state.
//===----------------------------------------------------------------------===//

#include "MinimalDatapathContext.h"

#include "vast/VASTModulePass.h"
#include "vast/VASTModule.h"
#include "vast/Passes.h"

#include "llvm/IR/DataLayout.h"
#define DEBUG_TYPE "shang-control-logic-synthesis"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct ControlLogicBuilder {
  DatapathBuilder &Builder;
  VASTCtrlRgn &R;

  ControlLogicBuilder(DatapathBuilder &Builder, VASTCtrlRgn &R)
    : Builder(Builder), R(R) {}

  void addSlotReady(VASTSlot *S, VASTValPtr V, VASTValPtr Cnd) {
    Builder.orEqual(SlotReadys[S->getValue()][V], Cnd);
  }

  void addSlotSucc(VASTSlot *S, VASTSlotCtrl *Br) {
    SlotSuccs[S->getValue()].push_back(Br);
  }

  typedef SmallVector<VASTSlotCtrl*, 4> SuccVecTy;
  typedef SuccVecTy::const_iterator const_succ_it;
  std::map<const VASTSeqValue*, SuccVecTy> SlotSuccs;

  typedef std::map<VASTValPtr, VASTValPtr> FUReadyVecTy;
  typedef FUReadyVecTy::const_iterator const_fu_rdy_it;
  std::map<const VASTSeqValue*, FUReadyVecTy> SlotReadys;

  // Signals need to set before this slot is ready.
  const FUReadyVecTy *getReadySet(const VASTSlot *S) const {
    std::map<const VASTSeqValue*, FUReadyVecTy>::const_iterator at
      = SlotReadys.find(S->getValue());

    if (at == SlotReadys.end()) return 0;

    return &at->second;
  }

  // State-transition graph building functions.
  VASTValPtr buildSlotReadyExpr(VASTSlot *S);
  void buildSlotReadyLogic(VASTSlot *S);
  void buildSlotLogic(VASTSlot *S);

  void collectControlLogicInfo(VASTSlot *S);

  void buildControlLogic(VASTModule &M);
};

struct ControlLogicSynthesis : public VASTModulePass {
  static char ID;

  ControlLogicSynthesis() : VASTModulePass(ID) {
    initializeControlLogicSynthesisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnVASTModule(VASTModule &VM);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addPreservedID(PreSchedBindingID);
  }
};
}

INITIALIZE_PASS(ControlLogicSynthesis, "control-logic-synthesis",
                "Implement the Control Logic", false, true)
char ControlLogicSynthesis::ID = 0;

char &llvm::ControlLogicSynthesisID = ControlLogicSynthesis::ID;

VASTValPtr ControlLogicBuilder::buildSlotReadyExpr(VASTSlot *S) {
  SmallVector<VASTValPtr, 4> Ops;

  const FUReadyVecTy *ReadySet = getReadySet(S);
  if (ReadySet)
    for (const_fu_rdy_it I = ReadySet->begin(), E = ReadySet->end();I != E; ++I) {
      // If the condition is true then the signal must be 1 to ready.
      VASTValPtr ReadyCnd = Builder.buildNotExpr(I->second);
      Ops.push_back(Builder.buildOrExpr(I->first, ReadyCnd, 1));
    }

  // No waiting signal means always ready.
  if (Ops.empty()) return VASTConstant::True;

  return Builder.buildAndExpr(Ops, 1);
}

void ControlLogicBuilder::buildSlotReadyLogic(VASTSlot *S) {
  SmallVector<VASTValPtr, 4> Ops;
  // FU ready for current slot.
  Ops.push_back(buildSlotReadyExpr(S));

  // All signals should be 1 before the slot is ready.
  VASTValPtr ReadyExpr = Builder.buildAndExpr(Ops, 1);
  VASTValPtr ActiveExpr = Builder.buildAndExpr(S->getValue(), ReadyExpr, 1);

  // The slot is activated when the slot is enable and all waiting signal is
  // ready.
  assert(!S->IsSubGrp && "Unexpected subgroup!");
  typedef VASTSlot::subgrp_iterator subgrp_iterator;
  for (subgrp_iterator SI = S->subgrp_begin(), SE = S->subgrp_end();
        SI != SE; ++SI) {
    VASTSlot *Child = *SI;
    Child->getActive().set(ActiveExpr);
  }
}

void ControlLogicBuilder::buildSlotLogic(VASTSlot *S) {
  VASTValPtr AlwaysTrue = VASTConstant::True;

  std::map<const VASTSeqValue*, SuccVecTy>::const_iterator at
    = SlotSuccs.find(S->getValue());

  // TODO: Assert there is implicit flow if the successors set cannot be found.
  if (at != SlotSuccs.end()) {
    const SuccVecTy &NextSlots = at->second;
    for (const_succ_it I = NextSlots.begin(),E = NextSlots.end(); I != E; ++I) {
      VASTSlotCtrl *Br = (*I);
      VASTSeqValue *NextSlotReg = Br->getTargetSlot()->getValue();

      VASTUse &U = Br->getSrc(0);
      assert(isa<VASTSlotCtrl>(U.getUser()) && "Unexpected user!");
      U.unlinkUseFromUser();

      // Build the assignment and update the successor branching condition.
      Br->addSrc(AlwaysTrue, 0, NextSlotReg);
    }
  }
}

void ControlLogicBuilder::collectControlLogicInfo(VASTSlot *S) {
  typedef VASTSlot::op_iterator op_iterator;

  // We need to read the S->op_end() at every iteration because it may be
  // changed by removeOp.
  for (op_iterator I = S->op_begin(); I != S->op_end(); ++I) {
    if (VASTSlotCtrl *SeqOp = dyn_cast<VASTSlotCtrl>(*I)) {
      VASTValPtr Pred = SeqOp->getGuard();

      if (SeqOp->isBranch())
        addSlotSucc(S, SeqOp);
      else
        addSlotReady(S, SeqOp->getWaitingSignal(), Pred);
    }
  }
}

void ControlLogicBuilder::buildControlLogic(VASTModule &M) {
  // Building the Slot active signals.
  typedef VASTModule::slot_iterator slot_iterator;

  // Build the signals corresponding to the slots.
  for (slot_iterator I = R.slot_begin(), E = R.slot_end();I != E; ++I) {
    VASTSlot *S = I;

    if (S->IsSubGrp)
      continue;

    S->createSignals(&M);

    // Share the signal to the virtual slots, because the virtual slot reachable
    // from this slot without visiting any non-virtual slots are sharing the
    // same state in the STG with the current slot.
    typedef VASTSlot::subgrp_iterator subgrp_iterator;
    for (subgrp_iterator SI = S->subgrp_begin(), SE = S->subgrp_end();
         SI != SE; ++SI) {
      VASTSlot *Child = *SI;
      if (Child != S)
        Child->copySignals(S);
    }
  }

  for (slot_iterator I = R.slot_begin(), E = R.slot_end(); I != E; ++I)
    collectControlLogicInfo(I);

  for (slot_iterator I = R.slot_begin(), E = R.slot_end(); I != E; ++I) {
    VASTSlot *S = I;

    // No need to synthesize the control logic for virtual slots.
    if (S->IsSubGrp)
      continue;
    
    // Build the ready logic.
    buildSlotReadyLogic(S);
    // Build the state-transfer logic and the functional unit controlling logic.
    buildSlotLogic(S);
  }
}

bool ControlLogicSynthesis::runOnVASTModule(VASTModule &M) {
  // Do not fail if ControlLogicSynthesis had already run on the module.
  if (M.getStartSlot()->getRegister())
    return false;

  MinimalDatapathContext Context(M, getAnalysisIfAvailable<DataLayout>());
  DatapathBuilder Builder(Context);
  VASTCtrlRgn &R = M;
  ControlLogicBuilder(Builder, R).buildControlLogic(M);

  return true;
}
