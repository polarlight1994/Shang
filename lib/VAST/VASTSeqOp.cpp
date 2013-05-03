//===------- VASTSeqOp.cpp - Operations in the Control-path  ----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VASTSeqOp and its subclasses.
//
//===----------------------------------------------------------------------===//

#include "shang/VASTSeqOp.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTSlot.h"

#include "llvm/IR/Instruction.h"
#define DEBUG_TYPE "vast-seq-op"
#include "llvm/Support/Debug.h"
using namespace llvm;
//===----------------------------------------------------------------------===//
VASTLatch::operator VASTUse &() const {
  return Op->getUseInteranal(No);
}

VASTLatch::operator VASTValPtr() const {
  return Op->getUseInteranal(No);
}

VASTUse &VASTLatch::operator ->() const {
  return Op->getUseInteranal(No);
}

void VASTLatch::replaceUsedBy(VASTValPtr V) const {
  Op->getUseInteranal(No).replaceUseBy(V);
}

void VASTLatch::replacePredBy(VASTValPtr V, bool UseSlotActive) const {
  Op->replacePredBy(V, UseSlotActive);
}

VASTSlot *VASTLatch::getSlot() const {
  return Op->getSlot();
}

unsigned VASTLatch::getSlotNum() const {
  return getSlot()->SlotNum;
}

VASTUse &VASTLatch::getPred() const {
  return Op->getPred();
}

VASTValPtr VASTLatch::getSlotActive() const {
  return Op->getSlotActive();
}

VASTRegister *VASTLatch::getDst() const {
  return cast<VASTRegister>(&Op->getUseInteranal(No).getUser());
}

//----------------------------------------------------------------------------//
bool VASTSeqOp::operator <(const VASTSeqOp &RHS) const  {
  // Same predicate?
  if (getPred() < RHS.getPred()) return true;
  else if (getPred() > RHS.getPred()) return false;

  // Same slot?
  return getSlot() < RHS.getSlot();
}

VASTLatch VASTSeqOp::getDef(unsigned No) {
  assert(No < getNumDefs() && "Bad define number!");
  return VASTLatch(this, No);
}

void VASTSeqOp::addDefDst(VASTRegister *Def) {
  assert(std::find(Defs.begin(), Defs.end(), Def) == Defs.end()
         && "Define the same seqval twice!");
  Defs.push_back(Def);
}

void VASTSeqOp::addSrc(VASTValPtr Src, unsigned SrcIdx, bool IsDef,
                       VASTRegister *D) {
  assert(SrcIdx < getNumSrcs() && "Bad source index!");
  // The source value of assignment is used by the SeqValue.
  VASTNode *DstUser = D ? (VASTNode*)D : (VASTNode*)this;
  // DIRTYHACK: Temporary allow cycles in register assignment.
  assert(D != Src && "Unexpected cycle!");

  new (src_begin() + SrcIdx) VASTUse(DstUser, Src);
  // Do not add the assignment if the source is invalid.
  if (Src && D) D->addAssignment(this, SrcIdx, IsDef);
}

void VASTSeqOp::print(raw_ostream &OS) const {
  for (unsigned I = 0, E = getNumDefs(); I != E; ++I) {
    OS << Defs[I]->getName() << ", ";
  }

  if (getNumDefs()) OS << "<- ";

  OS << '@' << getSlotNum() << "{ pred";
  if (getSlotActive()) OS << " SlotActive#" << getSlotNum() << ' '; 

  for (unsigned i = 0; i < Size; ++i) {
    VASTValPtr V = getOperand(i);
    V.printAsOperand(OS);

    const VASTNode *User = &getOperand(i).getUser();
    if (const VASTNamedValue *NV = dyn_cast<VASTNamedValue>(User))
      OS << '[' << NV->getName() << ']';

    OS << ", ";
  }
  OS << "} ";

  if (Value *V = getValue()) {
    V->print(OS);
    if (Instruction *Inst = dyn_cast<Instruction>(V))
      OS << " Inst Parent: " << Inst->getParent()->getName();
  }

  if (BasicBlock *BB = getSlot()->getParent())
    OS << " Slot Parent: " << BB->getName();
}

void VASTSeqOp::printPredicate(raw_ostream &OS) const {
  OS << '(';
  if (VASTValPtr SlotActive = getSlotActive()) {
    SlotActive.printAsOperand(OS);
    OS << '&';
  }

  getPred().printAsOperand(OS);

  OS << ')';
}

VASTSeqOp::VASTSeqOp(VASTTypes T, VASTSlot *S, bool UseSlotActive, unsigned Size)
  : VASTOperandList(Size + 1), VASTNode(T), S(S, UseSlotActive) {
  S->addOperation(this);
  Contents.LLVMValue = 0;
}

void VASTSeqOp::replacePredBy(VASTValPtr V, bool UseSlotActive) {
  getPred().replaceUseBy(V);
  S.setInt(UseSlotActive);
}

unsigned VASTSeqOp::getSlotNum() const { return getSlot()->SlotNum; }

VASTValPtr VASTSeqOp::getSlotActive() const {
  if (S.getInt())
    return getSlot()->getActive();

  return VASTValPtr();
}

Value *VASTSeqOp::getValue() const {
  return Contents.LLVMValue;
}

void VASTSeqOp::annotateValue(Value *V) {
  assert((getValue() == 0 || getValue() == V)
         && "Already annotated with some value!");
  Contents.LLVMValue =  V;
}

void VASTSeqOp::dropUses() {
  dropOperands();
}

void VASTSeqOp::removeFromParent() {
  getSlot()->removeOp(this);
}

VASTOperandList *VASTOperandList::GetOperandList(VASTNode *N) {
  if (VASTOperandList *L = GetDatapathOperandList(N))
    return L;

  return dyn_cast_or_null<VASTSeqOp>(N);
}

//----------------------------------------------------------------------------//

VASTSeqInst::VASTSeqInst(Value *V, VASTSlot *S, unsigned Size, VASTSeqInst::Type T)
  : VASTSeqOp(vastSeqInst, S, true, Size), T(T), Data(0) {
  annotateValue(V);
}

void VASTSeqInst::print(raw_ostream &OS) const {
  VASTSeqOp::print(OS);

  switch (getSeqOpType()) {
  case Launch: OS << " <Launch> "; break;
  case Latch:  OS << " <Latch> ";  break;
  }

  OS << '\n';
}

//----------------------------------------------------------------------------//
VASTSeqCtrlOp::VASTSeqCtrlOp(VASTSlot *S, bool UseSlotActive)
  : VASTSeqOp(vastSeqCtrlOp, S, UseSlotActive, 1) {}

void VASTSeqCtrlOp::print(raw_ostream &OS) const {
  VASTSeqOp::print(OS);
  OS << '\n';
}

//----------------------------------------------------------------------------//
VASTSlotCtrl::VASTSlotCtrl(VASTSlot *S, VASTNode *N)
  : VASTSeqOp(vastSlotCtrl, S, false, 0), Ptr() {
  if (VASTSlot *S = dyn_cast<VASTSlot>(N)) Ptr = S;
  else                                     Ptr = cast<VASTValue>(N);
}

bool VASTSlotCtrl::isBranch() const { return Ptr.is<VASTSlot*>(); }

VASTSlot *VASTSlotCtrl::getTargetSlot() const {
  return Ptr.get<VASTSlot*>();
}

VASTValue *VASTSlotCtrl::getWaitingSignal() const {
  return Ptr.get<VASTValue*>();
}

void VASTSlotCtrl::print(raw_ostream &OS) const {
  VASTSeqOp::print(OS);
  if (isBranch()) {
    OS << " Slot Br #" << getTargetSlot()->SlotNum;

    if (BasicBlock *BB = getTargetSlot()->getParent())
      OS << " Target BB: " << BB->getName();
  } else
    OS << " Waiting " << VASTValPtr(getWaitingSignal());

  OS << '\n';
}
