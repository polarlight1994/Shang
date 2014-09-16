//===----------- SIR.h - Modules in SIR -----------*- C++ -*-===//
//
//                      The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the SIR.
//
//===----------------------------------------------------------------------===//
#include "llvm/IR/Value.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/ValueMap.h"

#ifndef SIR_MODULE_H
#define SIR_MODULE_H

namespace llvm {
// Represent the Mux structure in Verilog
class SIRSelector : public ilist_node<SIRSelector> {
private:
  SIRSelector(const SIRSelector &) LLVM_DELETED_FUNCTION;
  void operator=(const SIRSelector &) LLVM_DELETED_FUNCTION;

  // Map the transaction condition to transaction value.
  typedef std::vector<Value *> FaninVector;
  FaninVector Fanins;

  typedef std::vector<Value *> FaninGuardVector;
  FaninGuardVector FaninGuards;

  const std::string Name;
  unsigned BitWidth;

public:
  SIRSelector(std::string Name = "", unsigned BitWidth = 0)
    : Name(Name), BitWidth(BitWidth) {}

  const std::string getName() const { return Name; }
  unsigned getBitWidth() const { return BitWidth; }

  typedef FaninVector::const_iterator const_iterator;
  const_iterator assign_begin() const { return Fanins.begin(); }
  const_iterator assign_end() const { return Fanins.end(); }
  typedef FaninVector::iterator iterator;
  iterator assign_begin() { return Fanins.begin(); }
  iterator assign_end() { return Fanins.end(); }
  unsigned assign_size() const { return Fanins.size(); }
  bool assign_empty() const { return Fanins.empty(); }

  typedef FaninGuardVector::const_iterator const_guard_iterator;
  const_guard_iterator guard_begin() const { return FaninGuards.begin(); }
  const_guard_iterator guard_end() const { return FaninGuards.end(); }
  typedef FaninGuardVector::iterator guard_iterator;
  guard_iterator guard_begin() { return FaninGuards.begin(); }
  guard_iterator guard_end() { return FaninGuards.end(); }
  unsigned guard_size() const { return FaninGuards.size(); }
  bool guard_empty() const { return FaninGuards.empty(); }

  void addAssignment(Value *Fanin, Value *FaninGuard);

  // Print the declaration of this selector
  void printDecl(raw_ostream &OS) const;
};

// Represent the registers in the Verilog.
class SIRRegister : public ilist_node<SIRRegister> {
private:
  const uint64_t InitVal;
  SIRSelector *Sel;

public:
  SIRRegister(unsigned BitWidth = 0, std::string Name = "",
              uint64_t InitVal = 0) : InitVal(InitVal) {
    this->Sel = new SIRSelector(Name, BitWidth);
  }
  SIRRegister(SIRSelector *Sel, uint64_t InitVal = 0)
    : Sel(Sel), InitVal(InitVal) {}

  SIRSelector *getSelector() const { return Sel; }

  // Forward the functions from the Selector.
  std::string getName() const { return getSelector()->getName(); }
  unsigned getBitWidth() const { return getSelector()->getBitWidth(); }
  void addAssignment(Value *Fanin, Value *FaninGuard) {
    return getSelector()->addAssignment(Fanin, FaninGuard);
  }
  void printDecl(raw_ostream &OS) const {
    return getSelector()->printDecl(OS);
  }
};

// Represent the ports in the Verilog.
class SIRPort : public ilist_node<SIRPort> {
public:
  enum SIRPortTypes {
    InPort,
    OutPort
  };

protected:
  virtual std::string getNameImpl() const;
  virtual unsigned getBitWidthImpl() const;
  virtual SIRPortTypes getPortTypeImpl() const;
public:
  SIRPort();
  virtual ~SIRPort();

  const std::string getName() const { return getNameImpl(); }
  unsigned getBitWidth() const { return getBitWidthImpl(); }
  SIRPortTypes getPortType() const { return getPortTypeImpl(); }
  bool isInput() const { return getPortType() == InPort; }

  // Print the port
  void printDecl(raw_ostream &OS) const;
};

// Represent the In-Port in Verilog.
class SIRInPort : public SIRPort {
private:
  unsigned BitWidth;
  const std::string Name;

  std::string getNameImpl() const { return Name; }
  unsigned getBitWidthImpl() const { return BitWidth; }
  SIRPort::SIRPortTypes getPortTypeImpl() { return SIRPort::InPort; }

public:
  SIRInPort(unsigned BitWidth, const std::string Name)
    : BitWidth(BitWidth), Name(Name) {}
};

// Represent the Out-Port in Verilog.
class SIROutPort : public SIRPort {
private:
  unsigned BitWidth;
  const std::string Name;
  SIRRegister *Reg;

  std::string getNameImpl() const { return Name; }
  unsigned getBitWidthImpl() const { return BitWidth; }
  SIRPort::SIRPortTypes getPortTypeImpl() { return SIRPort::OutPort; }

public:
  SIROutPort(unsigned BitWidth, const std::string Name) {
    this->Reg = new SIRRegister(BitWidth, Name);
  }
};

// The module in Shang IR.
class SIR {
  typedef ilist<SIRPort> SIRPortVector;
  typedef SIRPortVector::iterator port_iterator;
  typedef SIRPortVector::const_iterator const_port_iterator;

  typedef ilist<SIRRegister> RegisterVector;
  typedef RegisterVector::iterator register_iterator;
  typedef RegisterVector::const_iterator const_register_iterator;


  typedef DenseMap<Instruction *, SIRRegister *> SeqInst2RegMapTy;
  typedef SeqInst2RegMapTy::iterator seqinst2reg_iterator;
  typedef SeqInst2RegMapTy::const_iterator const_seqinst2reg_iterator;

private:
  // Input/Output ports of the module.
  SIRPortVector Ports;

  // Registers in the module.
  RegisterVector Registers;

  // The map between SeqInst and SIRRegister
  SeqInst2RegMapTy SeqInst2Reg;

protected:
  Module *M;

public:
  SIR(Module *M) : M(M) {}
  ~SIR();

  port_iterator ports_begin() { return Ports.begin(); }
  const_port_iterator ports_begin() const { return Ports.begin(); }

  port_iterator ports_end() { return Ports.end(); }
  const_port_iterator ports_end() const { return Ports.end(); }

  register_iterator registers_begin() { return Registers.begin(); }
  register_iterator registers_end() { return Registers.end(); }

  const_register_iterator registers_begin() const { return Registers.begin(); }
  const_register_iterator registers_end() const { return Registers.end(); }

  bool IndexSeqInst2Reg(Instruction *SeqInst, SIRRegister *Reg) {
    return SeqInst2Reg.insert(std::make_pair(SeqInst, Reg)).second;
  }

  SIRRegister *lookupSIRReg(Instruction *SeqInst) {
    const_seqinst2reg_iterator at = SeqInst2Reg.find(SeqInst);
    return at == SeqInst2Reg.end() ? 0 : at->second;
  }


  const std::string getName() const { return M->getModuleIdentifier(); }
  Module *getModule() { return M; }

  // Create register for corresponding SeqInst.
  SIRRegister *getOrCreateRegister(Instruction *SeqInst, StringRef Name = 0,
                                   unsigned BitWidth = 0, uint64_t InitVal = 0);


  // -------------------Functions to generate Verilog-------------------- //
  
  // Print the declaration of module.
  void printModuleDecl(raw_ostream &OS) const;

};

}

#endif