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

#include "llvm/IR/GlobalVariable.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/PathV2.h"
#define DEBUG_TYPE "vast-memory-bus"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumUnusedRead, "Number of unused read ports in memory bus");
STATISTIC(NumUnusedWrite, "Number of unused write ports in memory bus");

VASTMemoryBus::VASTMemoryBus(unsigned BusNum, unsigned AddrSize,
                             unsigned DataSize, bool RequireByteEnable)
  : VASTSubModuleBase(VASTNode::vastMemoryBus, "", BusNum),
    AddrSize(AddrSize), DataSize(DataSize),
    RequireByteEnable(RequireByteEnable), CurrentOffset(0) {}

void VASTMemoryBus::addPorts(VASTModule *VM) {
  unsigned ByteEnSize = getByteEnWdith();
  VASTNode *Parent = isDefault() ? 0 : this;

  // The read ports.
  VASTSelector *REn = VM->createSelector(getREnName(Idx), 1, Parent,
                                          VASTSelector::Enable);
  addFanin(REn);
  if (isDefault()) VM->addPort(REn);

  VASTSelector *RAddr
    = VM->createSelector(getRAddrName(Idx), getAddrWidth(), Parent);
  addFanin(RAddr);
  if (isDefault()) VM->addPort(RAddr);

  if (isDefault()) {
    VASTInPort *RData = VM->addInputPort(getRDataName(Idx), getDataWidth());
    addFanout(RData->getValue());
  } else
    addFanout(VM->addWire(getRDataName(Idx), getDataWidth(), this));

  // The write ports.
  VASTSelector *WEn = VM->createSelector(getWEnName(Idx), 1, Parent,
                                         VASTSelector::Enable);
  addFanin(WEn);
  if (isDefault()) VM->addPort(WEn);


  VASTSelector *WAddr
    = VM->createSelector(getWAddrName(Idx), getAddrWidth(), Parent);
  addFanin(WAddr);
  if (isDefault()) VM->addPort(WAddr);

  VASTSelector *WData
    = VM->createSelector(getWDataName(Idx), getDataWidth(), Parent);
  addFanin(WData);
  if (isDefault()) VM->addPort(WData);

  if (requireByteEnable()) {
    VASTSelector *RBEn
      = VM->createSelector(getRByteEnName(Idx), ByteEnSize, Parent);
    addFanin(RBEn);
    if (isDefault()) VM->addPort(RBEn);

    VASTSelector *WBEn
      = VM->createSelector(getWByteEnName(Idx), ByteEnSize, Parent);
    addFanin(WBEn);
    if (isDefault()) VM->addPort(WBEn);
  }
}

void VASTMemoryBus::addGlobalVariable(GlobalVariable *GV, unsigned SizeInBytes) {
  DEBUG(dbgs() << GV->getName() << " CurOffset: " << CurrentOffset << "\n");
  // Insert the GlobalVariable to the offset map, and calculate its offset.
  // Please note that the computation is in the byte address.
  assert(GV->getAlignment() >= (DataSize / 8) && "Bad alignment!");
  assert(CurrentOffset % (DataSize / 8) == 0 && "Bad CurrentOffset!");
  CurrentOffset = RoundUpToAlignment(CurrentOffset, GV->getAlignment());
  DEBUG(dbgs() << "Roundup to " << CurrentOffset << " according to alignment "
         << GV->getAlignment() << '\n');
  bool inserted = BaseAddrs.insert(std::make_pair(GV, CurrentOffset)).second;
  assert(inserted && "GV had already added!");
  (void) inserted;
  DEBUG(dbgs() << "Size of GV " << SizeInBytes << " Offset increase to "
         << (CurrentOffset + SizeInBytes) << "\n");
  CurrentOffset = RoundUpToAlignment(CurrentOffset + SizeInBytes, DataSize / 8);
  DEBUG(dbgs() << "Roundup to Word address " << CurrentOffset << "\n");
}

unsigned VASTMemoryBus::getStartOffset(GlobalVariable *GV) const {
  std::map<GlobalVariable*, unsigned>::const_iterator at = BaseAddrs.find(GV);
  assert(at != BaseAddrs.end() && "GV is not assigned to this memory bank?");
  return at->second;
}

// The read port of the memory bus.
VASTSelector *VASTMemoryBus::getREnable() const {
  return getFanin(0);
}

VASTSelector *VASTMemoryBus::getRAddr() const {
  return getFanin(1);
}

VASTValue    *VASTMemoryBus::getRData() const {
  return getFanout(0);
}

// The write port of the memory bus.
VASTSelector *VASTMemoryBus::getWEnable() const {
  return getFanin(2);
}

VASTSelector *VASTMemoryBus::getWAddr() const {
  return getFanin(3);
}

VASTSelector *VASTMemoryBus::getWData() const {
  return getFanin(4);
}

VASTSelector *VASTMemoryBus::getRByteEn() const {
  return getFanin(5);
}

VASTSelector *VASTMemoryBus::getWByteEn() const {
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

std::string VASTMemoryBus::getArrayName() const {
  return "mem" + utostr(Idx) + "ram";
}

void VASTMemoryBus::printDecl(raw_ostream &OS) const {
  if (isDefault()) return;

  if (requireByteEnable()) {
    getREnable()->printDecl(OS);
    getRAddr()->printDecl(OS);
    getRByteEn()->printDecl(OS);

    getWEnable()->printDecl(OS);
    getWByteEn()->printDecl(OS);
    getWAddr()->printDecl(OS);
    getWData()->printDecl(OS);
  }
}

static void printAssigment(vlang_raw_ostream &OS, VASTSelector *Selector,
                           const Twine &Enable, const VASTModule *Mod) {
  if (!Selector->empty())
    OS.if_begin(Enable + "_selector_enable");

  OS << Selector->getName() << " <= ";
  if (Selector->empty())
    OS << VASTImmediate::buildLiteral(0, Selector->getBitWidth(), false) << ";\n";
  else
    OS << Selector->getName() << "_selector_wire"
       << VASTValue::printBitRange(Selector->getBitWidth(), 0, false) << ";\n";

  if (!Selector->empty()) OS.exit_block();
}

void VASTMemoryBus::printBank(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  // The default memory bus are printed as module ports.
  if (isDefault()) return;

  // Print the read port.
  VASTSelector *ReadEnable = getREnable();
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
  else
    OS << ReadEnable->getName() << "_selector_enable" << ";\n";

  printAssigment(OS, getRAddr(), ReadEnable->getName(), Mod);
  printAssigment(OS, getRByteEn(), ReadEnable->getName(), Mod);

  OS << "// synthesis translate_off\n";
  ReadEnable->verifyAssignCnd(OS, Mod);
  getRAddr()->verifyAssignCnd(OS, Mod);
  getRByteEn()->verifyAssignCnd(OS, Mod);
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end(false);

  // Print the write port.
  VASTSelector *WriteEnable = getWEnable();
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
  else
    OS << WriteEnable->getName() << "_selector_enable" << ";\n";

  printAssigment(OS, getWAddr(), WriteEnable->getName(), Mod);
  printAssigment(OS, getWData(), WriteEnable->getName(), Mod);
  printAssigment(OS, getWByteEn(), WriteEnable->getName(), Mod);

  OS << "// synthesis translate_off\n";
  WriteEnable->verifyAssignCnd(OS, Mod);
  getWAddr()->verifyAssignCnd(OS, Mod);
  getWData()->verifyAssignCnd(OS, Mod);
  getWByteEn()->verifyAssignCnd(OS, Mod);
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end(false);

  // Print the implementation of the block RAM.
  if (isDefault()) return;

  // The width of the byte address in a word.
  unsigned BytesPerWord = getDataWidth() / 8;
  unsigned ByteAddrWidth = Log2_32_Ceil(BytesPerWord);
  assert(ByteAddrWidth && "Should print as block RAM!");

  OS << "reg " << VASTValue::printBitRange(getAddrWidth())
     << " mem" << Idx << "interanal_raddr1r;\n";
  OS << "reg "<< VASTValue::printBitRange(getDataWidth())
     << " mem" << Idx << "interanal_rdata1r;\n";

  // Shift the byte enable according to the byte address in a word.
  OS << "wire " << VASTValue::printBitRange(getByteEnWdith())
     << " mem" << Idx << "interanal_wbe0w = "
        "mem" << Idx << "wbe <<"
        " mem" << Idx << "waddr" << VASTValue::printBitRange(ByteAddrWidth)
     << ";\n";
  // Shift the data according to the byte address also.
  OS << "wire "<< VASTValue::printBitRange(getDataWidth())
     << " mem" << Idx << "interanal_wdata0w = "
        "(mem" << Idx << "wdata"
        " << { mem" << Idx << "waddr" << VASTValue::printBitRange(ByteAddrWidth)
     << ", 3'b0 });\n";

  // Stage 2: Access the block ram.
  assert(CurrentOffset % BytesPerWord == 0 && "CurrentOffset not aligned!");
  unsigned NumWords = (CurrentOffset / BytesPerWord);
  // use a multi-dimensional packed array to model individual bytes within the
  // word. Please note that the bytes is ordered from 0 to 7 ([0:7]) because
  // so that the byte address can access the correct byte.
  OS << "(* ramstyle = \"no_rw_check\", max_depth = " << NumWords << " *) logic"
     << VASTValue::printBitRange(BytesPerWord) << VASTValue::printBitRange(8)
     << ' ' << getArrayName() << "[0:" << NumWords << "-1];\n";

  writeInitializeFile(OS);

  OS.always_ff_begin(false);
  OS.if_() << "mem" << Idx << "wen";
  OS._then();

  for (unsigned i = 0; i < BytesPerWord; ++i)
    OS << "if(mem" << Idx << "interanal_wbe0w[" << i << "]) "
       << getArrayName() << "[mem" << Idx << "waddr"
       << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true) << "]"
          "[" << i << "]"
          " <= mem" << Idx << "interanal_wdata0w"
       << VASTValue::printBitRange((i + 1) * 8, i * 8) << ";\n";

  OS << "if (mem" << Idx << "waddr"
     << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true) << ">= "
     << NumWords << ")  $finish(\"Write access out of bound!\");\n";
  OS.exit_block();

  // TODO: Guard the read pipeline stages by stage enable signal.
  OS << "mem" << Idx << "interanal_rdata1r <= " << getArrayName() << "[mem" << Idx << "raddr"
     << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true) << "];\n";
  OS << "mem" << Idx << "interanal_raddr1r <= mem" << Idx << "raddr;\n";

  OS.if_() << "mem" << Idx << "ren";
  OS._then();
  OS << "if (mem" << Idx << "raddr"
     << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true)
     << ">= "<< NumWords <<") $finish(\"Read access out of bound!\");\n";
  OS.exit_block();
  OS.always_ff_end(false);

  // Shift the result to the Lower part according to the byte address.
  OS << "assign mem" << Idx << "rdata "
        "= mem" << Idx << "interanal_rdata1r"
        " >> { mem" << Idx << "interanal_raddr1r" << VASTValue::printBitRange(ByteAddrWidth)
     << ", 3'b0};\n";
}

static inline int base_addr_less(const void *P1, const void *P2) {
  #define T(x) reinterpret_cast<const std::pair<GlobalVariable*, unsigned>*>(x)

  return T(P2)->second - T(P1)->second;
}

typedef SmallVector<uint8_t, 1024> ByteBuffer;

static unsigned FillByteBuffer(ByteBuffer &Buf, uint64_t Val, unsigned SizeInBytes) {
  SizeInBytes = std::max(1u, SizeInBytes);
  for (unsigned i = 0; i < SizeInBytes; ++i) {
    Buf.push_back(Val & 0xff);
    Val >>= 8;
  }

  return SizeInBytes;
}

static void FillByteBuffer(ByteBuffer &Buf, const Constant *C) {
  if (const ConstantInt *CI = dyn_cast<ConstantInt>(C)) {
    FillByteBuffer(Buf, CI->getZExtValue(), CI->getBitWidth() / 8);
    return;
  }

  if (isa<ConstantPointerNull>(C)) {
    unsigned PtrSizeInBytes = getFUDesc<VFUMemBus>()->getAddrWidth() / 8;
    FillByteBuffer(Buf, 0, PtrSizeInBytes);
    return;
  }

  if (const ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(C)) {
    for (unsigned i = 0, e = CDS->getNumElements(); i != e; ++i)
      FillByteBuffer(Buf, CDS->getElementAsConstant(i));

    return;
  }

  if (const ConstantArray *CA = dyn_cast<ConstantArray>(C)) {
    for (unsigned i = 0, e = CA->getNumOperands(); i != e; ++i)
      FillByteBuffer(Buf, cast<Constant>(CA->getOperand(i)));

    return;
  }

  llvm_unreachable("Unsupported constant type to bind to script engine!");
}

static unsigned padZeroToByteAddr(raw_ostream &OS, unsigned CurByteAddr,
                                  unsigned TargetByteAddr,
                                  unsigned WordSizeInBytes) {
  assert(TargetByteAddr % WordSizeInBytes == 0 && "Bad target byte address!");
  assert(CurByteAddr <= TargetByteAddr && "Bad current byte address!");
  while (CurByteAddr != TargetByteAddr) {
    OS << "00";
    ++CurByteAddr;
    if (CurByteAddr % WordSizeInBytes == 0)
      OS << "// " << CurByteAddr << '\n';
  }

  return CurByteAddr;
}

void VASTMemoryBus::writeInitializeFile(vlang_raw_ostream &OS) const {
  SmallVector<std::pair<GlobalVariable*, unsigned>, 8> Vars;

  typedef std::map<GlobalVariable*, unsigned>::const_iterator iterator;
  for (iterator I = BaseAddrs.begin(), E = BaseAddrs.end(); I != E; ++I)
    Vars.push_back(*I);

  array_pod_sort(Vars.begin(), Vars.end(), base_addr_less);

  std::string InitFileName = "mem" + utostr_32(Idx) + "ram_init.txt";

  // Dirty Hack: Use the same initialize directory with the bram.
  SmallString<1024> FullInitFilePath;
  sys::path::append(FullInitFilePath,
                    getFUDesc<VFUBRAM>()->InitFileDir, InitFileName);

  std::string ErrorInfo;
  const char *CFullInitFilePath = FullInitFilePath.c_str();
  raw_fd_ostream InitFileO(CFullInitFilePath, ErrorInfo);

  if (ErrorInfo.empty()) {
    DEBUG(dbgs() << "writing" << CFullInitFilePath << '\n');

    OS << "initial  $readmemh(\"";
    OS.write_escaped(FullInitFilePath);
    OS << "\", " << getArrayName() << ");\n";
  } else {
    errs() << "error opening file '" << FullInitFilePath.data()
           << "' for writing block RAM initialize file!\n";
    return;
  }

  unsigned CurByteAddr = 0;
  unsigned WordSizeInByte = getDataWidth() / 8;
  ByteBuffer Buffer;

  while (!Vars.empty()) {
    std::pair<GlobalVariable*, unsigned> Var = Vars.pop_back_val();

    GlobalVariable *GV = Var.first;
    DEBUG(dbgs() << GV->getName() << " CurByteAddress " << CurByteAddr << '\n');
    unsigned StartOffset = Var.second;
    CurByteAddr = padZeroToByteAddr(InitFileO, CurByteAddr, StartOffset,
                                    WordSizeInByte);
    DEBUG(dbgs() << "Pad zero to " << StartOffset << '\n');
    InitFileO << "//" << GV->getName() << " start byte address "
              << StartOffset << '\n';
    if (GV->hasInitializer() && !GV->getInitializer()->isNullValue()) {
      FillByteBuffer(Buffer, GV->getInitializer());
      unsigned BytesToPad = OffsetToAlignment(Buffer.size(), WordSizeInByte);
      for (unsigned i = 0; i < BytesToPad; ++i)
        Buffer.push_back(0);

      // Directly write out the buffer in little endian!
      assert(Buffer.size() % WordSizeInByte == 0 && "Buffer does not padded!");
      for (unsigned i = 0, e = (Buffer.size() / WordSizeInByte); i != e; ++i) {
        for (unsigned j = 0; j < WordSizeInByte; ++j) {
          unsigned Idx = i * WordSizeInByte + (WordSizeInByte - j - 1);
          InitFileO << format("%02x", Buffer[Idx]);
          ++CurByteAddr;
        }
        InitFileO << "// " << CurByteAddr << '\n';
      }

      assert((CurByteAddr % WordSizeInByte) == 0 && "Bad ByteBuffer size!");
      DEBUG(dbgs() << "Write initializer: " << CurByteAddr << '\n');
      Buffer.clear();
    }
  }

  padZeroToByteAddr(InitFileO, CurByteAddr, CurrentOffset, WordSizeInByte);
}

void VASTMemoryBus::printBlockRAM(vlang_raw_ostream &OS,
                                  const VASTModule *Mod) const {
  unsigned BytesPerWord = getDataWidth() / 8;
  unsigned ByteAddrWidth = Log2_32_Ceil(BytesPerWord);
  assert((CurrentOffset * 8) % getDataWidth() == 0
         && "CurrentOffset not aligned!");
  unsigned NumWords = (CurrentOffset * 8 / getDataWidth());
  // use a multi-dimensional packed array to model individual bytes within the
  // word. Please note that the bytes is ordered from 0 to 7 ([0:7]) because
  // so that the byte address can access the correct byte.
  OS << "(* ramstyle = \"no_rw_check\", max_depth = " << NumWords << " *) logic"
     << VASTValue::printBitRange(getDataWidth())
     //<< VASTValue::printBitRange(BytesPerWord) << VASTValue::printBitRange(8)
     << ' ' << getArrayName() << "[0:" << NumWords << "-1];\n";

  writeInitializeFile(OS);

  // Declare the internal register.
  OS << "reg " << VASTValue::printBitRange(getDataWidth(), 0)
     << ' ' << getArrayName() << "_rdata0r;\n";

  // Print the selectors.
  getRAddr()->printSelector(OS);
  getWAddr()->printSelector(OS);
  getWData()->printSelector(OS, false);

  OS.always_ff_begin(false);

  // Print the read port.
  VASTSelector *RAddr = getRAddr();
  if (!RAddr->empty()) {
    OS.if_begin(Twine(RAddr->getName()) + "_selector_enable");

    OS << ' ' << getArrayName() << "_rdata0r"
       << VASTValue::printBitRange(getDataWidth(), 0, false) << " <= "
       << ' ' << getArrayName() << "[" << RAddr->getName() << "_selector_wire"
       << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true) << "];\n";

    OS.exit_block();
  }

  // Print the write port.
  VASTSelector *WAddr = getWAddr();
  if (!WAddr->empty()) {
    OS.if_begin(Twine(WAddr->getName()) + "_selector_enable");
    OS << ' ' << getArrayName() << "[" << WAddr->getName() << "_selector_wire"
      << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true) << ']'
      << " <= " << getWData()->getName() << "_selector_wire"
      << VASTValue::printBitRange(getDataWidth(), 0, false) << ";\n";
    OS.exit_block();
  }

  OS << "// synthesis translate_off\n";
  for (const_fanin_iterator I = fanin_begin(), E = fanin_end(); I != E; ++I) {
    VASTSelector *V = *I;
    V->verifyAssignCnd(OS, Mod);
  }

  // Verify the addresses.
  if (!RAddr->empty()) {
    OS << "if (" << RAddr->getName() << "_selector_wire"
       << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true)
       << ">= "<< NumWords <<") $finish(\"Read access out of bound!\");\n";

    if (ByteAddrWidth) {
      OS << "if (" << RAddr->getName() << "_selector_wire"
         << VASTValue::printBitRange(ByteAddrWidth, 0) << " != " << ByteAddrWidth
         << "'b0) $finish(\"Read access out of bound!\");\n";
    }
  }

  if (!WAddr->empty()) {
    OS << "if (" << WAddr->getName() << "_selector_wire"
        << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true)
        << ">= "<< NumWords <<") $finish(\"Read access out of bound!\");\n";
    if (ByteAddrWidth)
      OS << "if (" << WAddr->getName() << "_selector_wire"
         << VASTValue::printBitRange(ByteAddrWidth, 0) << " != " << ByteAddrWidth
         << "'b0) $finish(\"Write access out of bound!\");\n";
  }

  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end(false);

  // Only print the assignment to the read data if there is any use.
  if (!getRData()->use_empty())
    OS << "assign " << cast<VASTWire>(getRData())->getName() << " = "
       << ' ' << getArrayName() << "_rdata0r;\n\n";
}

void VASTMemoryBus::print(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  if (requireByteEnable()) printBank(OS, Mod);
  else                     printBlockRAM(OS, Mod);
}
