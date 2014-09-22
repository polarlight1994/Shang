//===------------------- SIR.h - Modules in SIR ----------------*- C++ -*-===//
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
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/ValueMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#ifndef SIR_MODULE_H
#define SIR_MODULE_H

namespace llvm {
  static std::string buildLiteral(uint64_t Value, unsigned bitwidth, bool isMinValue) {
    std::string ret;
    ret = utostr_32(bitwidth) + '\'';
    if (bitwidth == 1) ret += "b";
    else               ret += "h";
    // Mask the value that small than 4 bit to prevent printing something
    // like 1'hf out.
    if (bitwidth < 4) Value &= (1 << bitwidth) - 1;

    if(isMinValue) {
      ret += utohexstr(Value);
      return ret;
    }

    std::string ss = utohexstr(Value);
    unsigned int uselength = (bitwidth/4) + (((bitwidth&0x3) == 0) ? 0 : 1);
    if(uselength < ss.length())
      ss = ss.substr(ss.length() - uselength, uselength);
    ret += ss;

    return ret;
  }

  static std::string getFUName(IntrinsicInst &I) {
    switch (I.getIntrinsicID()) {
    case Intrinsic::shang_add:  return "shang_addc";
    case Intrinsic::shang_mul:  return "shang_mult";
    case Intrinsic::shang_rand: return "shang_rand";
    case Intrinsic::shang_shl:  return "shang_shl";
    case Intrinsic::shang_lshr: return "shang_srl";
    case Intrinsic::shang_ashr: return "shang_ashr";
    case Intrinsic::shang_ugt:  return "shang_ugt";
    case Intrinsic::shang_sgt:  return "shang_sgt";
    default: break;
    }

    return NULL;
  }

  static std::string Mangle(const std::string &S) {
    std::string Result;

    for (unsigned i = 0, e = S.size(); i != e; ++i) {
      if (isalnum(S[i]) || S[i] == '_') {
        Result += S[i];
      } else {
        Result += '_';
      }
    }

    return Result;
  }

  static std::string BitRange(unsigned UB, unsigned LB = 0, bool printOneBit = false) {
    std::string ret;
    assert(UB && UB > LB && "Bad bit range!");
    --UB;
    if (UB != LB)
      ret = "[" + utostr_32(UB) + ":" + utostr_32(LB) + "]";
    else if(printOneBit)
      ret = "[" + utostr_32(LB) + "]";

    return ret;
  }

  static void printName(raw_ostream &OS, Instruction &I) {
    OS << Mangle(I.getName());
  }
}

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
class SIRRegister {
private:
  const uint64_t InitVal;
  SIRSelector *Sel;

public:
  enum SIRRegisterTypes {
    General,            // Common registers which hold data for data-path.
    OutPort,            // Register for OutPort of module.
  };
  SIRRegister(SIRRegisterTypes T = SIRRegister::General, unsigned BitWidth = 0,
              std::string Name = "", uint64_t InitVal = 0);
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
class SIRPort {
public:
  enum SIRPortTypes {
    Clk,
    Rst,
    Start,
    ArgPort,
    InPort = ArgPort,
    RetPort,
    OutPort = RetPort    
  };

private:
  unsigned BitWidth;
  const std::string Name;
  SIRPortTypes T;

public:
  SIRPort(SIRPortTypes T, unsigned BitWidth, const std::string Name)
          : T(T), BitWidth(BitWidth), Name(Name) {}
  ~SIRPort();

  const std::string getName() const { return Name; }
  unsigned getBitWidth() const { return BitWidth; }
  SIRPortTypes getPortType() const { return T; }
  bool isInput() const { return T != RetPort; }

  // Print the port
  void printDecl(raw_ostream &OS) const;
};

// Represent the In-Port in Verilog.
class SIRInPort : public SIRPort {
public:
  SIRInPort(SIRPort::SIRPortTypes T, 
            unsigned BitWidth, const std::string Name)
    : SIRPort(T, BitWidth, Name) {}
};

// Represent the Out-Port in Verilog.
class SIROutPort : public SIRPort {
private:
  SIRRegister *Reg;

public:
  SIROutPort(SIRPort::SIRPortTypes T,
             unsigned BitWidth, const std::string Name)
             : SIRPort(T, BitWidth, Name){
    this->Reg = new SIRRegister(SIRRegister::General, BitWidth, Name);
  }
};

// The module in Shang IR.
class SIR {
  typedef SmallVector<SIRPort *, 8> SIRPortVector;
  typedef SIRPortVector::iterator port_iterator;
  typedef SIRPortVector::const_iterator const_port_iterator;

  typedef SmallVector<SIRPort *, 8> RegisterVector;
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
  Function *F;

public:
  SIR(Function *F) : F(F) {}
  ~SIR();

  Function *getFunction() { return F; }

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

  // -------------------Functions to generate Verilog-------------------- //

  // Create register for corresponding SeqInst.
  SIRRegister *getOrCreateRegister(Instruction *SeqInst = 0,
                                   SIRRegister::SIRRegisterTypes T = SIRRegister::General,
                                   StringRef Name = 0, unsigned BitWidth = 0,
                                   uint64_t InitVal = 0);
  // Create port for interface of module.
  SIRPort *getOrCreatePort(SIRPort::SIRPortTypes T, StringRef Name, unsigned BitWidth);


  // -------------------Functions to generate Verilog-------------------- //
  
  // Print the declaration of module.
  void printModuleDecl(raw_ostream &OS) const;

  void printAsOperandImpl(raw_ostream &OS, Value *U, unsigned UB, unsigned LB) {
    if (ConstantInt *CI = dyn_cast<ConstantInt>(U)) {
      assert(UB == CI->getBitWidth() && LB == 0 && "The slice of constant is not supported yet!");
      OS << "((";
      OS << buildLiteral(CI->getZExtValue(), UB, false);
      OS << "))";
      return;
    }

    // Print the correct name if this value is a SeqValue.
    Instruction *SeqInst = dyn_cast<Instruction>(U);
    if (lookupSIRReg(SeqInst))
      OS << "((" << Mangle(SeqInst->getName());
    else
      OS << "((" << Mangle(SeqInst->getName());

    unsigned OperandWidth = UB - LB;
    if (UB)
      OS << BitRange(UB, LB, OperandWidth > 1);

    // Ignore the mask for now
    OS << "))";

  }

  void printAsOperand(raw_ostream &OS, Value *U, unsigned BitWidth) {
    // Need to find a more proper way to get BitWidth 
    printAsOperandImpl(OS, U, BitWidth, 0);
  }

  void printSimpleOpImpl(raw_ostream &OS, ArrayRef<Value *> Ops,
                         const char *Opc, unsigned BitWidth) {
    unsigned NumOps = Ops.size();
    assert(NumOps && "Unexpected zero operand!");
    printAsOperand(OS, Ops[0], BitWidth);

    for (unsigned i = 1; i < NumOps; ++i) {
      OS << Opc;
      printAsOperand(OS, Ops[i], BitWidth);
    }
  }



};

}

#endif