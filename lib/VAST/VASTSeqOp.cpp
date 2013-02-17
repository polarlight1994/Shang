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
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-seq-op"
#include "llvm/Support/Debug.h"
using namespace llvm;
STATISTIC(NumCycles, "Number of Cycles in Register Assignment");
//===----------------------------------------------------------------------===//
VASTSeqUse::operator VASTUse &() const {
  return Op->getUseInteranal(No);
}

VASTSeqUse::operator VASTValPtr() const {
  return Op->getUseInteranal(No);
}

VASTUse &VASTSeqUse::operator ->() const {
  return Op->getUseInteranal(No);
}

VASTSlot *VASTSeqUse::getSlot() const {
  return Op->getSlot();
}

VASTUse &VASTSeqUse::getPred() const {
  return Op->getPred();
}

VASTValPtr VASTSeqUse::getSlotActive() const {
  return Op->getSlotActive();
}

VASTSeqValue *VASTSeqUse::getDst() const {
  return cast<VASTSeqValue>(&Op->getUseInteranal(No).getUser());
}

//===----------------------------------------------------------------------===//
VASTSeqOp *VASTSeqDef::operator ->() const {
  return Op;
}

VASTSeqDef::operator VASTSeqValue *() const {
  return Op->Defs[No];
}

const char *VASTSeqDef::getName() const {
  return Op->Defs[No]->getName();
}

//----------------------------------------------------------------------------//
bool VASTSeqOp::operator <(const VASTSeqOp &RHS) const  {
  // Same predicate?
  if (getPred() < RHS.getPred()) return true;
  else if (getPred() > RHS.getPred()) return false;

  // Same slot?
  return getSlot() < RHS.getSlot();
}

void VASTSeqOp::addDefDst(VASTSeqValue *Def) {
  assert(std::find(Defs.begin(), Defs.end(), Def) == Defs.end()
         && "Define the same seqval twice!");
  Defs.push_back(Def);
}

void VASTSeqOp::addSrc(VASTValPtr Src, unsigned SrcIdx, bool IsDef,
                       VASTSeqValue *D) {
  assert(SrcIdx < getNumSrcs() && "Bad source index!");
  // The source value of assignment is used by the SeqValue.
  VASTNode *DstUser = D ? (VASTNode*)D : (VASTNode*)this;
  // DIRTYHACK: Temporary allow cycles in register assignment.
  if (D == Src) {
    ++NumCycles;
    DstUser = this;
  }

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
  for (unsigned i = 0; i < Size; ++i) {
    VASTValPtr V = getOperand(i);
    V.printAsOperand(OS);

    const VASTNode *User = &getOperand(i).getUser();
    if (const VASTNamedValue *NV = dyn_cast<VASTNamedValue>(User))
      OS << '[' << NV->getName() << ']';

    OS << ", ";
  }
  OS << "} ";

  if (getValue()) getValue()->print(OS);
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
  if (isBranch()) OS << " Slot Br #" << getTargetSlot()->SlotNum;
  else            OS << " Waiting " << VASTValPtr(getWaitingSignal());

  OS << '\n';
}
