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

void SIRRegister::printVirtualPortDecl(raw_ostream &OS, bool IsInput) const {
	OS << ",\n (* altera_attribute = \"-name VIRTUAL_PIN on\" *)";
	OS.indent(4);

	if (IsInput)
		OS << "input ";
	else
		OS << "output ";

	// If it is a output, then it should be a register.
	if (!IsInput)
		OS << "reg";
	else
		OS << "wire";

	if (getBitWidth() > 1) OS << "[" << utostr_32(getBitWidth() - 1) << ":0]";

	OS << " " << Mangle(getName());
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

SIRMemoryBank::SIRMemoryBank(unsigned BusNum, unsigned AddrSize, unsigned DataSize,
	                           bool RequireByteEnable, bool IsReadOnly, unsigned ReadLatency)
	: SIRSubModuleBase(MemoryBank, BusNum), AddrSize(AddrSize),
	DataSize(DataSize),	RequireByteEnable(RequireByteEnable),
	IsReadOnly(IsReadOnly), ReadLatency(ReadLatency), EndByteAddr(0) {}

unsigned SIRMemoryBank::getByteAddrWidth() const {
	assert(requireByteEnable() && "Unexpected non-RequiredByteEnable memory bus!");

	// To access the memory bus in byte size, the address width need to increase
	// by log2(DataWidth).
	unsigned ByteAddrWidth = Log2_32_Ceil(getDataWidth() / 8);
	assert(ByteAddrWidth && "Unexpected NULL ByteAddrWidth!");

	return ByteAddrWidth;
}

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

SIRRegister *SIRMemoryBank::getWriteEn() const {
	return getFanin(3);
}

SIRRegister *SIRMemoryBank::getByteEn() const {
	return getFanin(4);
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

std::string SIRMemoryBank::getByteEnName() const {
	return "mem" + utostr(Idx) + "byteen";
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
	getWriteEn()->printDecl(OS.indent(2));

	if (requireByteEnable())
		getByteEn()->printDecl(OS.indent(2));
}

void SIRMemoryBank::printVirtualPortDecl(raw_ostream &OS) const {
	getEnable()->printVirtualPortDecl(OS, false);
	getWriteEn()->printVirtualPortDecl(OS, false);
	getAddr()->printVirtualPortDecl(OS, false);
	getRData()->printVirtualPortDecl(OS, true);
	getWData()->printVirtualPortDecl(OS, false);

	if (requireByteEnable())
		getByteEn()->printVirtualPortDecl(OS, false);
}

void SIRSeqOp::print(raw_ostream &OS) const {
	OS << "Assign the Src Value " << "[" <<Src->getName()
		 << "]" << " to register " << "[" << DstReg->getName()
		 << "]" << " in Slot #" << S->getSlotNum()
		 << " Scheduled to " << S->getStepInLocalBB()
		 << " in local BB"<< "\n";
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

		// Find the this slot in the PredSlot of the successor and erase it.
		pred_iterator at
			= std::find(SuccSlot->PredSlots.begin(), SuccSlot->PredSlots.end(), this);
		SuccSlot->PredSlots.erase(at);
	}

	NextSlots.clear();
}

void SIRSlot::print(raw_ostream &OS) const {
	OS << "Slot #" << SlotNum << " Scheduled to " << Step
		 << " in local BB Guarding by "
     << getGuardValue()->getName() << "\n";
}

void SIRSlot::dump() const {
	print(dbgs());
	dbgs() << "\n";
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
				// If the Inst is a pseudo instruction, then it is a register.
				// The GC of useless register is not handled here.
				if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst))
					if (II->getIntrinsicID() == Intrinsic::shang_pseudo)
						continue;

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

  //dumpIR(OS);
	dumpBB2Slot(OS);
	//dumpReg2Slot(OS);
	//dumpSeqOp2Slot(OS);

	OS << "======================Note=====================\n" << "All INFO of SIR will be printed by default!\n";
	OS << "If you want to view only part of it, please select following dump methods:\n";
	OS << "(1) IRInfo: dumpIR(); (2) BB2SlotInfo: dumpBB2Slot();\n";
	OS << "(3) Reg2SlotInfo: dumpReg2Slot()\n";
	OS << "===============================================\n";
}

void SIR::dump(raw_ostream &OS) {
	print(OS);
	OS << "\n";
}

void SIR::dumpIR(raw_ostream &OS) {
	Function *F = this->getFunction();

	F->print(OS);
}

void SIR::dumpBB2Slot(raw_ostream &OS) {
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

void SIR::dumpReg2Slot(raw_ostream &OS) {
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

void SIR::dumpSeqOp2Slot(raw_ostream &OS) {
	OS << "\n";

	typedef seqop_iterator iterator;
	for (iterator I = seqop_begin(), E = seqop_end(); I != E; I++) {
		SIRSeqOp *SeqOp = I;

		// Get the corresponding slot.
		SIRSlot *Slot = SeqOp->getSlot();

		OS << "Assign Value [" << SeqOp->getSrc()->getName() << "] to register ["
			 << SeqOp->getDst()->getName() << "] in slot Slot#" << Slot->getSlotNum()
			 << "\n";
	}
}