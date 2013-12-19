//===------ VASTMemoryBank.h - Memory Banks in Verilog AST ------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the classes for memory ports in Verilog AST.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_MEMORY_PORT_H
#define VAST_MEMORY_PORT_H

#include "vast/VASTSubModules.h"
#include <map>

namespace llvm {
class GlobalVariable;

class VASTMemoryBank : public VASTSubModuleBase {
  const unsigned AddrSize, DataSize;
  const bool RequireByteEnable : 1;
  const bool IsDualPort : 1;
  const bool IsCombROM : 1;
  const unsigned ReadLatency : 5;
  static const unsigned InputsPerPort = 2;
  std::map<GlobalVariable*, unsigned> BaseAddrs;
  unsigned EndByteAddr;

  VASTMemoryBank(unsigned BusNum, unsigned AddrSize, unsigned DataSize,
                 bool RequireByteEnable, bool IsDualPort, bool IsCombinational,
                 unsigned ReadLatency);
  friend class VASTModule;

  void addPorts(VASTModule *VM);
  // Add all ports except byte enables.
  void addBasicPins(VASTModule *VM, unsigned PortNum);
  void addExternalPins(VASTModule *VM);
  void addByteEnables(VASTModule *VM, VASTNode *Parent, unsigned PortNum);

  // Signal names of the function unit.
  std::string getAddrName(unsigned PortNum) const;
  std::string getRDataName(unsigned PortNum) const;
  std::string getInernalRDataName(unsigned PortNum) const;
  std::string getWDataName(unsigned PortNum) const;
  std::string getByteEnName(unsigned PortNum) const;
  std::string getEnableName(unsigned PortNum) const;
  std::string getWriteEnName(unsigned PortNum) const;

  std::string getRDataName(unsigned PortNum, unsigned CurPipelineStage) const;
  std::string getAddrName(unsigned PortNum, unsigned CurPipelineStage) const;
  std::string getWDataName(unsigned PortNum, unsigned CurPipelineStage) const;
  std::string getByteEnName(unsigned PortNum, unsigned CurPipelineStage) const;

  void printPortDecl(raw_ostream &OS, unsigned PortNum) const;

  void writeInitializeFile(vlang_raw_ostream &OS) const;

  // Print the implementation of the memory blocks according to the requirement
  // of the byte enable.
  void printBank(vlang_raw_ostream &OS) const;
  void printBanksPort(vlang_raw_ostream &OS, unsigned PortNum,
                      unsigned BytesPerWord, unsigned ByteAddrWidth,
                      unsigned NumWords) const;

  void printBlockRAM(vlang_raw_ostream &OS) const;
  void printBlockPort(vlang_raw_ostream &OS, unsigned PortNum,
                      unsigned ByteAddrWidth, unsigned NumWords) const;

public:
  unsigned getDataWidth() const { return DataSize; }
  unsigned getAddrWidth() const { return AddrSize; }
  unsigned getByteEnWidth() const { return getDataWidth() / 8; }
  unsigned getByteAddrWidth() const;
  unsigned getReadLatency() const { return ReadLatency; }

  std::string getArrayName() const;

  bool isDefault() const { return Idx == 0; }
  bool requireByteEnable() const { return RequireByteEnable; }
  bool isDualPort() const { return IsDualPort; }
  bool isCombinationalROM() const { return IsCombROM; }
  unsigned getNumber() const { return Idx; }

  // The ports of the memory bus.
  VASTSelector *getByteEn(unsigned PortNum) const;
  VASTSelector *getAddr(unsigned PortNum) const;
  VASTSelector *getRData(unsigned PortNum) const;
  VASTSelector *getWData(unsigned PortNum) const;
  VASTSelector *getEnable() const;
  VASTSelector *getWriteEnable() const;

  void addGlobalVariable(GlobalVariable *GV, unsigned SizeInBytes);
  unsigned getStartOffset(GlobalVariable *GV) const;

  void printDecl(raw_ostream &OS) const;

  void print(vlang_raw_ostream &OS) const;
  void
  printAsCombROM(const VASTExpr *LHS, VASTValPtr Addr, raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTMemoryBank *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastMemoryBank;
  }
};
}

#endif
