//===------- VASTSeqOp.cpp - Operations in the Control-path  ----*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VASTSeqOp and its subclasses.
//
//===----------------------------------------------------------------------===//

#include "vast/VASTSeqOp.h"
#include "vast/VASTSeqValue.h"
#include "vast/VASTCtrlRgn.h"
#include "vast/LuaI.h"

#include "llvm/IR/Instruction.h"
#define DEBUG_TYPE "vast-seq-op"
#include "llvm/Support/Debug.h"
using namespace llvm;
//===----------------------------------------------------------------------===//
VASTLatch::operator VASTUse &() const {
  return getFanin();
}

VASTLatch::operator VASTValPtr() const {
  return getFanin();
}

VASTUse &VASTLatch::operator ->() const {
  return getFanin();
}

void VASTLatch::replaceUsedBy(VASTValPtr V) const {
  Op->getUseInteranal(No).replaceUseBy(V);
}

void VASTLatch::replaceGuardBy(VASTValPtr V, bool UseSlotActive) const {
  Op->replaceGuardBy(V, UseSlotActive);
}

VASTSlot *VASTLatch::getSlot() const {
  return Op->getSlot();
}

unsigned VASTLatch::getSlotNum() const {
  return getSlot()->SlotNum;
}

VASTUse &VASTLatch::getGuard() const {
  return Op->getGuard();
}

VASTUse &VASTLatch::getFanin() const {
  return Op->getUseInteranal(No);
}

VASTValPtr VASTLatch::getSlotActive() const {
  return Op->getSlotActive();
}

VASTSeqValue *VASTLatch::getDst() const {
  return Op->getDef(No);
}

VASTSelector *VASTLatch::getSelector() const {
  return dyn_cast<VASTSelector>(&Op->getUseInteranal(No).getUser());
}

void VASTLatch::removeFromParent() {
  VASTNode &N = Op->getUseInteranal(No).getUser();

  if (VASTSelector *Sel = dyn_cast<VASTSelector>(&N))
    Sel->eraseFanin(*this);
}

void VASTLatch::eraseOperand() {
  Op->eraseOperand(No);
}

//----------------------------------------------------------------------------//
bool VASTSeqOp::operator <(const VASTSeqOp &RHS) const  {
  // Same predicate?
  if (getGuard() < RHS.getGuard()) return true;
  else if (getGuard() > RHS.getGuard()) return false;

  // Same slot?
  return getSlot() < RHS.getSlot();
}

VASTSeqValue *VASTSeqOp::getDef(unsigned No) {
  return No < getNumDefs() ? Defs[No] : 0;
}

void VASTSeqOp::addSrc(VASTValPtr Src, unsigned SrcIdx, VASTSelector *Sel,
                       VASTSeqValue *Dst) {
  assert(Src && "Bad source value!");
  assert(SrcIdx < num_srcs() && "Bad source index!");

  new (src_begin() + SrcIdx) VASTUse(Sel, Src);

  // Remember the defined SeqVal if it exists.
  if (Dst)  {
    assert(Sel->defines(Dst)
           && "Selector didn't defines Dst, forget to transfer?");
    assert(Dst->getBitWidth() == Sel->getBitWidth()
           && "Bitwidth not matched in assignment!");
    assert(std::find(Defs.begin(), Defs.end(), Dst) == Defs.end()
          && "Define the same seqval twice!");
    assert(SrcIdx == Defs.size()
           && "Define index and source index not synchronized!");
    Defs.push_back(Dst);
  }

  // At the assignment as a fanin to the selector.
  Sel->addAssignment(this, SrcIdx);
}

void VASTSeqOp::addSrc(VASTValPtr Src, unsigned SrcIdx, VASTSeqValue *Dst) {
  addSrc(Src, SrcIdx, Dst->getSelector(), Dst);
}

void VASTSeqOp::addSrc(VASTValPtr Src, unsigned SrcIdx, VASTSelector *Sel) {
  addSrc(Src, SrcIdx, Sel, 0);
}

void VASTSeqOp::print(raw_ostream &OS) const {
  for (unsigned I = 0, E = getNumDefs(); I != E; ++I) {
    OS << Defs[I]->getName() << ", ";
  }

  if (getNumDefs()) OS << "<- ";

  OS << '@' << getSlotNum() << "{ guard";
  if (getSlotActive()) OS << " SlotActive#" << getSlotNum() << ' '; 

  for (unsigned i = 0; i < Size; ++i) {
    VASTValPtr V = getOperand(i);
    V.printAsOperand(OS);

    const VASTNode *User = &getOperand(i).getUser();
    if (const VASTSelector *Sel = dyn_cast<VASTSelector>(User))
      OS << '[' << Sel->getName() << ']';

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

void VASTSeqOp::printGuard(raw_ostream &OS) const {
  OS << '(';
  if (VASTValPtr SlotActive = getSlotActive()) {
    SlotActive.printAsOperand(OS);
    OS << '&';
  }

  getGuard().printAsOperand(OS);

  OS << ')';
}

VASTSeqOp::VASTSeqOp(VASTTypes T, VASTSlot *S, bool UseSlotActive, unsigned Size)
  : VASTOperandList(Size + 1), VASTNode(T), S(S, UseSlotActive) {
  S->addOperation(this);
  Contents64.LLVMValue = 0;
}

void VASTSeqOp::replaceGuardBy(VASTValPtr V, bool UseSlotActive) {
  getGuard().replaceUseBy(V);
  S.setInt(UseSlotActive);
}

unsigned VASTSeqOp::getSlotNum() const { return getSlot()->SlotNum; }

VASTValPtr VASTSeqOp::getSlotActive() const {
  if (guardedBySlotActive())
    return getSlot()->getActive().unwrap();

  return None;
}

Value *VASTSeqOp::getValue() const {
  return Contents64.LLVMValue;
}

void VASTSeqOp::annotateValue(Value *V) {
  assert((getValue() == 0 || getValue() == V)
         && "Already annotated with some value!");
  Contents64.LLVMValue =  V;
}

void VASTSeqOp::eraseOperand(unsigned Idx) {
  // Unlink the use.
  VASTUse &U = getUseInteranal(Idx);
  U.unlinkUseFromUser();

  if (VASTSelector *Sel = dyn_cast<VASTSelector>(&U.getUser()))
    Sel->eraseFanin(getSrc(Idx));

  for (unsigned i = Idx + 1; i < num_srcs(); ++i) {
    VASTLatch L = getSrc(i);
    L.removeFromParent();
    // Create the use at the earlier operand, please note that we are not going
    // to add the defined SeqVal again, otherwise we are pushing the same SeqVal
    // more than once in the Defs vector.
    addSrc(L, i - 1, L.getSelector(), 0);
    getUseInteranal(i).unlinkUseFromUser();
  }

  if (getNumDefs() > Idx)
    Defs.erase(Defs.begin() + Idx);

  --Size;
}

void VASTSeqOp::dropUses() {
  dropOperands();
}

void VASTSeqOp::eraseFromParentList(ilist<VASTSeqOp> &List) {
  assert(getSlot() == NULL &&
         "Should call clear parent before calling this function!");

  assert(size() && "Deleting the same operation twice?");
  for (unsigned i = 0, e = num_srcs(); i != e; ++i)
    getSrc(i).removeFromParent();

  dropUses();

  // Set the size to 0 since we had clear up the operand list.
  Size = 0;

  // Erase the current node from the list that contains the current node.
  List.erase(this);
}

void VASTSeqOp::eraseFromParent() {
  // Get the list that containing this node.
  VASTSlot *S = getSlot();
  assert(S && "Cannot get parent slot!");
  VASTCtrlRgn &R = S->getParentRgn();

  S->removeOp(this);
  eraseFromParentList(R.getOpList());
}

//----------------------------------------------------------------------------//

VASTSeqInst::VASTSeqInst(Value *V, VASTSlot *S, unsigned Size, bool IsLatch)
  : VASTSeqOp(vastSeqInst, S, true, Size) {
  annotateValue(V);
  Contents16.SeqInstData = 0;
  Contents8.SeqInstIsLatch = IsLatch;
}

void VASTSeqInst::print(raw_ostream &OS) const {
  VASTSeqOp::print(OS);

  if (isLatch()) OS << " <Latch> ";
  else           OS << " <Launch> ";

  OS << '\n';
}

VFUs::FUTypes VASTSeqInst::getFUType() const {
  if (!isLaunch())
    return VFUs::Trivial;

  Instruction *Inst = dyn_cast<Instruction>(getValue());
  if (Inst == 0)
    return VFUs::Trivial;

  switch (Inst->getOpcode()) {
  default: break;
  case Instruction::Add:
  case Instruction::Sub:
    if (getNumDefs() == 3)
      return VFUs::AddSub;
    break;
  case Instruction::Mul:
    if (getNumDefs() == 2)
      return VFUs::Mult;
    break;
  case Instruction::Shl:
  case Instruction::AShr:
  case Instruction::LShr:
    if (getNumDefs() == 2)
      return VFUs::Shift;
    break;
  case Instruction::ICmp:
    if (getNumDefs() == 2)
      return VFUs::ICmp;
    break;
  case Instruction::Load:
  case Instruction::Store:
    return VFUs::MemoryBus;
  }

  return VFUs::Trivial;
}

unsigned VASTSeqInst::getFUCost() const {
  unsigned BitWidth = const_cast<VASTSeqInst*>(this)->getSrc(0)->getBitWidth();
  switch (getFUType()) {
  case VFUs::AddSub:
    return LuaI::Get<VFUAddSub>()->lookupCost(std::min(64u, BitWidth));
  case VFUs::ICmp:
    return LuaI::Get<VFUICmp>()->lookupCost(BitWidth);
  case VFUs::Mult:
    return LuaI::Get<VFUMult>()->lookupCost(BitWidth);
  case VFUs::Shift:
    return LuaI::Get<VFUShift>()->lookupCost(std::min(64u, BitWidth));
  default:
    break;
  }

  return 0;
}

bool VASTSeqInst::isBindingCandidate() const {
  return getNumDefs() == num_srcs();
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
  : VASTSeqOp(vastSlotCtrl, S, true, 1), TargetSlot(0) {
  VASTValue *Src = VASTConstant::True;
  if (VASTSlot *S = dyn_cast<VASTSlot>(N)) TargetSlot = S;
  else                                     Src = cast<VASTValue>(N);

  // Initialize the operand.
  new (src_begin() + 0) VASTUse(this, Src);
}

bool VASTSlotCtrl::isBranch() const { return TargetSlot != 0; }

VASTSlot *VASTSlotCtrl::getTargetSlot() const {
  assert(isBranch() && "Wrong accessor!");
  return TargetSlot;
}

VASTValue *VASTSlotCtrl::getWaitingSignal() const {
  assert(!isBranch() && "Wrong accessor!");
  VASTValPtr WaitingSignal = *src_begin();
  assert(!WaitingSignal.isInverted() && "Unexpected inverted waiting signal!");
  return WaitingSignal.get();
}

VASTNode *VASTSlotCtrl::getNode() const {
  if (isBranch()) return getTargetSlot();

  return getWaitingSignal();
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
