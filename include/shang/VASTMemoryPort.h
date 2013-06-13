//===------ VASTMemoryPort.h - Memory Ports in Verilog AST ------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
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

#include "shang/VASTSubModules.h"
#include <map>

namespace llvm {
class GlobalVariable;

class VASTMemoryBus : public VASTSubModuleBase {
  unsigned AddrSize, DataSize;
  bool RequireByteEnable;

  std::map<GlobalVariable*, unsigned> BaseAddrs;
  unsigned CurrentOffset;

  VASTMemoryBus(unsigned BusNum, unsigned AddrSize, unsigned DataSize,
                bool RequireByteEnable);
  friend class VASTModule;

  void addPorts(VASTModule *VM);
  // Add all ports except byte enables.
  void addBasicPorts(VASTModule *VM, VASTNode *Parent);
  void addByteEnables(VASTModule *VM, VASTNode *Parent);

  // Signal names of the function unit.
  std::string getRAddrName() const;

  std::string getWAddrName() const;

  std::string getRDataName() const;

  std::string getWDataName() const;

  std::string getWByteEnName() const;

  std::string getRByteEnName() const;

  std::string getWEnName() const;

  std::string getREnName() const;

  void writeInitializeFile(vlang_raw_ostream &OS) const;
  // Print the implementation of the memory blocks according to the requirement
  // of the byte enable.
  void printBank(vlang_raw_ostream &OS, const VASTModule *Mod) const;
  void printBlockRAM(vlang_raw_ostream &OS, const VASTModule *Mod) const;
public:
  unsigned getDataWidth() const { return DataSize; }
  unsigned getAddrWidth() const { return AddrSize; }
  unsigned getByteEnWdith() const { return getDataWidth() / 8; }

  std::string getArrayName() const;
  VASTValPtr getFinalRDataShiftAmountOperand(VASTModule *VM) const;

  bool isDefault() const { return Idx == 0; }
  bool requireByteEnable() const { return RequireByteEnable; }

  // The read port of the memory bus.
  VASTSelector *getREnable() const;
  VASTSelector *getRByteEn() const;
  VASTSelector *getRAddr() const;
  VASTSelector *getRData() const;

  // The write port of the memory bus.
  VASTSelector *getWEnable() const;
  VASTSelector *getWByteEn() const;
  VASTSelector *getWAddr() const;
  VASTSelector *getWData() const;

  void addGlobalVariable(GlobalVariable *GV, unsigned SizeInBytes);
  unsigned getStartOffset(GlobalVariable *GV) const;

  void printDecl(raw_ostream &OS) const;

  void print(vlang_raw_ostream &OS, const VASTModule *Mod) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTMemoryBus *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastMemoryBus;
  }
};
}

#endif
