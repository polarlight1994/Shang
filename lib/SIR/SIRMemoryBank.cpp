//===----------- SIRMemoryBank.cpp - Memory Banks in SIR --------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the classes for memory banks in SIR.
//
//===----------------------------------------------------------------------===//

#include "sir/SIRMemoryBank.h"

#include "llvm/Support/Debug.h"

using namespace llvm;

SIRMemoryBank::SIRMemoryBank(unsigned BusNum, unsigned AddrSize,
	                           unsigned DataSize, unsigned ReadLatency)
  : BusNum(BusNum), AddrSize(AddrSize), DataSize(DataSize),
	  ReadLatency(ReadLatency) {}

void SIRMemoryBank::addPorts(SIRCtrlRgnBuilder *SCRB) {
	addBasicPins(SCRB);
}

void SIRMemoryBank::addBasicPins(SIRCtrlRgnBuilder *SCRB) {
	// Address pin
	SIRRegister *Addr = SCRB->createRegister(getAddrName(), getAddrWidth(), 0,
		                                       0, 0, SIRRegister::FUInput);
	addFanin(Addr);

	// Write (to memory) data pin
	unsigned WDataWidth = getDataWidth();
	SIRRegister *WData = SCRB->createRegister(getWDataName(), WDataWidth,
		                                        0, 0, 0, SIRRegister::FUInput);
	addFanin(WData);

	// Read (from memory) data pin
	unsigned RDataWidth = getDataWidth();
	SIRRegister *RData = SCRB->createRegister(getRDataName(), RDataWidth,
		                                        0, 0, 0, SIRRegister::FUOutput);
	addFanout(RData);

	// Enable pin
	SIRRegister *Enable = SCRB->createRegister(getEnableName(), 1, 0, 0,
		                                         0, SIRRegister::FUInput);
	addFanin(Enable);

	// Write enable pin
	SIRRegister *WriteEn = SCRB->createRegister(getWriteEnName(), 1, 0, 0,
		                                          0, SIRRegister::FUInput);
	addFanin(WriteEn);
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
	getAddr()->printDecl(OS);
	getRData()->printDecl(OS);
	getWData()->printDecl(OS);
  getEnable()->printDecl(OS);
	getWriteEnable()->printDecl(OS);
}

void SIRMemoryBank::printMemoryBank(vlang_raw_ostream &OS) const {
	
}
