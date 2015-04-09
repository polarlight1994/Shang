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

	// Read (from memory) data pin
	unsigned RDataWidth = getDataWidth();
	SIRRegister *RData = SCRB->createRegister(getRDataName(PortNum), RDataWidth,
		                                        0, 0, 0, SIRRegister::FUOutput);
	addFanout(RData);

	// Write (to memory) data pin
	unsigned WDataWidth = getDataWidth();
	SIRRegister *WData = SCRB->createRegister(getWDataName(PortNum), WDataWidth,
		                                        0, 0, 0, SIRRegister::FUInput);
	addFanin(WData);
}

void SIRMemoryBank::addGlobalVariable(GlobalVariable *GV, unsigned SizeInBytes) {

}

