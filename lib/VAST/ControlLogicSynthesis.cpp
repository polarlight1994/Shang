//===-- ControlLogicSynthesis.h - Implement the Control Logic ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the control logic synthesis pass.
//
//===----------------------------------------------------------------------===//

#include "MinimalDatapathContext.h"

#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"
#include "shang/Passes.h"

#include "llvm/IR/DataLayout.h"
#define DEBUG_TYPE "shang-control-logic-synthesis"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct ControlLogicSynthesis : public VASTModulePass {
  static char ID;

  DatapathBuilder *Builder;
  VASTModule *VM;

  void addSlotReady(VASTSlot *S, VASTValue *V, VASTValPtr Cnd) {
    Builder->orEqual(SlotReadys[S->getValue()][V], Cnd);
  }

  void addSlotSucc(VASTSlot *S, VASTRegister *P, VASTValPtr Cnd) {
    bool inserted
      = SlotSuccs[S->getValue()][P].insert(std::make_pair(S, Cnd)).second;
    assert(inserted && "Predicated value had already existed!");
  }

  typedef std::map<VASTSlot*, VASTValPtr> ConditionVector;
  typedef std::map<VASTRegister*, ConditionVector> FUCtrlVecTy;
  typedef FUCtrlVecTy::const_iterator const_fu_ctrl_it;
  std::map<const VASTRegister*, FUCtrlVecTy> SlotSuccs;

  typedef std::map<VASTValue*, VASTValPtr> FUReadyVecTy;
  typedef FUReadyVecTy::const_iterator const_fu_rdy_it;
  std::map<const VASTRegister*, FUReadyVecTy> SlotReadys;

  const FUCtrlVecTy &getSlotSucc(const VASTSlot *S) {
    std::map<const VASTRegister*, FUCtrlVecTy>::const_iterator at
      = SlotSuccs.find(S->getValue());
    assert(at != SlotSuccs.end() && "Slot do not have successor!");
    return at->second;
  }

  // Signals need to set before this slot is ready.
  const FUReadyVecTy *getReadySet(const VASTSlot *S) const {
    std::map<const VASTRegister*, FUReadyVecTy>::const_iterator at
      = SlotReadys.find(S->getValue());

    if (at == SlotReadys.end()) return 0;

    return &at->second;
  }

  // State-transition graph building functions.
  VASTValPtr buildSlotReadyExpr(VASTSlot *S);
  void buildSlotReadyLogic(VASTSlot *S);
  void buildSlotLogic(VASTSlot *S);

  void collectControlLogicInfo(VASTSlot *S);

  ControlLogicSynthesis() : VASTModulePass(ID) {
    initializeControlLogicSynthesisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnVASTModule(VASTModule &VM);

  void releaseMemory() {
    SlotReadys.clear();
    SlotSuccs.clear();
  }
};
}

INITIALIZE_PASS(ControlLogicSynthesis, "control-logic-synthesis",
                "Implement the Control Logic", false, true)
char ControlLogicSynthesis::ID = 0;

char &llvm::ControlLogicSynthesisID = ControlLogicSynthesis::ID;

VASTValPtr ControlLogicSynthesis::buildSlotReadyExpr(VASTSlot *S) {
  SmallVector<VASTValPtr, 4> Ops;

  const FUReadyVecTy *ReadySet = getReadySet(S);
  if (ReadySet)
    for (const_fu_rdy_it I = ReadySet->begin(), E = ReadySet->end();I != E; ++I) {
      // If the condition is true then the signal must be 1 to ready.
      VASTValPtr ReadyCnd = Builder->buildNotExpr(I->second.getAsInlineOperand());
      Ops.push_back(Builder->buildOrExpr(I->first, ReadyCnd, 1));
    }

  // No waiting signal means always ready.
  if (Ops.empty()) return VASTImmediate::True;

  return Builder->buildAndExpr(Ops, 1);
}

void ControlLogicSynthesis::buildSlotReadyLogic(VASTSlot *S) {
  SmallVector<VASTValPtr, 4> Ops;
  // FU ready for current slot.
  Ops.push_back(buildSlotReadyExpr(S));

  // All signals should be 1 before the slot is ready.
  VASTValPtr ReadyExpr = Builder->buildAndExpr(Ops, 1);
  VM->assign(cast<VASTWire>(S->getReady()), ReadyExpr);
  // The slot is activated when the slot is enable and all waiting signal is
  // ready.
  VM->assign(cast<VASTWire>(S->getActive()),
             Builder->buildAndExpr(S->getValue(), ReadyExpr, 1));
}

void ControlLogicSynthesis::buildSlotLogic(VASTSlot *S) {
  typedef const_fu_ctrl_it succ_cnd_iterator;
  typedef ConditionVector::const_iterator cnd_iterator;

  SmallVector<VASTValPtr, 2> LoopCndVector;
  VASTValPtr AlwaysTrue = VASTImmediate::True;

  assert(!S->succ_empty() && "Expect at least 1 next slot!");
  const FUCtrlVecTy &NextSlots = getSlotSucc(S);
  for (succ_cnd_iterator I = NextSlots.begin(),E = NextSlots.end(); I != E; ++I) {
    VASTRegister *NextSlotReg = I->first;
    bool IsLoop = NextSlotReg == S->getValue();
    const ConditionVector &Cnds = I->second;
    for (cnd_iterator CI = Cnds.begin(), CE = Cnds.end(); CI != CE; ++CI) {
      VASTValPtr Cnd = CI->second;
      // Disable the current slot when we are not looping back.
      if (IsLoop) LoopCndVector.push_back(Builder->buildNotExpr(Cnd));

      // Build the assignment and update the successor branching condition.
      VM->assignCtrlLogic(NextSlotReg, AlwaysTrue, CI->first, Cnd, true);
    }
  }

  // Disable the current slot. Do not export the definition of the assignment.
  VM->assignCtrlLogic(S->getValue(), VASTImmediate::False, S,
                      Builder->buildAndExpr(LoopCndVector, 1),
                      true, false);
}

void ControlLogicSynthesis::collectControlLogicInfo(VASTSlot *S) {
  typedef VASTSlot::op_iterator op_iterator;

  // We need to read the S->op_end() at every iteration because it may be
  // changed by removeOp.
  for (op_iterator I = S->op_begin(); I != S->op_end(); /*++I*/) {
    if (VASTSlotCtrl *SeqOp = dyn_cast<VASTSlotCtrl>(*I)) {
      VASTValPtr Pred = SeqOp->getPred();

      if (SeqOp->isBranch())
        addSlotSucc(S, SeqOp->getTargetSlot()->getValue(), Pred);
      else
        addSlotReady(S, SeqOp->getWaitingSignal(), Pred);

      // TODO: Do not remove these operation, so that we can build the control
      // logic again after we reschedule the design.
      // This SeqOp is not used any more.
      I = S->removeOp(I);
      VM->eraseSeqOp(SeqOp);
      continue;
    }

    ++I;
  }
}

bool ControlLogicSynthesis::runOnVASTModule(VASTModule &M) {
  VM = &M;
  MinimalDatapathContext Context(M, getAnalysisIfAvailable<DataLayout>());
  Builder = new DatapathBuilder(Context);

  // Building the Slot active signals.
  typedef VASTModule::slot_iterator slot_iterator;

  // Build the signals corresponding to the slots.
  for (slot_iterator I = VM->slot_begin(), E = llvm::prior(VM->slot_end());
       I != E; ++I) {
    VASTSlot *S = I;

    if (S->IsSubGrp) continue;

    S->createSignals(VM);

    // Share the signal to the virtual slots, because the virtual slot reachable
    // from this slot without visiting any non-virtual slots are sharing the
    // same state in the STG with the current slot.
    typedef VASTSlot::subgrp_iterator subgrp_iterator;
    for (subgrp_iterator SI = S->subgrp_begin(), SE = S->subgrp_end();
         SI != SE; ++SI) {
      VASTSlot *Child = *SI;
      if (Child != S) Child->copySignals(S);
    }
  }

  VM->getFinishSlot()->copySignals(VM->getStartSlot());

  for (slot_iterator I = VM->slot_begin(), E = llvm::prior(VM->slot_end());
       I != E; ++I)
    collectControlLogicInfo(I);

  for (slot_iterator I = VM->slot_begin(), E = llvm::prior(VM->slot_end());
       I != E; ++I) {
    VASTSlot *S = I;

    // No need to synthesize the control logic for virtual slots.
    if (S->IsSubGrp) continue;
    
    // Build the ready logic.
    buildSlotReadyLogic(S);
    // Build the state-transfer logic and the functional unit controlling logic.
    buildSlotLogic(S);
  }

  delete Builder;
  releaseMemory();

  return true;
}
