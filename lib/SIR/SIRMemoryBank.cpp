//===------ SIRMemoryBank.cpp - Memory Banks in Verilog AST -----*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the classes for memory banks in Verilog AST.
//
//===----------------------------------------------------------------------===//

#include "sir/SIRMemoryBank.h"

using namespace llvm;

SIRMemoryBank::SIRMemoryBank(unsigned BusNum, unsigned AddrSize,
	                           unsigned DataSize, bool IsDualPort, unsigned ReadLatency)
  : BusNum(BusNum), AddrSize(AddrSize), DataSize(DataSize),
	  IsDualPort(IsDualPort), ReadLatency(ReadLatency) {}

void SIRMemoryBank::addPorts(SIR *SM) {
	addBasicPins(SM, 0);
	if (isDualPort()) addBasicPins(SM, 1);
}

void SIRMemoryBank::addBasicPins(SIR *SM, unsigned PortNum) {

}