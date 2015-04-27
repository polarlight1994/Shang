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

void SIRSubModuleBase::addFanin(SIRRegister *Fanin) {
	Fanins.push_back(Fanin);
}

void SIRSubModuleBase::addFanout(SIRRegister *Fanout) {
	Fanouts.push_back(Fanout);
}

std::string SIRSubModule::getPortName(const Twine &PortName) const {
	return "SubMod" + utostr(getNum()) + "_" + PortName.str();
}

void SIRSubModule::printDecl(raw_ostream &OS) const {
	if (SIRRegister *Start = getStartPort())
		Start->printDecl(OS);
	if (SIRRegister *Fin = getFinPort())
		Fin->printDecl(OS);
	if (SIRRegister *Ret = getRetPort())
		Ret->printDecl(OS);
}

SIRMemoryBank::SIRMemoryBank(unsigned BusNum, unsigned AddrSize,
	                           unsigned DataSize, unsigned ReadLatency)
	: SIRSubModuleBase(MemoryBank, BusNum), AddrSize(AddrSize),
	DataSize(DataSize),	ReadLatency(ReadLatency), EndByteAddr(0) {}

void SIRMemoryBank::addGlobalVariable(GlobalVariable *GV, unsigned SizeInBytes) {
	DEBUG(dbgs() << "Insert the GV [" << GV->getName() 
		<< "] to offset [" << EndByteAddr << "\n");

	// Hack: I think if GV->getAlignment < (DataSiz / 8), then we can align the
	// EndByteAddr to the (DataSize / 8).
	assert(GV->getAlignment() >= (DataSize / 8) && "Bad GV alignment!");
	assert(EndByteAddr % (DataSize / 8) == 0 && "Bad Current Offset!");

	/// Insert the GV to the offset map, and calculate its offset in the byte address.
	// Roundup the address to align to the GV alignment.
	EndByteAddr = RoundUpToAlignment(EndByteAddr, GV->getAlignment());

	DEBUG(dbgs() << "Roundup EndByteAddr to [" << EndByteAddr
		<< "] according to alignment " << GV->getAlignment() << '\n');

	// Insert the GV at the address of EndByteAddr.
	bool inserted = BaseAddrs.insert(std::make_pair(GV, EndByteAddr)).second;
	assert(inserted && "GV had already been added before!");

	DEBUG(dbgs() << "Insert the GV with size [" << SizeInBytes
		<< "] and Offset increase to " << (EndByteAddr + SizeInBytes) << "\n");

	// Round up the address again to align to the (DataSize / 8).
	EndByteAddr = RoundUpToAlignment(EndByteAddr + SizeInBytes, DataSize / 8);

	DEBUG(dbgs() << "Roundup to Word address " << EndByteAddr << "\n");	
}

unsigned SIRMemoryBank::getOffset(GlobalVariable *GV) const {
	std::map<GlobalVariable *, unsigned>::const_iterator at = BaseAddrs.find(GV);
	assert(at != BaseAddrs.end() && "GV is not inserted to offset map!");

	return at->second;
}

bool SIRMemoryBank::indexGV2OriginalPtrSize(GlobalVariable *GV, unsigned PtrSize) {
	return GVs2PtrSize.insert(std::make_pair(GV, PtrSize)).second;
}

SIRRegister *SIRMemoryBank::getAddr() const {
	return getFanin(0);
}

SIRRegister *SIRMemoryBank::getWData() const {
	return getFanin(1);
}

SIRRegister *SIRMemoryBank::getRData() const {
	return getFanout(0);
}

SIRRegister *SIRMemoryBank::getEnable() const {
	return getFanin(2);
}

SIRRegister *SIRMemoryBank::getWriteEnable() const {
	return getFanin(3);
}

std::string SIRMemoryBank::getAddrName() const {
	return "mem" + utostr(Idx) + "addr";
}

std::string SIRMemoryBank::getRDataName() const {
	return "mem" + utostr(Idx) + "rdata";
}

std::string SIRMemoryBank::getWDataName() const {
	return "mem" + utostr(Idx) + "wdata";
}

std::string SIRMemoryBank::getEnableName() const {
	return "mem" + utostr(Idx) + "en";
}

std::string SIRMemoryBank::getWriteEnName() const {
	return "mem" + utostr(Idx) + "wen";
}

std::string SIRMemoryBank::getArrayName() const {
	return "mem" + utostr(Idx) + "ram";
}

void SIRMemoryBank::printDecl(raw_ostream &OS) const {
	typedef std::map<GlobalVariable*, unsigned>::const_iterator iterator;
	for (iterator I = const_baseaddrs_begin(), E = const_baseaddrs_end();
		I != E; ++I) {
			unsigned PtrSize = lookupPtrSize(I->first);
			OS << "reg" << BitRange(PtrSize, 0, false);
			OS << " " << Mangle(I->first->getName());
			// Print the initial offset.
			OS << " = " << buildLiteralUnsigned(I->second, PtrSize)
				 << ";\n";
	}

	getAddr()->printDecl(OS.indent(2));
	getRData()->printDecl(OS.indent(2));
	getWData()->printDecl(OS.indent(2));
	getEnable()->printDecl(OS.indent(2));
	getWriteEnable()->printDecl(OS.indent(2));
}

void SIRSeqOp::print(raw_ostream &OS) const {
	OS << "Assign the Src Value " << "[" <<Src->getName()
		 << "]" << " to register " << "[" << DstReg->getName()
		 << "]" << " in Slot #" << S->getSlotNum()
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

	typedef const_register_iterator iterator;
	for (iterator I = const_registers_begin(), E = const_registers_end();
		   I != E; ++I) {
    SIRRegister *Reg = *I;

    // Do not need to declaration registers for the output and FUInOut,
		// since we have do it elsewhere.
    if (Reg->isOutPort() || Reg->isFUInOut()) continue;

		Reg->printDecl(OS.indent(2));
	}
}

void SIR::printMemoryBankDecl(raw_ostream &OS) const {
	OS << "\n";	

	typedef const_submodulebase_iterator iterator;
	for (iterator I = const_submodules_begin(), E = const_submodules_end(); I != E; ++I) {
		if (SIRMemoryBank *SMB = dyn_cast<SIRMemoryBank>(*I)) {
			SMB->printDecl(OS.indent(2));			
		}
	}
}

bool SIR::gcImpl() {
	// Remove all the instructions that is not be used anymore.
	Function *F = getFunction();

	// Visit all BasicBlock in Function to delete all useless instruction.
	typedef Function::iterator iterator;
	for (iterator BBI = F->begin(), BBE = F->end(); BBI != BBE; ++BBI) {
		BasicBlock *BB = BBI;
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

void SIR::print(raw_ostream &OS) {
	// Print all information about SIR.
	OS << "======================Note=====================\n" << "All INFO of SIR will be printed by default!\n";
	OS << "If you want to view only part of it, please select following dump methods:\n";
	OS << "(1) IRInfo: dumpIR(); (2) BB2SlotInfo: dumpBB2Slot();\n";
	OS << "(3) Reg2SlotInfo: dumpReg2Slot()\n";
	OS << "===============================================\n";

  dumpIR();
	dumpBB2Slot();
	dumpReg2Slot();
	dumpSeqOp2Slot();

	OS << "======================Note=====================\n" << "All INFO of SIR will be printed by default!\n";
	OS << "If you want to view only part of it, please select following dump methods:\n";
	OS << "(1) IRInfo: dumpIR(); (2) BB2SlotInfo: dumpBB2Slot();\n";
	OS << "(3) Reg2SlotInfo: dumpReg2Slot()\n";
	OS << "===============================================\n";
}

void SIR::dump() {
	print(dbgs());
	dbgs() << "\n";
}

void SIR::dumpIR() {
	Function *F = this->getFunction();

	F->dump();
}

void SIR::dumpBB2Slot() {
	raw_ostream &OS = dbgs();
	Function *F = this->getFunction();

	OS << "\n";

	// Print the entry slot.
	OS << "Entry Slot#0\n";

	typedef Function::iterator iterator;
	for (iterator I = F->begin(), E = F->end(); I != E; I++) {
		BasicBlock *BB = I;

		// Get the corresponding landing slot and latest slot.
		SIRSlot *LandingSlot = getLandingSlot(BB);
		SIRSlot *LatestSlot = getLatestSlot(BB);

		OS << "BasicBlock [" << BB->getName() << "] with landing slot "
			 << "Slot#" << LandingSlot->getSlotNum() << " and latest slot "
			 << "Slot#" << LatestSlot->getSlotNum() << "\n";
	}

	OS << "\n";
}

void SIR::dumpReg2Slot() {
	raw_ostream &OS = dbgs();	

	OS << "\n";

	for (const_register_iterator I = registers_begin(), E = registers_end(); I != E; I++) {
		SIRRegister *Reg = *I;

		// Get the corresponding slot.
		SIRSlot *Slot = lookupSIRSlot(Reg);

		OS << "Register [" << Reg->getName() << "] in slot "
			 << "Slot#" << Slot->getSlotNum() << "\n";
	}

	OS << "\n";
}

void SIR::dumpSeqOp2Slot() {
	raw_ostream &OS = dbgs();	

	OS << "\n";

	typedef seqop_iterator iterator;
	for (iterator I = seqop_begin(), E = seqop_end(); I != E; I++) {
		SIRSeqOp *SeqOp = *I;

		// Get the corresponding slot.
		SIRSlot *Slot = SeqOp->getSlot();

		OS << "Assign Value [" << SeqOp->getSrc()->getName() << "] to register ["
			 << SeqOp->getDst()->getName() << "] in slot Slot#" << Slot->getSlotNum()
			 << "\n";
	}
}