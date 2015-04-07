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
class SIRMemoryBank {
	const unsigned BusNum;
	const unsigned AddrSize, DataSize;
	// The read port and write port has different clk.
	const bool IsDualPort : 1;
	const unsigned ReadLatency : 5;

	// The map between GVs and its offset in memory bank
	std::map<GlobalVariable *, unsigned> BaseAddrs;

	SIRMemoryBank(unsigned BusNum, unsigned AddrSize, unsigned DataSize,
		            bool IsDualPort, unsigned ReadLatency);
	friend class SIR;

	void addPorts(SIR *SM);
	void addBasicPins(SIR *SM, unsigned PortNum);	

	// Signal names of the memory bank.
	std::string getAddrName(unsigned PortNum) const;
	std::string getRDataName(unsigned PortNum) const;
	std::string getWDataName(unsigned PortNum) const;
	std::string getEnableName(unsigned PortNum) const;
	std::string getWriteEnName(unsigned PortNum) const;
	
	void printInitializeFile(vlang_raw_ostream &OS) const;
	void printMemoryBank(vlang_raw_ostream &OS) const;
	void printMemoryBankPort(vlang_raw_ostream &OS, unsigned PortNum,
		                       unsigned ByteAddrWidth, unsigned NumWords) const;

public:
	unsigned getDataWidth() const { return DataSize; }
	unsigned getAddrWidth() const { return AddrSize; }
	unsigned getReadLatency() const { return ReadLatency; }

	bool isDualPort() const { return IsDualPort; }

	unsigned getByteAddrWidth() const;
	std::string getArrayName() const;

	SIRRegister *getAddr(unsigned PortNum) const;
	SIRRegister *getRData(unsigned PortNum) const;
	SIRRegister *getWData(unsigned PortNum) const;
	SIRRegister *getEnable() const;
	SIRRegister *getWriteEnable() const;

	void addGlobalVariable(GlobalVariable *GV, unsigned SizeInBytes);
	unsigned getStartOffset(GlobalVariable *GV) const;

	void printDecl(raw_ostream &OS) const;
	void print(vlang_raw_ostream &OS) const;
};
}

#endif