//===---- VASTSubModules.h - Submodules in VerilogAST -----------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the Submodules in the Verilog Abstract Syntax Tree.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_SUBMODULES_H
#define VAST_SUBMODULES_H

#include "shang/VASTRegister.h"

#include "llvm/ADT/StringMap.h"

namespace llvm {
class GlobalVariable;

class VASTBlockRAM : public VASTSubModuleBase {
  unsigned Depth;
  unsigned WordSize;
  // TODO: Support multiple initializers.
  const GlobalVariable *Initializer;

  VASTBlockRAM(const char *Name, unsigned BRamNum, unsigned WordSize,
               unsigned Depth, const GlobalVariable *Initializer)
    : VASTSubModuleBase(vastBlockRAM, Name, BRamNum), Depth(Depth),
      WordSize(WordSize), Initializer(Initializer)
  {}

  friend class VASTModule;

  void printPort(vlang_raw_ostream &OS, unsigned Num) const;
  void addPorts(VASTModule *VM);
public:
  unsigned getBlockRAMNum() const { return Idx; }
  unsigned getWordSize() const { return WordSize; }
  unsigned getDepth() const { return Depth; }
  unsigned getAddrWidth() const { return Log2_32_Ceil(getDepth()); }

  // Get the buses to block RAM.
  VASTRegister *getRAddr(unsigned PortNum) const {
    return getFanin(PortNum * 3);
  }

  VASTValue *getRData(unsigned PortNum) const {
    return getFanout(PortNum);
  }

  VASTRegister *getWAddr(unsigned PortNum) const {
    return getFanin(PortNum * 3 + 1);
  }

  VASTRegister *getWData(unsigned PortNum) const {
    return getFanin(PortNum * 3 + 2);
  }

  void print(vlang_raw_ostream &OS, const VASTModule *Mod) const;
  void printDecl(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTBlockRAM *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastBlockRAM;
  }
};

class VASTSubModule : public VASTSubModuleBase {
  // Remember the input/output flag in the pointer.
  typedef PointerIntPair<VASTValue*, 1, bool> VASTSubModulePortPtr;
  StringMap<VASTSubModulePortPtr> PortMap;
  // Can the submodule be simply instantiated?
  bool IsSimple;
  // Special ports in the submodule.
  VASTRegister *StartPort, *FinPort, *RetPort;

  // The latency of the submodule.
  unsigned Latency;

  VASTSubModule(const char *Name, unsigned FNNum)
    : VASTSubModuleBase(vastSubmodule, Name, FNNum), StartPort(0), FinPort(0),
      RetPort(0), Latency(0) {}

  friend class VASTModule;
  void addPort(const std::string &Name, VASTValue *V, bool IsInput);
  void
  printSimpleInstantiation(vlang_raw_ostream &OS, const VASTModule *Mod) const;
  void printInstantiationFromTemplate(vlang_raw_ostream &OS,
                                      const VASTModule *Mod) const;
public:
  unsigned getNum() const { return Idx; }
  const char *getName() const { return Contents.Name; }

  void setIsSimple(bool isSimple = true) { IsSimple = isSimple; }

  typedef StringMap<VASTSubModulePortPtr>::const_iterator const_port_iterator;
  const_port_iterator port_begin() const { return PortMap.begin(); }
  const_port_iterator port_end() const { return PortMap.end(); }

  void addInPort(const std::string &Name, VASTRegister *V) {
    addPort(Name, V, true);
  }

  void addOutPort(const std::string &Name, VASTValue *V) {
    addPort(Name, V, false);
  }

  VASTRegister *createStartPort(VASTModule *VM);
  VASTRegister *getStartPort() const { return StartPort; }

  VASTRegister *createFinPort(VASTModule *VM);
  VASTRegister *getFinPort() const { return FinPort; }

  VASTRegister *createRetPort(VASTModule *VM, unsigned Bitwidth,
                              unsigned Latency = 0);
  VASTRegister *getRetPort() const { return RetPort; }

  void printDecl(raw_ostream &OS) const;

  // Get the latency of the submodule.
  unsigned getLatency() const { return Latency; }

  static std::string getPortName(unsigned FNNum, const std::string &PortName);
  std::string getPortName(const std::string &PortName) const {
    return getPortName(getNum(), PortName);
  }

  void print(vlang_raw_ostream &OS, const VASTModule *Mod) const;
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSubModule *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSubmodule;
  }
};
}
#endif
