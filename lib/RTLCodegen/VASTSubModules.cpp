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
#include "vtm/VASTSubModules.h"
#include "vtm//VASTModule.h"

#include "vtm/LangSteam.h"
#include "vtm/FUInfo.h"
#define DEBUG_TYPE "vast-submodules"
#include "llvm/Support/Debug.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
void VASTSubModuleBase::addFanin(VASTSeqValue *V) {
  Fanins.push_back(V);
}

void VASTSubModuleBase::addFanout(VASTValue *V) {
  Fanouts.push_back(V);
}

void VASTSubModuleBase::print(raw_ostream &OS) const {
  vlang_raw_ostream S(dbgs());
  print(S, 0);
}

void VASTSubModuleBase::print(vlang_raw_ostream &OS,
                              const VASTModule *Mod) const {

}

//===----------------------------------------------------------------------===//
VASTRegister::VASTRegister(const char *Name, unsigned BitWidth,
                           uint64_t initVal, VASTNode::SeqValType T,
                           unsigned RegData,  const char *Attr)
  : VASTNode(vastRegister), Value(Name, BitWidth, T, RegData, *this),
    InitVal(initVal), AttrStr(Attr) {}

void VASTRegister::printCondition(raw_ostream &OS, const VASTSlot *Slot,
                                  const AndCndVec &Cnds) {
  OS << '(';
  if (Slot) {
    VASTValPtr Active = Slot->getActive();
    Active.printAsOperand(OS);
    if (VASTWire *S = Active.getAsLValue<VASTWire>()) S->Pin();
  } else      OS << "1'b1";

  typedef AndCndVec::const_iterator and_it;
  for (and_it CI = Cnds.begin(), CE = Cnds.end(); CI != CE; ++CI) {
    OS << " & ";
    CI->printAsOperand(OS);
    if (VASTWire *S = CI->getAsLValue<VASTWire>()) S->Pin();
  }

  OS << ')';
}

void VASTRegister::print(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  if (Value.empty()) return;

  // Print the data selector of the register.
  Value.printSelector(OS);

  OS.always_ff_begin();
  // Reset the register.
  OS << getName()  << " <= "
    << VASTImmediate::buildLiteral(InitVal, getBitWidth(), false) << ";\n";
  OS.else_begin();

  // Print the assignment.
  OS.if_begin(Twine(getName()) + Twine("_selector_enable"));
  OS << getName() << " <= " << getName() << "_selector_wire"
    << VASTValue::printBitRange(getBitWidth(), 0, false) << ";\n";
  OS.exit_block();

  OS << "// synthesis translate_off\n";
  Value.verifyAssignCnd(OS, getName(), Mod);
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end();
}

void VASTRegister::print(raw_ostream &OS) const {
  vlang_raw_ostream S(dbgs());
  print(S, 0);
}

//===----------------------------------------------------------------------===//
void VASTBlockRAM::addPorts(VASTModule *VM) {
  std::string BRamArrayName = VFUBRAM::getArrayName(getBlockRAMNum());
  
  // Add the address port and the data port.
  VASTSeqValue *ReadAddrA = VM->createSeqValue(BRamArrayName + "_rdata0",
                                               getWordSize(), VASTNode::BRAM,
                                               BRamNum, this);
  addFanin(ReadAddrA);
  addFanout(ReadAddrA);

  VASTSeqValue *WriteAddrA = VM->createSeqValue(BRamArrayName + "_waddr0",
                                                getAddrWidth(), VASTNode::BRAM,
                                                BRamNum, this);
  addFanin(WriteAddrA);

  VASTSeqValue *WriteDataA = VM->createSeqValue(BRamArrayName + "_wdata0",
                                                getWordSize(), VASTNode::BRAM,
                                                BRamNum, this);
  addFanin(WriteDataA);
}

void
VASTBlockRAM::print(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  // Print the array and the initializer.
  std::string InitFilePath = "";
  // Set the initialize file's name if there is any.
  if (Initializer)
    InitFilePath = VBEMangle(Initializer->getName()) + "_init.txt";

  // Generate the code for the block RAM.
  OS << "// Address space: " << getBlockRAMNum();
  if (Initializer) OS << *Initializer;
  OS << '\n'
     << "(* ramstyle = \"no_rw_check\" *) reg"
     << VASTValue::printBitRange(getWordSize(), 0, false) << ' '
     << VFUBRAM::getArrayName(getBlockRAMNum()) << "[0:" << getDepth() << "];\n";

  if (Initializer)
    OS << "initial $readmemh(\"" << InitFilePath << "\", "
       << VFUBRAM::getArrayName(getBlockRAMNum()) << ");\n";

  // Print the selectors.
  getRAddr(0)->printSelector(OS, getAddrWidth());
  getWAddr(0)->printSelector(OS);
  getWData(0)->printSelector(OS);

  OS.always_ff_begin(false);
  // Print the first port.
  printPort(OS, 0);


  OS << "// synthesis translate_off\n";
  for (const_fanin_iterator I = fanin_begin(), E = fanin_end(); I != E; ++I) {
    VASTSeqValue *V = *I;
    V->verifyAssignCnd(OS, V->getName(), Mod);
  }
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end(false);
}

void VASTBlockRAM::printPort(vlang_raw_ostream &OS, unsigned Num) const {
  const std::string &BRAMArray = VFUBRAM::getArrayName(getBlockRAMNum());

  // Print the read port.
  VASTSeqValue *RAddr = getRAddr(Num);
  if (!RAddr->empty()) {
    OS.if_begin(Twine(RAddr->getName()) + "_selector_enable");

    OS << RAddr->getName()
       << VASTValue::printBitRange(getWordSize(), 0, false) << " <= "
       << BRAMArray << '[' << RAddr->getName() << "_selector_wire"
       << VASTValue::printBitRange(getAddrWidth() , 0, false) << "];\n";

    OS.exit_block();
  }

  // Print the write port.
  VASTSeqValue *WAddr = getWAddr(Num);
  if (!WAddr->empty()) {
    OS.if_begin(Twine(WAddr->getName()) + "_selector_enable");
    OS << BRAMArray << '[' << WAddr->getName() << "_selector_wire"
      << VASTValue::printBitRange(getAddrWidth(), 0, false) << ']' << " <= "
      << getWData(Num)->getName() << "_selector_wire"
      << VASTValue::printBitRange(getWordSize(), 0, false) << ";\n";
    OS.exit_block();
  }
}
