//===--------- VASTSlot.cpp - The Time Slot in Control Path  ----*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the functions related to the control path of the design.
//
//===----------------------------------------------------------------------===//
#include "LangSteam.h"

#include "vast/VASTSlot.h"
#include "vast/VASTSeqValue.h"
#include "vast/VASTSubModules.h"
#include "vast/VASTModule.h"

#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "vast-slot"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define VASTSLOT_CTOR_NULL_RGN_REF *(reinterpret_cast<VASTCtrlRgn*>(NULL))

VASTSlot::VASTSlot() : VASTNode(vastSlot), R(VASTSLOT_CTOR_NULL_RGN_REF),
  SlotReg(this), SlotActive(this), SlotGuard(this),
  SlotNum(0), IsSubGrp(true), Schedule(0) {}

VASTSlot::VASTSlot(unsigned SlotNum, VASTCtrlRgn &R, BasicBlock *ParentBB,
                   VASTValPtr Pred, bool IsSubGrp, unsigned Schedule)
  : VASTNode(vastSlot), R(R), SlotReg(this, 0), SlotActive(this, 0),
    SlotGuard(this, Pred), SlotNum(SlotNum), IsSubGrp(IsSubGrp),
    Schedule(Schedule) {
  Contents64.ParentBB = ParentBB;
}

VASTSlot::VASTSlot(unsigned slotNum, VASTCtrlRgn &R)
  : VASTNode(vastSlot), R(R), SlotReg(this, 0), SlotActive(this, 0),
    SlotGuard(this, VASTConstant::True), SlotNum(slotNum), IsSubGrp(false),
    Schedule(0) {
  Contents64.ParentBB = 0;
}

VASTSlot::~VASTSlot() {
  while (!Operations.empty()) {
    Operations.back()->clearParent();
    Operations.back()->eraseFromParentList(R.getOpList());
    Operations.pop_back();
  }

  // Release the uses.
  if (!SlotReg.isInvalid()) SlotReg.unlinkUseFromUser();
  if (!SlotActive.isInvalid()) SlotActive.unlinkUseFromUser();
  if (!SlotGuard.isInvalid()) SlotGuard.unlinkUseFromUser();
}

void VASTSlot::createSignals() {
  assert(!IsSubGrp && "Cannot create signal for virtual slots!");
  VASTModule *VM = getParentRgn().getParentModule();

  // Create the relative signals.
  SmallString<36> S;
  S += "Slot";
  S += utostr_32(SlotNum);
  S += "r";
  uint64_t InitVal = SlotNum == 0 ? 1 : 0;
  VASTRegister *R =
    VM->createRegister(S.str(), 1, InitVal, VASTSelector::Slot);
  SlotReg.set(VM->createSeqValue(R->getSelector(), SlotNum, getParent()));
}

void VASTSlot::copySignals(VASTSlot *S) {
  // All control equivalent groups share the same enable signal.
  SlotReg.set(S->SlotReg);
}

BasicBlock *VASTSlot::getParent() const {
  return Contents64.ParentBB;
}

bool VASTSlot::hasNextSlot(VASTSlot *NextSlot) const {
  for (const_succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I)
    if (NextSlot == EdgePtr(*I))
      return true;

  return false;
}

void VASTSlot::unlinkSucc(VASTSlot *S) {
  for (succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
    if (S != EdgePtr(*I))
      continue;

    pred_iterator at = std::find(S->PredSlots.begin(), S->PredSlots.end(), this);
    S->PredSlots.erase(at);
    NextSlots.erase(I);
    return;
  }

  llvm_unreachable("Not the sucessor of this slot!");
}

void VASTSlot::unlinkSuccs() {
  for (succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
    VASTSlot *SuccSlot = *I;
    assert(SuccSlot != this && "Unexpected loop!");

    // Find the this slot in the PredSlot of the successor and erase it.
    pred_iterator at
      = std::find(SuccSlot->PredSlots.begin(), SuccSlot->PredSlots.end(), this);
    SuccSlot->PredSlots.erase(at);
  }

  NextSlots.clear();
}

void VASTSlot::eraseFromParent() {
  R.getSLotList().erase(this);
}

VASTSlot::op_iterator VASTSlot::removeOp(op_iterator where) {
  // Clear the SeqOp's parent slot.
  VASTSeqOp *Op = *where;
  Op->clearParent();
  // Erase it from the SeqOp vector.
  return Operations.erase(where);
}

void VASTSlot::removeOp(VASTSeqOp *Op) {
  op_iterator at = std::find(op_begin(), op_end(), Op);
  assert(at != op_end() && "Op is not in this slot?");
  removeOp(at);
}

VASTRegister *VASTSlot::getRegister() const {
  if (VASTSeqValue *V = getValue())
    return cast<VASTRegister>(V->getParent());

  return 0;
}

VASTSeqValue *VASTSlot::getValue() const {
  return SlotReg.unwrap().getAsLValue<VASTSeqValue>();
}

const char *VASTSlot::getName() const {
  return getValue()->getName();
}

void VASTSlot::addSuccSlot(VASTSlot *NextSlot, EdgeType T) {
  assert(T <= 3 && "Unexpected distance!");
  // Do not add the same successor slot twice.
  if (hasNextSlot(NextSlot)) return;

  assert(NextSlot != this && "Unexpected loop!");

  // Connect the slots.
  NextSlot->PredSlots.push_back(EdgePtr(this, T));
  NextSlots.push_back(EdgePtr(NextSlot, T));
}

VASTSlot *VASTSlot::getSubGroup(BasicBlock *BB) const {
  VASTSlot *SubGrp = 0;
  typedef VASTSlot::const_succ_iterator iterator;
  for (iterator I = succ_begin(), E = succ_end(); I != E; ++I){
    VASTSlot *Succ = *I;

    if (!Succ->IsSubGrp || Succ->getParent() != BB) continue;

    assert(SubGrp == 0 && "Unexpected multiple subgroup with the same BB!");
    SubGrp = Succ;
  }

  return SubGrp;
}

VASTSlot *VASTSlot::getParentState() {
  // The VASTSlot represent the entire state if it is not a sub group.
  if (!IsSubGrp) return this;

  // Else the State is the first State reachable from this SubGrp via the
  // predecessors tree.
  VASTSlot *S = this;
  while (S->pred_size() == 1 && S->IsSubGrp) {
    VASTSlot *PredSlot = S->getParentGroup();
    S = PredSlot;
  }

  return S;
}

VASTSlot *VASTSlot::getParentGroup() const {
  // Get the parent group of the current subgroup, to prevent us from
  // unnecessary retiming.
  assert(IsSubGrp && "Unexpected parent slot of conditional operation!");
  assert(pred_size() == 1 && "Unexpected parent state size for"
                             " subgroup ofPN!");
  return *pred_begin();
}

void VASTSlot::print(raw_ostream &OS) const {
  OS << "Slot#"<< SlotNum;
  if (IsSubGrp) OS << " subgroup";
  OS << " Pred: ";
  for (const_pred_iterator I = pred_begin(), E = pred_end(); I != E; ++I)
    OS << "S#" << (*I)->SlotNum << '(' << (*I)->IsSubGrp << ")v, ";

  if (BasicBlock *BB = getParent())
    OS << "BB: " << BB->getName();

  OS << '\n';

  for (const_op_iterator I = op_begin(), E = op_end(); I != E; ++I)
    (*I)->print(OS.indent(2));

  OS << "Succ: ";

  for (const_succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I)
    OS << "S#" << (*I)->SlotNum << '(' << (*I)->IsSubGrp << ")v, ";
}

void VASTSlot::verify() const {
  for (const_op_iterator I = op_begin(); I != op_end(); ++I) {
    VASTSeqOp *Op = *I;
    if (LLVM_UNLIKELY(Op->getSlot() != this))
      llvm_unreachable("Contains operation in other slot?");
  }
}
