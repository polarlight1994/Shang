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
STATISTIC(NumUnusedPorts, "Number of unused ports in memory bus");

VASTMemoryBus::VASTMemoryBus(unsigned BusNum, unsigned AddrSize,
                             unsigned DataSize, bool RequireByteEnable,
                             bool IsDualPort)
  : VASTSubModuleBase(VASTNode::vastMemoryBus, "", BusNum),
    AddrSize(AddrSize), DataSize(DataSize),
    RequireByteEnable(RequireByteEnable), IsDualPort(IsDualPort),
    CurrentOffset(0) {}

void VASTMemoryBus::addBasicPins(VASTModule *VM, unsigned PortNum) {
  assert(!isDefault() && "Just handle internal memory here");

  // Address pin
  VASTSelector *Address
    = VM->createSelector(getAddrName(PortNum), getAddrWidth(), this);
  addFanin(Address);

  // Read (from memory) data pin
  VASTSelector *RData = VM->createSelector(getRDataName(PortNum), getDataWidth(),
                                           this, VASTSelector::FUOutput);
  addFanout(RData);

  // Write (to memory) data pin
  VASTSelector *WData
    = VM->createSelector(getWDataName(PortNum), getDataWidth(), this);
  addFanin(WData);  
}

void VASTMemoryBus::addExternalPins(VASTModule *VM) {
    assert(isDefault() && "Just handle external memory here");

    // Address pin
    VASTSelector *Address
      = VM->createSelector(getAddrName(0), getAddrWidth(), 0);
    addFanin(Address);
    VM->addPort(Address, false);

    // Read (from memory) data pin
    VASTSelector *RData = VM->createSelector(getRDataName(0), getDataWidth(),
      0, VASTSelector::FUOutput);
    addFanout(RData);
    VM->addPort(RData, true);

    // Write (to memory) data pin
    VASTSelector *WData
      = VM->createSelector(getWDataName(0), getDataWidth(), 0);
    addFanin(WData);
    VM->addPort(WData, false);

    // Byte enable pin
    assert(requireByteEnable() && "External always require byte enable pin");
    addByteEnables(VM, 0, 0);

    // Enable pin
    VASTSelector *Enable
      = VM->createSelector(getEnableName(0), 1, 0);
    addFanin(Enable);
    VM->addPort(Enable, false);

    // Write enable pin
    VASTSelector *WriteEn
      = VM->createSelector(getWriteEnName(0), 1, 0);
    addFanin(WriteEn);
    VM->addPort(WriteEn, false);
}

void VASTMemoryBus::addByteEnables(VASTModule *VM, VASTNode *Parent,
                                  unsigned PortNum) {
  VASTSelector *ByteEnable
    = VM->createSelector(getByteEnName(PortNum), getByteEnWidth(), Parent);
  addFanin(ByteEnable);
  if (isDefault()) VM->addPort(ByteEnable, false);
}

void VASTMemoryBus::addPorts(VASTModule *VM) {
  if (isDefault()) {
    addExternalPins(VM);
    return;
  }
  addBasicPins(VM, 0);
  if (isDualPort()) addBasicPins(VM, 1);

  if (requireByteEnable()) {
    addByteEnables(VM, this, 0);
    if (isDualPort()) addByteEnables(VM, this, 1);
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

VASTSelector *VASTMemoryBus::getAddr(unsigned PortNum) const {
  return getFanin(InputsPerPort * PortNum + 0);
}

VASTSelector *VASTMemoryBus::getWData(unsigned PortNum) const {
  return getFanin(InputsPerPort * PortNum + 1);
}

VASTSelector *VASTMemoryBus::getRData(unsigned PortNum) const {
  return getFanout(PortNum);
}

VASTSelector *VASTMemoryBus::getByteEn(unsigned PortNum) const {
  unsigned Offset = InputsPerPort;
  if (isDualPort()) Offset += InputsPerPort;

  return getFanin(Offset + PortNum);
}

std::string VASTMemoryBus::getAddrName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "addr";

  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "addr";
}

std::string VASTMemoryBus::getRDataName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "rdata";
  
  return getArrayName() + "p" + utostr(PortNum) + "rdata";
}

std::string VASTMemoryBus::getWDataName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "wdata";

  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "wdata";
}

std::string VASTMemoryBus::getByteEnName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "be";

  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "be";
}

std::string VASTMemoryBus::getEnableName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "en";

  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "en";
}

std::string VASTMemoryBus::getWriteEnName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "wen";

  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "wen";
}

std::string VASTMemoryBus::getLastStageAddrName(unsigned PortNum) const {
  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "pipe1_addr";
}

std::string VASTMemoryBus::getInternalWEnName(unsigned PortNum) const {
  return getWDataName(PortNum) + "_pipe1_enable";
}

std::string VASTMemoryBus::getArrayName() const {
  return "mem" + utostr(Idx) + "ram";
}

void VASTMemoryBus::printPortDecl(raw_ostream &OS, unsigned PortNum) const {
  getRData(PortNum)->printDecl(OS);

  if (requireByteEnable()) {
    getAddr(PortNum)->printDecl(OS);
    getByteEn(PortNum)->printDecl(OS);
    getWData(PortNum)->printDecl(OS);
    // Also need to declare the register at last stage.
    unsigned BytesPerWord = getDataWidth() / 8;
    unsigned ByteAddrWidth = Log2_32_Ceil(BytesPerWord);
    VASTNamedValue::PrintDecl(OS, getLastStageAddrName(PortNum),
                              ByteAddrWidth, true);
  }
}

void VASTMemoryBus::printDecl(raw_ostream &OS) const {
  if (isDefault()) return;

  printPortDecl(OS, 0);
  if (isDualPort()) printPortDecl(OS, 1);
}

void VASTMemoryBus::printBank(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  // The default memory bus are printed as module ports.
  if (isDefault()) return;

  // The width of the byte address in a word.
  unsigned BytesPerWord = getDataWidth() / 8;
  unsigned ByteAddrWidth = Log2_32_Ceil(BytesPerWord);
  assert(ByteAddrWidth && "Should print as block RAM!");

  assert(CurrentOffset % BytesPerWord == 0 && "CurrentOffset not aligned!");
  unsigned NumWords = (CurrentOffset / BytesPerWord);
  // use a multi-dimensional packed array to model individual bytes within the
  // word. Please note that the bytes is ordered from 0 to 7 ([0:7]) because
  // so that the byte address can access the correct byte.
  OS << "(* ramstyle = \"no_rw_check\", max_depth = " << NumWords << " *) logic"
    << VASTValue::printBitRange(BytesPerWord) << VASTValue::printBitRange(8)
    << ' ' << getArrayName() << "[0:" << NumWords << "-1];\n";

  writeInitializeFile(OS);

  printBanksPort(OS, Mod, 0, BytesPerWord, ByteAddrWidth, NumWords);
  if (isDualPort())
    printBanksPort(OS, Mod, 1, BytesPerWord, ByteAddrWidth, NumWords);
}

void
VASTMemoryBus::printBanksPort(vlang_raw_ostream &OS, const VASTModule *Mod,
                              unsigned PortNum, unsigned BytesPerWord,
                              unsigned ByteAddrWidth, unsigned NumWords) const {
  // Print the read port.
  VASTSelector *Addr = getAddr(PortNum);
  if (Addr->empty()) {
    ++NumUnusedPorts;
    return;
  }

  VASTSelector *WData = getWData(PortNum);
  VASTSelector *ByteEn = getByteEn(PortNum);

  Addr->printRegisterBlock(OS, Mod, 0);
  WData->printRegisterBlock(OS, Mod, 0);
  ByteEn->printRegisterBlock(OS, Mod, 0);

  // Print the pipeline stages of the block RAM.
  // Shift the byte enable according to the byte address in a word.
  OS << "wire " << VASTValue::printBitRange(getByteEnWidth())
     << " mem" << Idx << 'p' << PortNum << "pipe0_be0w = " << ByteEn->getName()
     << " << " << Addr->getName() << VASTValue::printBitRange(ByteAddrWidth)
     << ";\n"
  // Shift the data according to the byte address also.
     << "wire "<< VASTValue::printBitRange(getDataWidth())
     << " mem" << Idx << 'p' << PortNum << "pipe0_wdata0w = ("
     << WData->getName() << " << { " << Addr->getName()
     << VASTValue::printBitRange(ByteAddrWidth) << ", 3'b0 });\n"
  // The enables.
     << "reg " << getInternalWEnName(PortNum) << ";\n";

  // Access the block ram.
  OS.always_ff_begin(false);

  if (!WData->empty()) {
    // Use the enable of the write data as the write enable.
    OS << getInternalWEnName(PortNum) << " <= "
        << WData->getName() << "_selector_enable;\n";

    // TODO: Guard the read pipeline stages by stage enable signal.
    OS.if_begin(getInternalWEnName(PortNum));

    for (unsigned i = 0; i < BytesPerWord; ++i) {
      OS.if_() << "mem" << Idx << 'p' << PortNum << "pipe0_be0w[" << i << "]";
      OS._then() << getArrayName() << "[" << Addr->getName()
         << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true) << "]"
            "[" << i << "]"
            " <= mem" << Idx << 'p' << PortNum << "pipe0_wdata0w"
         << VASTValue::printBitRange((i + 1) * 8, i * 8) << ";\n";

      OS.exit_block();
    }

    OS.exit_block();
  }

  OS << getRDataName(PortNum) << " <= " << getArrayName() << "[" << Addr->getName()
     << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true) << "];\n";
  OS << getLastStageAddrName(PortNum) << " <= " << Addr->getName() << ";\n";

  OS << "if (" << Addr->getName()
     << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true) << ">= "
     << NumWords << ")  $finish(\"Write access out of bound!\");\n";

  OS.always_ff_end(false);
}

VASTValPtr
VASTMemoryBus::getFinalRDataShiftAmountOperand(VASTModule *VM,
                                               unsigned PortNum) const {
  assert(requireByteEnable() && "Shift is only required for byteenable!");
  unsigned BytesPerWord = getDataWidth() / 8;
  unsigned ByteAddrWidth = Log2_32_Ceil(BytesPerWord);
  unsigned BitWidth = ByteAddrWidth + 3;

  std::string Operand;
  raw_string_ostream OS(Operand);
  OS << "{ " << getLastStageAddrName(PortNum)
     << VASTValue::printBitRange(ByteAddrWidth) << ", 3'b0 }";

  return VM->getOrCreateSymbol(OS.str(), BitWidth);
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

  const char *BRamInitFileDIRPath[] = { "Misc", "BRamInitFileDIR" };
  std::string BRamInitFileDIR = getStrValueFromEngine(BRamInitFileDIRPath);
  SmallString<1024> FullInitFilePath;
  sys::path::append(FullInitFilePath, BRamInitFileDIR, InitFileName);

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

    // Print the information about the global variable in the memory.
    OS << "/* Offset: " << StartOffset << ' ' << *GV->getType() << ' '
       << GV->getName() << "*/\n";
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

  printBlockPort(OS, Mod, 0, ByteAddrWidth, NumWords);
  if (isDualPort()) printBlockPort(OS, Mod, 1, ByteAddrWidth, NumWords);
}

void VASTMemoryBus::printBlockPort(vlang_raw_ostream &OS, const VASTModule *Mod,
                                   unsigned PortNum, unsigned ByteAddrWidth,
                                   unsigned NumWords) const {
  VASTSelector *Addr = getAddr(PortNum);
  // The port is not used if the address is not active.
  if (Addr->empty()) return;

  VASTSelector *RData = getRData(PortNum),
               *WData = getWData(PortNum);

  // Print the selectors.
  Addr->printSelector(OS);
  RData->printSelector(OS);
  WData->printSelector(OS);

  OS.always_ff_begin(false);

  if (!WData->empty()) {
    OS.if_begin(Twine(WData->getName()) + "_selector_enable");
    OS << getArrayName() << "[" << Addr->getName() << "_selector_wire"
        << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true) << ']'
        << " <= " << WData->getName() << "_selector_wire"
        << VASTValue::printBitRange(getDataWidth(), 0, false) << ";\n";

    OS.exit_block();
  }

  OS << RData->getName()
    << VASTValue::printBitRange(getDataWidth(), 0, false) << " <= "
    << ' ' << getArrayName() << "[" << Addr->getName() << "_selector_wire"
    << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true) << "];\n";

  // Verify the addresses.
  OS << "if (" << Addr->getName() << "_selector_wire"
      << VASTValue::printBitRange(getAddrWidth(), ByteAddrWidth, true)
      << ">= "<< NumWords <<") $finish(\"Write access out of bound!\");\n";
  if (ByteAddrWidth)
    OS << "if (" << Addr->getName() << "_selector_wire"
        << VASTValue::printBitRange(ByteAddrWidth, 0, true) << " != "
        << ByteAddrWidth << "'b0) $finish(\"Write access out of bound!\");\n";

  OS << "// synthesis translate_off\n";
  Addr->verifyAssignCnd(OS, Mod);
  WData->verifyAssignCnd(OS, Mod);
  RData->verifyAssignCnd(OS, Mod);
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end(false);
}

void VASTMemoryBus::print(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  if (requireByteEnable()) printBank(OS, Mod);
  else                     printBlockRAM(OS, Mod);
}
