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
VASTRegister::VASTRegister(VASTSeqValue *V, uint64_t initVal, const char *Attr)
  : VASTNode(vastRegister), Value(V), InitVal(initVal), AttrStr(Attr) {}

void VASTRegister::print(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  if (getValue()->empty()) return;

  // Print the data selector of the register.
  getValue()->printSelector(OS);

  OS.always_ff_begin();
  // Reset the register.
  OS << getName()  << " <= "
    << VASTImmediate::buildLiteral(InitVal, getBitWidth(), false) << ";\n";
  OS.else_begin();

  // Print the assignment.
  if (getValue()->getValType() == VASTSeqValue::Enable)
    OS << getName() << " <= " << getName() << "_selector_enable" << ";\n";
  else {
    OS.if_begin(Twine(getName()) + Twine("_selector_enable"));
    OS << getName() << " <= " << getName() << "_selector_wire"
       << VASTValue::printBitRange(getBitWidth(), 0, false) << ";\n";
    OS.exit_block();
  }

  OS << "// synthesis translate_off\n";
  getValue()->verifyAssignCnd(OS, getName(), Mod);
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
                                               getWordSize(), VASTSeqValue::BRAM,
                                               getBlockRAMNum(), this);
  addFanin(ReadAddrA);
  addFanout(ReadAddrA);

  VASTSeqValue *WriteAddrA = VM->createSeqValue(BRamArrayName + "_waddr0",
                                                getAddrWidth(), VASTSeqValue::BRAM,
                                                getBlockRAMNum(), this);
  addFanin(WriteAddrA);

  VASTSeqValue *WriteDataA = VM->createSeqValue(BRamArrayName + "_wdata0",
                                                getWordSize(), VASTSeqValue::BRAM,
                                                getBlockRAMNum(), this);
  addFanin(WriteDataA);
}

static void printConstant(raw_ostream &OS, uint64_t Val, unsigned SizeInBits) {
  if (SizeInBits == 1)
    OS << (Val ? '1' : '0');
  else {
    std::string FormatS = "%0" + utostr_32(SizeInBits / 8 * 2) + "llx";
    OS << format(FormatS.c_str(), Val);
  }

  OS << '\n';
}


static void WriteBRAMInitializer(raw_ostream &OS, const Constant *C,
                                 unsigned SizeInBits) {
  if (const ConstantInt *CI = dyn_cast<ConstantInt>(C)) {
    printConstant(OS, CI->getZExtValue(), SizeInBits);
    return;
  }

  if (isa<ConstantPointerNull>(C)) {
    printConstant(OS, 0, SizeInBits);
    return;
  }

  if (const ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(C)) {
    for (unsigned i = 0, e = CDS->getNumElements(); i != e; ++i)
      WriteBRAMInitializer(OS, CDS->getElementAsConstant(i), SizeInBits);

    return;
  }

  if (const ConstantArray *CA = dyn_cast<ConstantArray>(C)) {
    for (unsigned i = 0, e = CA->getNumOperands(); i != e; ++i)
      WriteBRAMInitializer(OS, cast<Constant>(CA->getOperand(i)), SizeInBits);

    return;
  }

  llvm_unreachable("Unsupported constant type to bind to script engine!");
  OS << '0';
}

void
VASTBlockRAM::print(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  bool HasInitializer = Initializer != 0;

  // Print the array and the initializer.
  std::string InitFileName = "";
  // Set the initialize file's name if there is any.
  if (HasInitializer)
    InitFileName = ShangMangle(Initializer->getName()) + "_init.txt";

  // Generate the code for the block RAM.
  OS << "// Address space: " << getBlockRAMNum();
  if (Initializer) OS << *Initializer;
  OS << '\n'
     << "(* ramstyle = \"no_rw_check\" *) reg"
     << VASTValue::printBitRange(getWordSize(), 0, false) << ' '
     << VFUBRAM::getArrayName(getBlockRAMNum()) << "[0:" << (getDepth() - 1)
     << "];\n";

  if (HasInitializer) {
    SmallString<1024> FullInitFilePath;
    sys::path::append(FullInitFilePath,
                      getFUDesc<VFUBRAM>()->InitFileDir, InitFileName);
    // Generate the initialize file.
    std::string ErrorInfo;
    const char *CFullInitFilePath = FullInitFilePath.c_str();
    raw_fd_ostream InitFileO(CFullInitFilePath, ErrorInfo);

    if (ErrorInfo.empty()) {
      DEBUG(dbgs() << "writing" << CFullInitFilePath << '\n');
      OS << "initial $readmemh(\"" << CFullInitFilePath << "\", "
         << VFUBRAM::getArrayName(getBlockRAMNum()) << ");\n";

      // Initialize the block RAM with the array or zeros.
      if (Initializer->hasInitializer() &&
          !Initializer->getInitializer()->isNullValue()) {
          WriteBRAMInitializer(InitFileO, Initializer->getInitializer(),
                               getWordSize());
      } else {
        for (unsigned i = 0; i < getDepth(); ++i)
          printConstant(InitFileO, 0, getWordSize());
      }

    } else
      errs() << "error opening file '" << FullInitFilePath.data()
             << "' for writing block RAM initialize file!\n";
  }

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

//===----------------------------------------------------------------------===//
std::string
VASTSubModule::getPortName(unsigned FNNum, const std::string &PortName) {
  return "SubMod" + utostr(FNNum) + "_" + PortName;
}

VASTSeqValue *VASTSubModule::createStartPort(VASTModule *VM) {
  StartPort
    = VM->addRegister(getPortName("start"), 1, 0, VASTSeqValue::Enable)->getValue();
  addInPort("start", StartPort);
  return StartPort;
}

VASTSeqValue *VASTSubModule::createFinPort(VASTModule *VM) {
  FinPort = VM->createSeqValue(getPortName("fin"), 1, VASTSeqValue::IO, 0, this);
  addOutPort("fin", FinPort);
  return FinPort;
}

VASTSeqValue *VASTSubModule::createRetPort(VASTModule *VM, unsigned Bitwidth,
                                           unsigned Latency) {
  RetPort = VM->createSeqValue(getPortName("return_value"),
                               Bitwidth, VASTSeqValue::IO, 0, this);
  addOutPort("return_value", RetPort);
  // Also update the latency.
  this->Latency = Latency;
  return RetPort;
}

void VASTSubModule::addPort(const std::string &Name, VASTValue *V, bool IsInput) {
  VASTSubModulePortPtr Ptr(V, IsInput);
  VASTSubModulePortPtr Inserted = PortMap.GetOrCreateValue(Name, Ptr).second;
  assert(Inserted == Ptr && "Already inserted!");
  (void) Inserted;

  // Do not add the pseudo drivers to the fanin/fanout list.
  if (V == 0) return;

  if (IsInput) addFanin(cast<VASTSeqValue>(V));
  else         addFanout(V);
}

void VASTSubModule::printSimpleInstantiation(vlang_raw_ostream &OS,
                                             const VASTModule *Mod) const {
  OS << getName() << ' ' << getName() << "_inst" << "(\n";

  // Print the port connections.
  for (const_port_iterator I = port_begin(), E = port_end(); I != E; ++I) {
    OS.indent(4) << "." << I->first() << '(';
    if (const VASTValue *Driver = I->second.getPointer())
      Driver->printAsOperand(OS, false);
    else
      // Simply repeat the port name for the pseudo drivers.
      OS << I->first();

    OS << "), //" << (I->second.getInt() ? "Input" : "Output") << "\n";
  }

  // Write the clock and the reset signal at last.
  OS.indent(4) << ".clk(clk), .rstN(rstN));\n";
}

void VASTSubModule::printInstantiationFromTemplate(vlang_raw_ostream &OS,
                                                   const VASTModule *Mod)
                                                   const {
  std::string Ports[5] = {
    "clk", "rstN",
    getPortName("start"),
    getPortName("fin"),
    getPortName("return_value")
  };

  // ask the constraint about how to instantiates this submodule.
  OS << "// External module: " << getName() << '\n';
  OS << VFUs::instantiatesModule(getName(), getNum(), Ports);
}

void VASTSubModule::print(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  if (IsSimple) {
    printSimpleInstantiation(OS, Mod);
    return;
  }

  printInstantiationFromTemplate(OS, Mod);
}
