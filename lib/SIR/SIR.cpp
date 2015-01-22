//===----------------- SIR.cpp - Modules in SIR ----------------*- C++ -*-===//
//
//                      The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the SIR.
//
//===----------------------------------------------------------------------===//
#include "sir/SIR.h"
#include "llvm/IR/Value.h"

using namespace llvm;

void SIRSelector::addAssignment(Value *Fanin, Value *FaninGuard) {
  Fanins.push_back(Fanin);
  FaninGuards.push_back(FaninGuard);
  assert(Fanins.size() == FaninGuards.size() && "Size not compatible!");
}

void SIRSelector::printDecl(raw_ostream &OS) const {
  // Need to Implement these functions.
  OS << "reg" << BitRange(getBitWidth(), 0, false);
  OS << " " << Mangle(getName());
  // Set the IniteVal into 0;
  OS << " = " << buildLiteral(0, getBitWidth(), false)
     << ";\n";
}

void SIRPort::printDecl(raw_ostream &OS) const {
  if (isInput())
    OS << "input ";
  else
    OS << "output ";

  // If it is a output, then it should be a register.
  if (!isInput())
    OS << "reg";
  else
    OS << "wire";

  if (getBitWidth() > 1) OS << "[" << utostr_32(getBitWidth() - 1) << ":0]";

  OS << " " << Mangle(getName());
}

void SIRSeqOp::print(raw_ostream &OS) const {
	OS << "Assign the Src Value " << Src->getName()
		 << " to register " << DstReg->getName()
		 << " in Slot #" << S->getSlotNum()
		 << " Scheduled to " << S->getSchedule() << "\n";
}

void SIRSeqOp::dump() const {
	print(dbgs());
	dbgs() << "\n";
}

bool SIRSlot::hasNextSlot(SIRSlot *NextSlot) {
  for (const_succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I)
    if (NextSlot == EdgePtr(*I))
      return true;

  return false;
}

void SIRSlot::addSuccSlot(SIRSlot *NextSlot, EdgeType T) {
  assert(T <= 3 && "Unexpected distance!");
  // Do not add the same successor slot twice.
  if (hasNextSlot(NextSlot)) return;

  // Hack: I think we can have this loop 
  // when we don't have subgrp to avoid it.
  //assert(NextSlot != this && "Unexpected loop!");

  // Connect the slots.
  NextSlot->PredSlots.push_back(EdgePtr(this, T));
  NextSlots.push_back(EdgePtr(NextSlot, T));
}

void SIRSlot::print(raw_ostream &OS) const {
	OS << "Slot #" << SlotNum << " Scheduled to " << Schedule
		 << " Guarding by " << getGuardValue()->getName() << "\n";
}

void SIRSlot::dump() const {
	print(dbgs());
	dbgs() << "\n";
}

IntegerType *SIR::createIntegerType(unsigned BitWidth) {
  return IntegerType::get(C, BitWidth);
}

Value *SIR::createIntegerValue(unsigned BitWidth, unsigned Val) {
  IntegerType *T = createIntegerType(BitWidth);
  return ConstantInt::get(T, Val);
}

void SIR::printModuleDecl(raw_ostream &OS) const {
  OS << "module " << F->getValueName()->getKey();
  OS << "(\n";
  Ports.front()->printDecl(OS.indent(4));  
  for (SIRPortVector::const_iterator I = Ports.begin() + 1, E = Ports.end();
       I != E; ++I) {
    // Assign the ports to virtual pins.
    OS << ",\n (* altera_attribute = \"-name VIRTUAL_PIN on\" *)";
    (*I)->printDecl(OS.indent(4));
  }
  OS << ");\n";
}

bool SIR::gcImpl() {
	// Hack: The GC function here should be constructed
	// according to the DeadInstElemPass in LLVM.
	return false;
}

bool SIR::gc() {
	bool changed = false;

	// Iteratively release the dead objects.
	while (gcImpl())
		changed = true;

	return changed;
}