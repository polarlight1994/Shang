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

#include "vtm/VASTNodeBases.h"
#include "vtm/VASTControlPathNodes.h"

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
  VASTSeqValue *getRAddr(unsigned PortNum) const {
    return getFanin(PortNum * 3);
  }

  VASTSeqValue *getWAddr(unsigned PortNum) const {
    return getFanin(PortNum * 3 + 1);
  }

  VASTSeqValue *getWData(unsigned PortNum) const {
    return getFanin(PortNum * 3 + 2);
  }

  void print(vlang_raw_ostream &OS, const VASTModule *Mod) const;

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

  VASTSubModule(const char *Name, unsigned FNNum)
    : VASTSubModuleBase(vastSubmodule, Name, FNNum) {}

  friend class VASTModule;
  void addPort(const std::string &Name, VASTValue *V, bool IsInput);
public:
  unsigned getNum() const { return Idx; }
  const char *getName() const { return Contents.Name; }

  typedef StringMap<VASTSubModulePortPtr>::const_iterator const_port_iterator;
  const_port_iterator port_begin() const { return PortMap.begin(); }
  const_port_iterator port_end() const { return PortMap.end(); }

  void addInPort(const std::string &Name, VASTValue *V) {
    addPort(Name, V, true);
  }

  void addOutPort(const std::string &Name, VASTValue *V) {
    addPort(Name, V, false);
  }

  std::string getSubModulePortName(const std::string &PortName) const;

  void print(vlang_raw_ostream &OS, const VASTModule *Mod) const;
};

class VASTRegister : public VASTNode {
  VASTSeqValue Value;
  uint64_t InitVal;

  VASTRegister(const char *Name, unsigned BitWidth, uint64_t InitVal,
               VASTNode::SeqValType T = VASTNode::Data, unsigned RegData = 0,
               const char *Attr = "");
  friend class VASTModule;
public:
  const char *const AttrStr;

  VASTSeqValue *getValue() { return &Value; }
  VASTSeqValue *operator->() { return getValue(); }

  const char *getName() const { return Value.getName(); }
  unsigned getBitWidth() const { return Value.getBitWidth(); }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTRegister *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastRegister;
  }

  typedef VASTSeqValue::AndCndVec AndCndVec;
  static void printCondition(raw_ostream &OS, const VASTSlot *Slot,
                             const AndCndVec &Cnds);

  void print(vlang_raw_ostream &OS, const VASTModule *Mod) const;
  void print(raw_ostream &OS) const;
};

}
#endif
