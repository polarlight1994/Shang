//===---- VASTMemoryPort.cpp - Memory Ports in Verilog AST ------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the classes for memory ports in Verilog AST.
//
//===----------------------------------------------------------------------===//
#include "LangSteam.h"

#include "shang/VASTMemoryPort.h"
#include "shang/VASTModule.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-memory-bus"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumUnusedRead, "Number of unused read ports in memory bus");
STATISTIC(NumUnusedWrite, "Number of unused write ports in memory bus");

VASTMemoryBus::VASTMemoryBus(unsigned BusNum, unsigned AddrSize,
                             unsigned DataSize)
  : VASTSubModuleBase(VASTNode::vastMemoryBus, "", BusNum),
    AddrSize(AddrSize), DataSize(DataSize) {}

void VASTMemoryBus::addPorts(VASTModule *VM) {
  unsigned ByteEnSize = getByteEnWdith();
  // The read ports.
  VASTSeqValue *REn = VM->createSeqValue(getREnName(Idx), 1,
                                         VASTSeqValue::Enable, 0, this);
  addFanin(REn);
  if (isDefault()) VM->createPort(REn, false);

  VASTSeqValue *RBEn = VM->createSeqValue(getRByteEnName(Idx), ByteEnSize,
                                          VASTSeqValue::IO, 0, this);
  addFanin(RBEn);
  if (isDefault()) VM->createPort(RBEn, false);

  VASTSeqValue *RAddr = VM->createSeqValue(getRAddrName(Idx), getAddrWidth(),
                                          VASTSeqValue::IO, 0, this);
  addFanin(RAddr);
  if (isDefault()) VM->createPort(RAddr, false);

  VASTSeqValue *RData = VM->createSeqValue(getRDataName(Idx), getDataWidth(),
                                          VASTSeqValue::IO, 0, this);
  addFanout(RData);
  if (isDefault()) VM->createPort(RData, true);

  // The write ports.
  VASTSeqValue *WEn = VM->createSeqValue(getWEnName(Idx), 1,
                                         VASTSeqValue::Enable, 0, this);
  addFanin(WEn);
  if (isDefault()) VM->createPort(WEn, false);

  VASTSeqValue *WBEn = VM->createSeqValue(getWByteEnName(Idx), ByteEnSize,
                                          VASTSeqValue::IO, 0, this);
  addFanin(WBEn);
  if (isDefault()) VM->createPort(WBEn, false);

  VASTSeqValue *WAddr = VM->createSeqValue(getWAddrName(Idx), getAddrWidth(),
                                           VASTSeqValue::IO, 0, this);
  addFanin(WAddr);
  if (isDefault()) VM->createPort(WAddr, false);

  VASTSeqValue *WData = VM->createSeqValue(getWDataName(Idx), getDataWidth(),
                                           VASTSeqValue::IO, 0, this);
  addFanin(WData);
  if (isDefault()) VM->createPort(WData, false);
}

// The read port of the memory bus.
VASTSeqValue *VASTMemoryBus::getREnable() const {
  return getFanin(0);
}

VASTSeqValue *VASTMemoryBus::getRByteEn() const {
  return getFanin(1);
}

VASTSeqValue *VASTMemoryBus::getRAddr() const {
  return getFanin(2);
}

VASTValue    *VASTMemoryBus::getRData() const {
  return getFanout(0);
}

// The write port of the memory bus.
VASTSeqValue *VASTMemoryBus::getWEnable() const {
  return getFanin(3);
}

VASTSeqValue *VASTMemoryBus::getWByteEn() const {
  return getFanin(4);
}

VASTSeqValue *VASTMemoryBus::getWAddr() const {
  return getFanin(5);
}

VASTSeqValue *VASTMemoryBus::getWData() const {
  return getFanin(6);
}

std::string VASTMemoryBus::getRAddrName(unsigned Idx) {
  return "mem" + utostr(Idx) + "raddr";
}

std::string VASTMemoryBus::getWAddrName(unsigned Idx) {
  return "mem" + utostr(Idx) + "waddr";
}

std::string VASTMemoryBus::getRDataName(unsigned Idx) {
  return "mem" + utostr(Idx) + "rdata";
}

std::string VASTMemoryBus::getWDataName(unsigned Idx) {
  return "mem" + utostr(Idx) + "wdata";
}

std::string VASTMemoryBus::getWByteEnName(unsigned Idx) {
  return "mem" + utostr(Idx) + "wbe";
}

std::string VASTMemoryBus::getRByteEnName(unsigned Idx) {
  return "mem" + utostr(Idx) + "rbe";
}

std::string VASTMemoryBus::getWEnName(unsigned Idx) {
  return "mem" + utostr(Idx) + "wen";
}

std::string VASTMemoryBus::getREnName(unsigned Idx) {
  return "mem" + utostr(Idx) + "ren";
}

static void printAssigment(vlang_raw_ostream &OS, VASTSeqValue *SeqVal,
                           const VASTModule *Mod) {
  OS << SeqVal->getName() << " <= ";
  if (SeqVal->empty()) {
    OS << VASTImmediate::buildLiteral(0, SeqVal->getBitWidth(), false) << ";\n";
    return;
  }

  OS << SeqVal->getName() << "_selector_wire"
     << VASTValue::printBitRange(SeqVal->getBitWidth(), 0, false) << ";\n";
}

void VASTMemoryBus::print(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  // Print the read port.
  VASTSeqValue *ReadEnable = getREnable();
  if (!ReadEnable->empty()) {
    ReadEnable->printSelector(OS);
    getRAddr()->printSelector(OS, false);
    getRByteEn()->printSelector(OS, false);
  } else
    ++NumUnusedRead;

  OS.always_ff_begin(false);
  OS << ReadEnable->getName() <<  " <= ";
  if (ReadEnable->empty())
    OS << "1'b0;\n";
  else {
    OS << ReadEnable->getName() << "_selector_enable" << ";\n";
    OS.if_begin(Twine(ReadEnable->getName()) + "_selector_enable");
  }

  printAssigment(OS, getRAddr(), Mod);
  printAssigment(OS, getRByteEn(), Mod);
  if (!ReadEnable->empty()) OS.exit_block();

  OS << "// synthesis translate_off\n";
  ReadEnable->verifyAssignCnd(OS, "memory_bus_read_" + utostr(Idx) , Mod);
  getRAddr()->verifyAssignCnd(OS, "memory_bus_read_" + utostr(Idx) , Mod);
  getRByteEn()->verifyAssignCnd(OS, "memory_bus_read_" + utostr(Idx) , Mod);
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end(false);

  // Print the write port.
  VASTSeqValue *WriteEnable = getWEnable();
  if (!WriteEnable->empty()) {
    WriteEnable->printSelector(OS);
    getWAddr()->printSelector(OS, false);
    getWData()->printSelector(OS, false);
    getWByteEn()->printSelector(OS, false);
  } else
    ++NumUnusedWrite;

  OS.always_ff_begin(false);

  OS << WriteEnable->getName() <<  " <= ";
  if (WriteEnable->empty())
    OS << "1'b0;\n";
  else {
    OS << WriteEnable->getName() << "_selector_enable" << ";\n";
    OS.if_begin(Twine(WriteEnable->getName()) + "_selector_enable");
  }

  printAssigment(OS, getWAddr(), Mod);
  printAssigment(OS, getWData(), Mod);
  printAssigment(OS, getWByteEn(), Mod);
  if (!WriteEnable->empty()) OS.exit_block();

  OS << "// synthesis translate_off\n";
  WriteEnable->verifyAssignCnd(OS, "memory_bus_write_" + utostr(Idx) , Mod);
  getWAddr()->verifyAssignCnd(OS, "memory_bus_write_" + utostr(Idx) , Mod);
  getWData()->verifyAssignCnd(OS, "memory_bus_write_" + utostr(Idx) , Mod);
  getWByteEn()->verifyAssignCnd(OS, "memory_bus_write_" + utostr(Idx) , Mod);
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end(false);
}
