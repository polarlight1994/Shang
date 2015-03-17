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
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

void SIRRegister::addAssignment(Value *Fanin, Value *FaninGuard) {
  Fanins.push_back(Fanin);
  FaninGuards.push_back(FaninGuard);
  assert(Fanins.size() == FaninGuards.size() && "Size not compatible!");
}

void SIRRegister::printDecl(raw_ostream &OS) const {
  // Need to Implement these functions.
  OS << "reg" << BitRange(getBitWidth(), 0, false);
  OS << " " << Mangle(getName());
  // Set the IniteVal.
  OS << " = " << buildLiteralUnsigned(InitVal, getBitWidth())
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

void SIRSlot::addSuccSlot(SIRSlot *NextSlot, EdgeType T, Value *Cnd) {
  assert(T <= 3 && "Unexpected distance!");
  // Do not add the same successor slot twice.
  if (hasNextSlot(NextSlot)) return;

  // Hack: I think we can have this loop 
  // when we don't have subgrp to avoid it.
  //assert(NextSlot != this && "Unexpected loop!");

  // Connect the slots.
  NextSlot->PredSlots.push_back(EdgePtr(this, T, Cnd));
  NextSlots.push_back(EdgePtr(NextSlot, T, Cnd));
}

void SIRSlot::unlinkSucc(SIRSlot *S) {
	for (succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
		if (S != *I)
			continue;

		// Remove the PredEdge in DstSlot.
		pred_iterator at = std::find(S->PredSlots.begin(), S->PredSlots.end(), this);
		S->PredSlots.erase(at);

		// Remove the SuccEdge in SrcSlot.
		NextSlots.erase(I);
		return;
	}

	llvm_unreachable("Not the sucessor of this slot!");
}

void SIRSlot::unlinkSuccs() {
	for (succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
		SIRSlot *SuccSlot = *I;
		assert(SuccSlot != this && "Unexpected loop!");

		// Find the this slot in the PredSlot of the successor and erase it.
		pred_iterator at
			= std::find(SuccSlot->PredSlots.begin(), SuccSlot->PredSlots.end(), this);
		SuccSlot->PredSlots.erase(at);
	}

	NextSlots.clear();
}

void SIRSlot::replaceAllUsesWith(SIRSlot *S) {
	SmallVector<EdgePtr, 4> Succs;

	// Store all the Succs of CurSlot.
	for (succ_iterator I = this->succ_begin(), E = this->succ_end(); I != E; ++I) 
		Succs.push_back(*I);
	
	// Unlink all Succs of CurSlot.
	this->unlinkSuccs();

	// Link all Succs of CurSlot to Target Slot.
	typedef SmallVector<EdgePtr, 4>::iterator iterator;
	for (iterator I = Succs.begin(), E = Succs.end(); I != E; ++I) 
		S->addSuccSlot(I->getSlot(), I->getType(), I->getCnd());	
}

void SIRSlot::print(raw_ostream &OS) const {
	OS << "Slot #" << SlotNum << " Scheduled to " << Schedule
		 << " Guarding by " << getGuardValue()->getName() << "\n";
}

void SIRSlot::dump() const {
	print(dbgs());
	dbgs() << "\n";
}

Value *SIR::creatConstantBoolean(bool True) {
	return True ? ConstantInt::getTrue(C) : ConstantInt::getFalse(C);
}

IntegerType *SIR::createIntegerType(unsigned BitWidth) {
  return IntegerType::get(C, BitWidth);
}

Value *SIR::createIntegerValue(unsigned BitWidth, signed Val) {
  IntegerType *T = createIntegerType(BitWidth);
  return ConstantInt::getSigned(T, Val);
}

Value *SIR::createIntegerValue(const APInt &Val) {
	return ConstantInt::get(C, Val);
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

void SIR::printRegDecl(raw_ostream &OS) const {
	OS << "\n";

	for (SIR::const_register_iterator I = registers_begin(), E = registers_end();
		   I != E; ++I) {
		(*I)->printDecl(OS.indent(2));
	}
}

bool SIR::gcImpl() {
	// Remove all the instructions that is not be used anymore.
	Function *F = getFunction();

	// Visit the basic block in topological order.
	ReversePostOrderTraversal<BasicBlock *> RPO(&F->getEntryBlock());
	typedef ReversePostOrderTraversal<BasicBlock *>::rpo_iterator bb_top_iterator;

	for (bb_top_iterator BBI = RPO.begin(), BBE = RPO.end(); BBI != BBE; ++BBI) {
		BasicBlock *BB = *BBI;
		typedef BasicBlock::iterator iterator;
		for (iterator InstI = BB->begin(), InstE = BB->end(); InstI != InstE; ++InstI) {
			Instruction *Inst = InstI;

			if (Inst->use_empty() && !isa<ReturnInst>(Inst)) {
				Inst->eraseFromParent();
				return true;
			}
		}
	}

	return false;
}

bool SIR::gc() {
	bool changed = false;

	// Iteratively release the dead objects.
 	while (gcImpl())
 		changed = true;

	return changed;
}