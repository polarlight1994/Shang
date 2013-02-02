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

void VASTSeqOp::addSrc(VASTValPtr Src, unsigned SrcIdx, bool IsDef, VASTSeqValue *D) {
  assert(SrcIdx < getNumSrcs() && "Bad source index!");
  // The source value of assignment is used by the SeqValue.
  new (src_begin() + SrcIdx) VASTUse(D ? (VASTNode*)D : (VASTNode*)this, Src);
  // Do not add the assignment if the source is invalid.
  if (Src && D) D->addAssignment(this, SrcIdx, IsDef);
}

void VASTSeqOp::print(raw_ostream &OS) const {
  for (unsigned I = 0, E = getNumDefs(); I != E; ++I) {
    OS << Defs[I]->getName() << ", ";
  }
  
  OS << "<-@" << getSlotNum() << "{ pred";
  for (unsigned i = 0; i < Size; ++i) {
    if (VASTValPtr V = getOperand(i).unwrap()) V.printAsOperand(OS);
    else                                       OS << "<nullptr>";
    OS << ", ";
  }
  OS << "} ";
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

VASTSeqOp::VASTSeqOp(VASTTypes T, VASTSlot *S, bool UseSlotActive,
                     VASTUse *Operands, unsigned Size)
  : VASTOperandList(Operands, Size + 1), VASTNode(T), S(S, UseSlotActive) {
  S->addOperation(this);
}

unsigned VASTSeqOp::getSlotNum() const { return getSlot()->SlotNum; }

VASTValPtr VASTSeqOp::getSlotActive() const {
  if (S.getInt())
    return getSlot()->getActive();

  return VASTValPtr();
}

Value *VASTSeqOp::getValue() const {
  llvm_unreachable("VASTSeqOp::getInst() should had been overrided!");
  return 0;
}

VASTOperandList *VASTOperandList::GetOperandList(VASTNode *N) {
  if (VASTOperandList *L = GetDatapathOperandList(N))
    return L;

  return dyn_cast_or_null<VASTSeqOp>(N);
}


//----------------------------------------------------------------------------//

VASTSeqInst::VASTSeqInst(Value *V, VASTSlot *S, VASTUse *Operands, unsigned Size,
                         VASTSeqInst::Type T)
  : VASTSeqOp(vastSeqInst, S, true, Operands, Size), V(V, T) {}

void VASTSeqInst::print(raw_ostream &OS) const {
  VASTSeqOp::print(OS);

  switch (getSeqOpType()) {
  case Launch: OS << "<Launch> "; break;
  case Latch:  OS << "<Latch> ";  break;
  case Alias:  OS << "<Alias> ";  break;
  }

  if (getValue()) getValue()->print(OS);
  else            OS << '\n';

  OS << '\n';
}

//----------------------------------------------------------------------------//
VASTSeqCtrlOp::VASTSeqCtrlOp(VASTSlot *S, bool UseSlotActive, VASTUse *Operands)
  : VASTSeqOp(vastSeqCtrlOp, S, UseSlotActive, Operands, 1) {}

void VASTSeqCtrlOp::print(raw_ostream &OS) const {
  VASTSeqOp::print(OS);
  OS << '\n';
}
