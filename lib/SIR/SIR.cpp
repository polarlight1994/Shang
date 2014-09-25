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


IntegerType *SIR::createIntegerType(unsigned BitWidth) {
  return IntegerType::get(C, BitWidth);
}

Value *SIR::createIntegerValue(unsigned BitWidth, unsigned Val) {
  IntegerType *T = createIntegerType(BitWidth);
  return ConstantInt::get(T, Val);
}

SIRSelector *SIR::createSelector(StringRef Name, unsigned BitWidth) {
  SIRSelector *Sel = new SIRSelector(Name, BitWidth);
  return Sel;
}

SIRPort *SIR::createPort(SIRPort::SIRPortTypes T, StringRef Name,
                         unsigned BitWidth) {
  // InPort or OutPort?
  if (T <= SIRPort::InPort) {
    SIRPort *P = new SIRInPort(T, BitWidth, Name);
    Ports.push_back(P);
    return P;
  } else {
    // Record the Idx of RetPort
    if (T == SIRPort::RetPort) RetPortIdx = Ports.size(); 

    // Create the register for OutPort.
    SIRRegister *Reg = getOrCreateRegister(Name, BitWidth, 0, 0,
                                           SIRRegister::OutPort);
    SIROutPort *P = new SIROutPort(T, Reg, BitWidth, Name);
    // Store the port.
    Ports.push_back(P);
    return P;
  }
}

SIRRegister *SIR::getOrCreateRegister(StringRef Name, unsigned BitWidth,
                                      Instruction *SeqInst, uint64_t InitVal,
                                      SIRRegister::SIRRegisterTypes T) {
  assert(SeqInst || T == SIRRegister::OutPort
         && "Only Reg for Port can have no corresponding SeqInst!");
  
  // If we already create the register, find it.
  if (SeqInst && lookupSIRReg(SeqInst)) return lookupSIRReg(SeqInst);

  // Create the register.
  SIRSelector *Sel = createSelector(Name, BitWidth);
  SIRRegister *Reg = new SIRRegister(Sel, InitVal);

  // Index the register with SeqInst if exists.
  if (SeqInst) IndexSeqInst2Reg(SeqInst, Reg);

  // Store the register.
  Registers.push_back(Reg);

  return Reg;
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



