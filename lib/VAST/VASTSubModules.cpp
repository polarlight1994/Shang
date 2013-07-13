//===---- VASTSubModules.cpp - Submodules in Verilog AST --------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the classes for Submodules in Verilog AST.
//
//===----------------------------------------------------------------------===//
#include "LangSteam.h"

#include "shang/VASTSubModules.h"
#include "shang/VASTModule.h"
#include "shang/FUInfo.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/PathV2.h"
#define DEBUG_TYPE "vast-submodules"
#include "llvm/Support/Debug.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
void VASTSubModuleBase::addFanin(VASTSelector *V) {
  Fanins.push_back(V);
}

void VASTSubModuleBase::addFanout(VASTSelector *V) {
  Fanouts.push_back(V);
}

void VASTSubModuleBase::print(raw_ostream &OS) const {
  vlang_raw_ostream S(dbgs());
  print(S);
}

void VASTSubModuleBase::print(vlang_raw_ostream &OS) const {

}

void VASTSubModuleBase::printDecl(raw_ostream &OS) const {

}

//===----------------------------------------------------------------------===//
std::string
VASTSubModule::getPortName(unsigned FNNum, const Twine &PortName) {
  return "SubMod" + utostr(FNNum) + "_" + PortName.str();
}

VASTSelector *VASTSubModule::createStartPort(VASTModule *VM) {
  VASTRegister *R
    = VM->createRegister(getPortName("start"), 1, 0, VASTSelector::Enable);
  StartPort = R->getSelector();
  VASTSubModuleBase::addFanin(StartPort);
  return StartPort;
}

VASTSelector *VASTSubModule::createFinPort(VASTModule *VM) {
  FinPort = VM->createSelector(getPortName("fin"), 1, this,
                               VASTSelector::FUOutput);
  addFanout(FinPort);
  return FinPort;
}

VASTSelector *VASTSubModule::createRetPort(VASTModule *VM, unsigned Bitwidth,
                                           unsigned Latency) {
  RetPort = VM->createSelector(getPortName("return_value"), Bitwidth, this,
                               VASTSelector::FUOutput);
  addFanout(RetPort);
  // Also update the latency.
  this->Latency = Latency;
  return RetPort;
}

void VASTSubModule::printSimpleInstantiation(vlang_raw_ostream &OS) const {
  //OS << getName() << ' ' << getName() << "_inst" << "(\n";

  //// Print the port connections.
  //for (const_port_iterator I = port_begin(), E = port_end(); I != E; ++I) {
  //  OS.indent(4) << "." << I->first() << '(';
  //  if (const VASTValue *Driver = I->second.getPointer())
  //    Driver->printAsOperand(OS, false);
  //  else
  //    // Simply repeat the port name for the pseudo drivers.
  //    OS << I->first();

  //  OS << "), //" << (I->second.getInt() ? "Input" : "Output") << "\n";
  //}

  //// Write the clock and the reset signal at last.
  //OS.indent(4) << ".clk(clk), .rstN(rstN));\n";
  llvm_unreachable("Not implemented yet!");
}

void VASTSubModule::printSubModuleLogic(vlang_raw_ostream &OS) const {
  // Print the code for simple modules.
  switch (Insts.front()->getOpcode()) {
  case Instruction::UDiv:
    OS << "assign " << getRetPort()->getName() << " = "
       << getFanin(0)->getName() << " / " << getFanin(1)->getName() << ";\n";
    break;
  case Instruction::SDiv:
    OS << "assign " << getRetPort()->getName() << " = $signed("
       << getFanin(0)->getName() << ") / $signed(" << getFanin(1)->getName()
       << ");\n";
    break;
  default:
    llvm_unreachable("Not implemented yet!");
    break;
  }
}

void VASTSubModule::print(vlang_raw_ostream &OS) const {
  if (isa<CallInst>(Insts.front())) {
    printSimpleInstantiation(OS);
    return;
  }

  printSubModuleLogic(OS);
}

void VASTSubModule::printDecl(raw_ostream &OS) const {
  if (VASTSelector *Fin = getFinPort())
    VASTNamedValue::PrintDecl(OS, Fin->getName(), Fin->getBitWidth(), false);
  if (VASTSelector *Ret = getRetPort())
    VASTNamedValue::PrintDecl(OS, Ret->getName(), Ret->getBitWidth(), false);
}
