//===----------------- SIR.cpp - Modules in SIR ----------------*- C++ -*-===//
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
#include "sir/SIR.h"
#include "llvm/IR/Value.h"

using namespace llvm;

void SIRSelector::addAssignment(Value *Fanin, Value *FaninGuard) {
  Fanins.push_back(Fanin);
  FaninGuards.push_back(FaninGuard);
  assert(Fanins.size() == FaninGuards.size() && "Size not compatible!");
}

void SIRSelector::printDecl(raw_ostream &OS) const {
  // Need to Implement these functions.
  OS << "reg" << BitRange(getBitWidth(), 0, false);
  OS << " " << Mangle(getName());
  // Set the IniteVal into 0;
  OS << " = " << buildLiteral(0, getBitWidth(), false)
     << ";\n";
}

SIRRegister::SIRRegister(SIRRegisterTypes T /* = SIRRegister::General */,
                         unsigned BitWidth /* = 0 */, std::string Name /* = "" */,
                         uint64_t InitVal /* = 0 */) : InitVal(InitVal) {
  this->Sel = new SIRSelector(Name, BitWidth);
}

void SIRPort::printDecl(raw_ostream &OS) const {
  if (isInput())
    OS << "input ";
  else
    OS << "output ";

  // If it is a output, then it should be a register.
  if (!isInput())
    OS << "reg";
  else
    OS << "wire";

  if (getBitWidth() > 1) OS << "[" << utostr_32(getBitWidth() - 1) << ":0]";

  OS << " " << Mangle(getName());
}

SIRRegister *SIR::getOrCreateRegister(Instruction *SeqInst /* = 0 */,
                                      SIRRegister::SIRRegisterTypes T /* = SIRRegister::General */,
                                      StringRef Name /* = 0 */, unsigned BitWidth /* = 0 */,
                                      uint64_t InitVal /* = 0 */) {
  assert(SeqInst || T == SIRRegister::OutPort
         && "Only Reg for Port can have no corresponding SeqInst!");
  
  // If we already create the register, find it.
  if (lookupSIRReg(SeqInst)) return lookupSIRReg(SeqInst);

  // Create the register.
  SIRRegister *Reg = new SIRRegister(T, BitWidth, Name, InitVal);
  // Index the register with SeqInst.
  IndexSeqInst2Reg(SeqInst, Reg);

  return Reg;
}

SIRPort *SIR::getOrCreatePort(SIRPort::SIRPortTypes T, StringRef Name,
                              unsigned BitWidth) {
  // InPort or OutPort?
  if (T <= SIRPort::InPort) {
    SIRPort *P = new SIRInPort(T, BitWidth, Name);
    Ports.push_back(P);
    return P;
  } else {
    SIRPort *P = new SIROutPort(T, BitWidth, Name);
    Ports.push_back(P);
    return P;
  }
}

void SIR::printModuleDecl(raw_ostream &OS) const {
  OS << "module " << F->getValueName()->getKey();
  OS << "(\n";
  Ports.front()->printDecl(OS.indent(4));  
  for (SIRPortVector::const_iterator I = Ports.begin() + 1, E = Ports.end();
       I != E; ++I) {
    // Assign the ports to virtual pins.
    OS << ",\n (* altera_attribute = \"-name VIRTUAL_PIN on\" *)";
    (*I)->printDecl(OS.indent(4));
  }
  OS << ");\n";
}



