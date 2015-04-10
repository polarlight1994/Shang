//===------- SIRMemoryBank.h - Memory Banks in Verilog AST ------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the classes for memory banks in Verilog AST.
//
//===----------------------------------------------------------------------===//

#ifndef SIR_MEMORY_BANK_H
#define SIR_MEMORY_BANK_H

#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRSubModuleBase.h"
#include "sir/LangSteam.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"

#include <map>

using namespace llvm;

namespace llvm {
class SIRMemoryBank : public SIRSubModuleBase {
	const unsigned BusNum;
	const unsigned AddrSize, DataSize;
	const unsigned ReadLatency : 5;
	// For each MemoryBank, we have two input port
	// including Address and WData.
	static const unsigned InputsPerPort = 2;
  // The address of last byte.
	unsigned EndByteAddr;

	// The map between GVs and its offset in memory bank
	std::map<GlobalVariable *, unsigned> BaseAddrs;

	SIRMemoryBank(unsigned BusNum, unsigned AddrSize,
		            unsigned DataSize, unsigned ReadLatency);
	friend class SIR;

	void addPorts(SIRCtrlRgnBuilder *SCRB);
	void addBasicPins(SIRCtrlRgnBuilder *SCRB);	

	// Signal names of the memory bank.
	std::string getAddrName() const;
	std::string getRDataName() const;
	std::string getWDataName() const;
	std::string getEnableName() const;
	std::string getWriteEnName() const;
	
	void printInitializeFile(vlang_raw_ostream &OS) const;
	void printMemoryBank(vlang_raw_ostream &OS) const;
	void printMemoryBankPort(vlang_raw_ostream &OS, unsigned ByteAddrWidth,
		                       unsigned NumWords) const;

public:
	unsigned getDataWidth() const { return DataSize; }
	unsigned getAddrWidth() const { return AddrSize; }
	unsigned getReadLatency() const { return ReadLatency; }

	std::string getArrayName() const;

	SIRRegister *getAddr() const;
	SIRRegister *getRData() const;
	SIRRegister *getWData() const;
	SIRRegister *getEnable() const;
	SIRRegister *getWriteEnable() const;

	void addGlobalVariable(GlobalVariable *GV, unsigned SizeInBytes);
	unsigned getOffset(GlobalVariable *GV) const;

	void printDecl(raw_ostream &OS) const;
	void print(vlang_raw_ostream &OS) const;
};
}

#endif