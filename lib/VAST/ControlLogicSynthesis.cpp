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

  void addSlotDisable(VASTSlot *S, VASTSeqValue *P, VASTValPtr Cnd) {
    Builder->orEqual(SlotDisables[S][P], Cnd);
  }

  void addSlotReady(VASTSlot *S, VASTValue *V, VASTValPtr Cnd) {
    Builder->orEqual(SlotReadys[S][V], Cnd);
  }

  void addSlotEnable(VASTSlot *S, VASTSeqValue *P, VASTValPtr Cnd) {
    Builder->orEqual(SlotEnables[S][P], Cnd);
  }

  typedef std::map<VASTSeqValue*, VASTValPtr> FUCtrlVecTy;
  typedef FUCtrlVecTy::const_iterator const_fu_ctrl_it;
  std::map<const VASTSlot*, FUCtrlVecTy> SlotEnables, SlotDisables;

  typedef std::map<VASTValue*, VASTValPtr> FUReadyVecTy;
  typedef FUReadyVecTy::const_iterator const_fu_rdy_it;
  std::map<const VASTSlot*, FUReadyVecTy> SlotReadys;

  // Signals need to be enabled at this slot.
  const FUCtrlVecTy *getEnableSet(const VASTSlot *S) const {
    std::map<const VASTSlot*, FUCtrlVecTy>::const_iterator at
      = SlotEnables.find(S);

    if (at == SlotEnables.end()) return 0;

    return &at->second;
  }

  bool isEnabled(const VASTSlot *S, VASTSeqValue *P) const {
    if (const FUCtrlVecTy *EnableSet = getEnableSet(S))
      return EnableSet->count(P);

    return false;
  }

  // Signals need to set before this slot is ready.
  const FUReadyVecTy *getReadySet(const VASTSlot *S) const {
    std::map<const VASTSlot*, FUReadyVecTy>::const_iterator at
      = SlotReadys.find(S);

    if (at == SlotReadys.end()) return 0;

    return &at->second;
  }

  // Signals need to be disabled at this slot.
  const FUCtrlVecTy *getDisableSet(const VASTSlot *S) const {
    std::map<const VASTSlot*, FUCtrlVecTy>::const_iterator at
      = SlotDisables.find(S);

    if (at == SlotDisables.end()) return 0;

    return &at->second;
  }

  bool isDisabled(const VASTSlot *S, VASTSeqValue *P) const {
    if (const FUCtrlVecTy *DisableSet = getDisableSet(S))
      return DisableSet->count(P);

    return false;
  }

  // State-transition graph building functions.
  VASTValPtr buildSlotReadyExpr(VASTSlot *S);
  void buildSlotReadyLogic(VASTSlot *S);
  void buildSlotLogic(VASTSlot *S);
  
  void getherControlLogicInfo(VASTSlot *S);

  ControlLogicSynthesis() : VASTModulePass(ID) {
    initializeControlLogicSynthesisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnVASTModule(VASTModule &VM);
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
  typedef VASTSlot::succ_cnd_iterator succ_cnd_iterator;

  VASTValPtr SelfLoopCnd;
  VASTValPtr AlwaysTrue = VASTImmediate::True;

  assert(!S->succ_empty() && "Expect at least 1 next slot!");
  for (succ_cnd_iterator I = S->succ_cnd_begin(),E = S->succ_cnd_end(); I != E; ++I) {
    VASTSeqValue *NextSlotReg = I->first->getValue();
    if (I->first->SlotNum == S->SlotNum) SelfLoopCnd = I->second;
    // Build the assignment and update the successor branching condition.
    VM->assignCtrlLogic(NextSlotReg, AlwaysTrue, S, I->second, true);
  }

  SmallVector<VASTValPtr, 2> CndVector;
  // Disable the current slot when we are not looping back.
  if (SelfLoopCnd)
    CndVector.push_back(Builder->buildNotExpr(SelfLoopCnd));

  // Disable the current slot. Do not export the definition of the assignment.
  VM->assignCtrlLogic(S->getValue(), VASTImmediate::False, S,
                      Builder->buildAndExpr(CndVector, 1),
                      true/*, false*/);

  if (const FUCtrlVecTy *EnableSet = getEnableSet(S))
    for (const_fu_ctrl_it I = EnableSet->begin(), E = EnableSet->end();
         I != E; ++I) {
      // No need to wait for the slot ready.
      // We may try to enable and disable the same port at the same slot.
      CndVector.clear();
      CndVector.push_back(S->getValue());
      VASTValPtr ReadyCnd
        = Builder->buildAndExpr(S->getReady()->getAsInlineOperand(false),
                                I->second.getAsInlineOperand(), 1);
      // No need to export the definition of the enable assignment, it is never
      // use inside the module.
      VM->assignCtrlLogic(I->first, ReadyCnd, S,
                          Builder->buildAndExpr(CndVector, 1),
                          false, false);
    }

  SmallVector<VASTValPtr, 4> DisableAndCnds;

  if (const FUCtrlVecTy *DisableSet = getDisableSet(S))
    for (const_fu_ctrl_it I = DisableSet->begin(), E = DisableSet->end();
         I != E; ++I) {
      // Look at the current enable set and alias enables set;
      // The port assigned at the current slot, and it will be disabled if
      // The slot is not ready or the enable condition is false. And it is
      // ok that the port is enabled.
      if (isEnabled(S, I->first)) continue;

      DisableAndCnds.push_back(S->getValue());
      DisableAndCnds.push_back(I->second);

      VASTSeqValue *En = I->first;
      // No need to export the definition of the disable assignment, it is never
      // use inside the module.
      VM->assignCtrlLogic(En, VASTImmediate::False, S, 
                          Builder->buildAndExpr(DisableAndCnds, 1),
                          false, true);
      DisableAndCnds.clear();
    }
}

static VASTSlot *GetLinearNextSlot(VASTSlot *S) {
  assert(S->succ_size() == 1 && "More than one slot!");
  VASTSlot *NextSlot = *S->succ_begin();
  assert(S->getSuccCnd(NextSlot) == VASTImmediate::True
         && "Next slot is conditional!");
  return NextSlot;
}

void ControlLogicSynthesis::getherControlLogicInfo(VASTSlot *S) {
  typedef VASTSlot::op_iterator op_iterator;

  for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
    VASTSeqOp *SeqOp = *I;
    VASTValPtr Pred = SeqOp->getPred();
    Instruction *Inst = dyn_cast_or_null<Instruction>(SeqOp->getValue());

    if (Inst == 0) continue;

    switch (Inst->getOpcode()) {
    // Nothing to do by default.
    default: break;
    case Instruction::Load:
    case Instruction::Store: {
      // FIXIME: Use the correct memory port number.
      std::string EnableName = VFUMemBus::getEnableName(0) + "_r";
      VASTSeqValue *MemEn = VM->getSymbol<VASTSeqValue>(EnableName);
      addSlotEnable(S, MemEn, Pred);

      VASTSlot *NextSlot = GetLinearNextSlot(S);
      addSlotDisable(NextSlot, MemEn, Pred);
      break;
    }
    // Enable the finish at the same slot.
    case Instruction::Ret:
      addSlotEnable(S, VM->getPort(VASTModule::Finish).getSeqVal(), Pred);
      break;
    }
  }
}

bool ControlLogicSynthesis::runOnVASTModule(VASTModule &M) {
  VM = &M;
  MinimalDatapathContext Context(M, getAnalysisIfAvailable<DataLayout>());
  Builder = new DatapathBuilder(Context);

  // Disable the finish signal when the module start.
  VASTSlot *StartSlot = VM->getStartSlot();
  addSlotDisable(StartSlot, VM->getPort(VASTModule::Finish).getSeqVal(),
                 VASTImmediate::True);

  // Building the Slot active signals.
  typedef VASTModule::slot_iterator slot_iterator;
  
  for (slot_iterator I = VM->slot_begin(), E = llvm::prior(VM->slot_end());
       I != E; ++I)
    getherControlLogicInfo(I);

  for (slot_iterator I = VM->slot_begin(), E = llvm::prior(VM->slot_end());
       I != E; ++I) {
    VASTSlot *S = I;

    // Build the ready logic.
    buildSlotReadyLogic(S);
    // Build the state-transfer logic and the functional unit controlling logic.
    buildSlotLogic(S);
  }

  delete Builder;

  return true;
}
