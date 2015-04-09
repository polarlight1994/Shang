//===----------- SIRSubModuleBase.h - SubModules in SIR ---------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declare the classes for sub-modules in SIR.
//
//===----------------------------------------------------------------------===//

#ifndef SIR_SUB_MODULE_BASE_H
#define SIR_SUB_MODULE_BASE_H

#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/LangSteam.h"

#include "llvm/ADT/None.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Casting.h"

namespace llvm {
class SIRSubModuleBase : public ilist_node<SIRSubModuleBase> {
private:
	SmallVector<SIRRegister *, 8> Fanins;
	SmallVector<SIRRegister *, 8> Fanouts;

protected:
	// The Idx of all sub-modules in SIR.
	const unsigned Idx;
	const char *Name;	

	SIRSubModuleBase() : Idx(0) {}
	SIRSubModuleBase(const char *Name, unsigned Idx) : Name(Name), Idx(Idx) {}

	void addFanin(SIRRegister *Fanin);
	void addFanout(SIRRegister *Fanout);

public:
	~SIRSubModuleBase() {}

	const char *getName() const { return Name; }
	unsigned getNum() const { return Idx; }

	typedef SmallVectorImpl<SIRRegister *>::iterator fanin_iterator;
	fanin_iterator fanin_begin() { return Fanins.begin(); }
	fanin_iterator fanin_end() { return Fanins.end(); }

	typedef SmallVectorImpl<SIRRegister *>::const_iterator const_fanin_iterator;
	const_fanin_iterator fanin_begin() const { return Fanins.begin(); }
	const_fanin_iterator fanin_end()   const { return Fanins.end(); }

	SIRRegister *getFanin(unsigned Idx) const { return Fanouts[Idx]; }
	SIRRegister *getFanout(unsigned Idx) const { return Fanins[Idx]; }

	virtual void printDecl(raw_ostream &OS) const;
	virtual void print(vlang_raw_ostream &OS) const;
	void print() const;

};

class SIRSubModule : public SIRSubModuleBase {
	// Special ports in the sub-module.
	SIRRegister *StartPort, *FinPort, *RetPort;

	// The latency of the sub-module.
	unsigned Latency;

	SIRSubModule(const char *Name, unsigned FUNum)
		: SIRSubModuleBase(Name, FUNum), StartPort(0),
		  FinPort(0), RetPort(0), Latency(0) {}

public:
	SIRRegister *createStartPort(SIRCtrlRgnBuilder *SCRB);
	SIRRegister *getStartPort() const { return StartPort; }

	SIRRegister *createFinPort(SIRCtrlRgnBuilder *SCRB);
	SIRRegister *getFinPort() const { return FinPort; }

	SIRRegister *createRetPort(SIRCtrlRgnBuilder *SCRB, unsigned BitWidth,
		                         unsigned Latency = 0);
	SIRRegister *getRetPort() const { return RetPort; }

	unsigned getLatency() const { return Latency; }
	std::string getPortName(const Twine &PortName) const;

	void printDecl(raw_ostream &OS) const;

};
}

#endif
