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

namespace llvm {
class VASTBlockRAM : public VASTNode {
  unsigned Depth;
  unsigned WordSize;
  unsigned BRamNum;
public:
  VASTSeqValue WritePortA;
private:
  void printSelector(raw_ostream &OS, const VASTSeqValue &Port) const;
  void printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod,
                       const VASTSeqValue &Port) const;

  VASTBlockRAM(const char *Name, unsigned BRamNum, unsigned WordSize,
               unsigned Depth)
    : VASTNode(vastBlockRAM), Depth(Depth), WordSize(WordSize),
      BRamNum(BRamNum),
      WritePortA(Name, WordSize, VASTNode::BRAM, BRamNum, *this)
  {}

  friend class VASTModule;
public:
  typedef VASTSeqValue::assign_itertor assign_itertor;
  unsigned getBlockRAMNum() const { return BRamNum; }

  unsigned getWordSize() const { return WordSize; }
  unsigned getDepth() const { return Depth; }

  void printSelector(raw_ostream &OS) const {
    printSelector(OS, WritePortA);
  }

  void printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod) const {
    printAssignment(OS, Mod, WritePortA);
  }

  void print(raw_ostream &OS) const {}

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTBlockRAM *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastBlockRAM;
  }

};

class VASTRegister : public VASTNode {
  VASTSeqValue Value;
  uint64_t InitVal;

  VASTRegister(const char *Name, unsigned BitWidth, uint64_t InitVal,
               VASTNode::SeqValType T = VASTNode::Data, unsigned RegData = 0,
               const char *Attr = "");
  friend class VASTModule;

  void dropUses() {
    assert(0 && "Function not implemented!");
  }

public:
  const char *const AttrStr;

  VASTSeqValue *getValue() { return &Value; }
  VASTSeqValue *operator->() { return getValue(); }

  const char *getName() const { return Value.getName(); }
  unsigned getBitWidth() const { return Value.getBitWidth(); }

  typedef VASTSeqValue::assign_itertor assign_itertor;
  assign_itertor assign_begin() const { return Value.begin(); }
  assign_itertor assign_end() const { return Value.end(); }
  unsigned num_assigns() const { return Value.size(); }

  void printSelector(raw_ostream &OS) const;

  // Print data transfer between registers.
  void printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod) const;
  // Return true if the reset is actually printed.
  bool printReset(raw_ostream &OS) const;
  void dumpAssignment() const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTRegister *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastRegister;
  }

  typedef VASTSeqValue::AndCndVec AndCndVec;
  static void printCondition(raw_ostream &OS, const VASTSlot *Slot,
                             const AndCndVec &Cnds);

  void print(raw_ostream &OS) const {}
};

}
#endif
