//===--------- SIRSubModuleBase.cpp - SubModules in SIR ---------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the classes for sub-modules in SIR.
//
//===----------------------------------------------------------------------===//

#include "sir/SIRSubModuleBase.h"

using namespace llvm;

void SIRSubModuleBase::addFanin(SIRRegister *Fanin) {
	Fanins.push_back(Fanin);
}

void SIRSubModuleBase::addFanout(SIRRegister *Fanout) {
	Fanouts.push_back(Fanout);
}

void SIRSubModuleBase::print() const {
	vlang_raw_ostream S(dbgs());
	print(S);
}

std::string SIRSubModule::getPortName(const Twine &PortName) const {
	return "SubMod" + utostr(getNum()) + "_" + PortName.str();
}

SIRRegister *SIRSubModule::createStartPort(SIRCtrlRgnBuilder *SCRB) {
	SIRRegister *Reg 
		= SCRB->createRegister(getPortName("start"), 1, 0, 0, 0, SIRRegister::FUInput);
	StartPort = Reg;
	addFanin(StartPort);

	return StartPort;
}

SIRRegister *SIRSubModule::createFinPort(SIRCtrlRgnBuilder *SCRB) {
	SIRRegister *Reg
		= SCRB->createRegister(getPortName("fin"), 1, 0, 0, 0, SIRRegister::FUOutput);
	FinPort = Reg;
	addFanout(FinPort);

	return FinPort;
}

SIRRegister *SIRSubModule::createRetPort(SIRCtrlRgnBuilder *SCRB,
	                                       unsigned BitWidth, unsigned Latency) {
	SIRRegister *Reg
		= SCRB->createRegister(getPortName("return_value"), BitWidth, 0, 0, 0, SIRRegister::FUOutput);
	RetPort = Reg;
	addFanout(RetPort);

	return RetPort;
}

void SIRSubModule::printDecl(raw_ostream &OS) const {
	if (SIRRegister *Start = getStartPort())
		Start->printDecl(OS);
	if (SIRRegister *Fin = getFinPort())
		Fin->printDecl(OS);
	if (SIRRegister *Ret = getRetPort())
		Ret->printDecl(OS);
}