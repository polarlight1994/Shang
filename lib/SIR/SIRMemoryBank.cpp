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
	                           unsigned DataSize, bool IsDualPort, unsigned ReadLatency)
  : BusNum(BusNum), AddrSize(AddrSize), DataSize(DataSize),
	  IsDualPort(IsDualPort), ReadLatency(ReadLatency) {}

void SIRMemoryBank::addPorts(SIRCtrlRgnBuilder *SCRB) {
	addBasicPins(SCRB, 0);
	if (isDualPort()) addBasicPins(SCRB, 1);
}

void SIRMemoryBank::addBasicPins(SIRCtrlRgnBuilder *SCRB, unsigned PortNum) {
	// Address pin
	SIRRegister *Addr = SCRB->createRegister(getAddrName(PortNum), getAddrWidth(), 0,
		                                       0, 0, SIRRegister::FUInput);
	addFanin(Addr);

	// Write (to memory) data pin
	unsigned WDataWidth = getDataWidth();
	SIRRegister *WData = SCRB->createRegister(getWDataName(PortNum), WDataWidth,
		                                        0, 0, 0, SIRRegister::FUInput);
	addFanin(WData);

	// Read (from memory) data pin
	unsigned RDataWidth = getDataWidth();
	SIRRegister *RData = SCRB->createRegister(getRDataName(PortNum), RDataWidth,
		0, 0, 0, SIRRegister::FUOutput);
	addFanout(RData);
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

SIRRegister *SIRMemoryBank::getAddr(unsigned PortNum) const {
	return getFanin(InputsPerPort * PortNum + 0);
}

SIRRegister *SIRMemoryBank::getWData(unsigned PortNum) const {
	return getFanin(InputsPerPort * PortNum + 1);
}

SIRRegister *SIRMemoryBank::getRData(unsigned PortNum) const {
	return getFanout(PortNum);
}



