//===---- VASTMemoryBank.cpp - Memory Ports in Verilog AST ------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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

#include "vast/VASTMemoryBank.h"
#include "vast/VASTModule.h"
#include "vast/LuaI.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#define DEBUG_TYPE "vast-memory-bus"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumUnusedPorts, "Number of unused ports in memory bus");

VASTMemoryBank::VASTMemoryBank(unsigned BusNum, unsigned AddrSize,
                               unsigned DataSize, bool RequireByteEnable,
                               bool IsDualPort, bool IsCombROM,
                               unsigned ReadLatency)
  : VASTSubModuleBase(VASTNode::vastMemoryBank, "", BusNum),
    AddrSize(AddrSize), DataSize(DataSize),
    RequireByteEnable(RequireByteEnable), IsDualPort(IsDualPort),
    IsCombROM(IsCombROM), ReadLatency(ReadLatency), EndByteAddr(0) {}

void VASTMemoryBank::addBasicPins(VASTModule *VM, unsigned PortNum) {
  assert(!isDefault() && "Just handle internal memory here");

  // Address pin
  VASTSelector *Address
    = VM->createSelector(getAddrName(PortNum), getAddrWidth(), this,
                         VASTSelector::FUInput);
  addFanin(Address);

  // Read (from memory) data pin
  unsigned RDataWidth = getDataWidth();
  // Also register the byte address so that we can align the result.
  if (requireByteEnable()) RDataWidth += getByteAddrWidth();
  VASTSelector *RData = VM->createSelector(getRDataName(PortNum), RDataWidth,
                                           this, VASTSelector::FUOutput);
  addFanout(RData);

  // Write (to memory) data pin
  VASTSelector *WData
    = VM->createSelector(getWDataName(PortNum), getDataWidth(), this,
                         VASTSelector::FUInput);
  addFanin(WData);  
}

void VASTMemoryBank::addExternalPins(VASTModule *VM) {
    assert(isDefault() && "Just handle external memory here");

    // Address pin
    VASTSelector *Address
      = VM->createSelector(getAddrName(0), getAddrWidth(), 0,
                           VASTSelector::FUInput);
    addFanin(Address);
    VM->addPort(Address, false);

    // Read (from memory) data pin
    VASTSelector *RData = VM->createSelector(getRDataName(0), getDataWidth(),
                                             0, VASTSelector::FUOutput);
    addFanout(RData);
    VM->addPort(RData, true);

    // Write (to memory) data pin
    VASTSelector *WData
      = VM->createSelector(getWDataName(0), getDataWidth(), 0,
                           VASTSelector::FUInput);
    addFanin(WData);
    VM->addPort(WData, false);

    // Byte enable pin
    assert(requireByteEnable() && "External always require byte enable pin");
    addByteEnables(VM, 0, 0);

    // Enable pin
    VASTSelector *Enable
      = VM->createSelector(getEnableName(0), 1, 0, VASTSelector::Enable);
    addFanin(Enable);
    VM->addPort(Enable, false);

    // Write enable pin
    VASTSelector *WriteEn
      = VM->createSelector(getWriteEnName(0), 1, 0, VASTSelector::Enable);
    addFanin(WriteEn);
    VM->addPort(WriteEn, false);
}

void VASTMemoryBank::addByteEnables(VASTModule *VM, VASTNode *Parent,
                                  unsigned PortNum) {
  VASTSelector *ByteEnable
    = VM->createSelector(getByteEnName(PortNum), getByteEnWidth(), Parent,
                         VASTSelector::FUInput);
  addFanin(ByteEnable);
  if (isDefault()) VM->addPort(ByteEnable, false);
}

void VASTMemoryBank::addPorts(VASTModule *VM) {
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

void VASTMemoryBank::addGlobalVariable(GlobalVariable *GV, unsigned SizeInBytes) {
  DEBUG(dbgs() << GV->getName() << " CurOffset: " << EndByteAddr << "\n");
  // Insert the GlobalVariable to the offset map, and calculate its offset.
  // Please note that the computation is in the byte address.
  assert(GV->getAlignment() >= (DataSize / 8) && "Bad alignment!");
  assert(EndByteAddr % (DataSize / 8) == 0 && "Bad CurrentOffset!");
  EndByteAddr = RoundUpToAlignment(EndByteAddr, GV->getAlignment());
  DEBUG(dbgs() << "Roundup to " << EndByteAddr << " according to alignment "
         << GV->getAlignment() << '\n');
  bool inserted = BaseAddrs.insert(std::make_pair(GV, EndByteAddr)).second;
  assert(inserted && "GV had already added!");
  (void) inserted;
  DEBUG(dbgs() << "Size of GV " << SizeInBytes << " Offset increase to "
         << (EndByteAddr + SizeInBytes) << "\n");
  EndByteAddr = RoundUpToAlignment(EndByteAddr + SizeInBytes, DataSize / 8);
  DEBUG(dbgs() << "Roundup to Word address " << EndByteAddr << "\n");
}

unsigned VASTMemoryBank::getStartOffset(GlobalVariable *GV) const {
  std::map<GlobalVariable*, unsigned>::const_iterator at = BaseAddrs.find(GV);
  assert(at != BaseAddrs.end() && "GV is not assigned to this memory bank?");
  return at->second;
}

VASTSelector *VASTMemoryBank::getAddr(unsigned PortNum) const {
  return getFanin(InputsPerPort * PortNum + 0);
}

VASTSelector *VASTMemoryBank::getWData(unsigned PortNum) const {
  return getFanin(InputsPerPort * PortNum + 1);
}

VASTSelector *VASTMemoryBank::getRData(unsigned PortNum) const {
  return getFanout(PortNum);
}

VASTSelector *VASTMemoryBank::getByteEn(unsigned PortNum) const {
  unsigned Offset = InputsPerPort;
  if (isDualPort()) Offset += InputsPerPort;

  return getFanin(Offset + PortNum);
}

VASTSelector *VASTMemoryBank::getEnable() const {
  assert(isDefault() && "Enable only exists in default memory bus!");
  unsigned Offset = InputsPerPort + 1;
  return getFanin(Offset);
}

VASTSelector *VASTMemoryBank::getWriteEnable() const {
  assert(isDefault() && "Write enable only exists in default memory bus!");
  unsigned Offset = InputsPerPort + 2;
  return getFanin(Offset);
}

unsigned VASTMemoryBank::getByteAddrWidth() const {
  assert(requireByteEnable() && "Called getByteAddrWidth on wrong memory bus!");
  unsigned ByteAddrWdith = Log2_32_Ceil(getDataWidth() / 8);
  assert(ByteAddrWdith && "Unexpected zero ByteAddrWdith!");
  return ByteAddrWdith;
}

std::string VASTMemoryBank::getAddrName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "addr";

  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "addr";
}

std::string VASTMemoryBank::getRDataName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "rdata";
  
  return getArrayName() + "p" + utostr(PortNum) + "rdata";
}

std::string VASTMemoryBank::getInernalRDataName(unsigned PortNum) const {
  assert(!isDefault() && "Unexpected port type!");

  return getArrayName() + "p" + utostr(PortNum) + "internal_rdata";
}

std::string VASTMemoryBank::getWDataName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "wdata";

  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "wdata";
}

std::string VASTMemoryBank::getByteEnName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "be";

  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "be";
}

std::string VASTMemoryBank::getEnableName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "en";

  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "en";
}

std::string VASTMemoryBank::getWriteEnName(unsigned PortNum) const {
  if (isDefault()) return "mem" + utostr(Idx) + "wen";

  return "mem" + utostr(Idx) + "p" + utostr(PortNum) + "wen";
}

std::string VASTMemoryBank::getArrayName() const {
  return "mem" + utostr(Idx) + "ram";
}

std::string VASTMemoryBank::getRDataName(unsigned PortNum,
                                         unsigned CurPipelineStage) const {
  assert(CurPipelineStage >= 2
         && "Cannot get the read data ealier than stage 2!");
  if (getReadLatency() <= CurPipelineStage)
    return getRDataName(PortNum);

  // TODO: Number the pipeline register?
  return getInernalRDataName(PortNum);
}

std::string VASTMemoryBank::getAddrName(unsigned PortNum,
                                        unsigned CurPipelineStage) const {
  assert(CurPipelineStage <= 1
         && "Cannot get the address latter than stage 1!");
  std::string AddrName = getAddrName(PortNum);

  if (getReadLatency() <= CurPipelineStage)
    AddrName += "_selector_wire";

  return AddrName;
}

std::string VASTMemoryBank::getWDataName(unsigned PortNum,
                                         unsigned CurPipelineStage) const {
  assert(CurPipelineStage <= 1
         && "Cannot get the write data latter than stage 1!");
  std::string WDataName = getWDataName(PortNum);

  if (getReadLatency() <= CurPipelineStage)
    WDataName += "_selector_wire";

  return WDataName;
}

std::string VASTMemoryBank::getByteEnName(unsigned PortNum,
                                          unsigned CurPipelineStage) const {
  assert(CurPipelineStage <= 1
         && "Cannot get the byte enable latter than stage 1!");
  std::string ByteEnName = getByteEnName(PortNum);

  if (getReadLatency() <= CurPipelineStage)
    ByteEnName += "_selector_wire";

  return ByteEnName;
}

void VASTMemoryBank::printPortDecl(raw_ostream &OS, unsigned PortNum) const {
  getRData(PortNum)->printDecl(OS);
  getAddr(PortNum)->printDecl(OS);
  getWData(PortNum)->printDecl(OS);

  if (requireByteEnable())
    getByteEn(PortNum)->printDecl(OS);

  if (!isDefault() && getReadLatency() > 2)
    VASTNamedValue::PrintDecl(OS, getInernalRDataName(PortNum),
                              getRData(PortNum)->getBitWidth(), true);
}

void VASTMemoryBank::printDecl(raw_ostream &OS) const {
  if (isDefault() || isCombinationalROM()) return;

  printPortDecl(OS, 0);
  if (isDualPort())
    printPortDecl(OS, 1);
}

void VASTMemoryBank::printBank(vlang_raw_ostream &OS) const {
  // The default memory bus are printed as module ports.
  if (isDefault()) return;

  // The width of the byte address in a word.
  unsigned BytesPerWord = getDataWidth() / 8;
  unsigned ByteAddrWidth = Log2_32_Ceil(BytesPerWord);
  assert(ByteAddrWidth && "Should print as block RAM!");

  assert(EndByteAddr % BytesPerWord == 0 && "CurrentOffset not aligned!");
  unsigned NumWords = (EndByteAddr / BytesPerWord);
  // use a multi-dimensional packed array to model individual bytes within the
  // word. Please note that the bytes is ordered from 0 to 7 ([0:7]) because
  // so that the byte address can access the correct byte.
  OS << "(* ramstyle = \"no_rw_check\", max_depth = " << NumWords << " *) logic"
    << VASTValue::BitRange(BytesPerWord) << VASTValue::BitRange(8)
    << ' ' << getArrayName() << "[0:" << NumWords << "-1];\n";

  writeInitializeFile(OS);

  printBanksPort(OS, 0, BytesPerWord, ByteAddrWidth, NumWords);
  if (isDualPort())
    printBanksPort(OS, 1, BytesPerWord, ByteAddrWidth, NumWords);
}

void
VASTMemoryBank::printBanksPort(vlang_raw_ostream &OS, unsigned PortNum,
                              unsigned BytesPerWord, unsigned ByteAddrWidth,
                              unsigned NumWords) const {
  // Print the read port.
  VASTSelector *Addr = getAddr(PortNum);
  if (Addr->empty()) {
    ++NumUnusedPorts;
    return;
  }

  VASTSelector *WData = getWData(PortNum);
  VASTSelector *ByteEn = getByteEn(PortNum);

  Addr->printRegisterBlock(OS, 0);
  WData->printRegisterBlock(OS, 0);
  ByteEn->printRegisterBlock(OS, 0);

  // Work around: It looks like that the single element array will be implemented
  // by block RAM in Stratix IV Platform, and the netlist simulation will
  // complain 'Port size (2 or 2) does not match connection size (1) for port'.
  // We can just add an zero to MSB to silence the warning.
  SmallString<64> AddrConnection;

  {
    raw_svector_ostream SS(AddrConnection);
    if (Addr->getBitWidth() == ByteAddrWidth + 1)
      SS << "{ 1'b0, ";

    SS << getAddrName(PortNum, 1)
       << VASTValue::BitRange(getAddrWidth(), ByteAddrWidth, true);

    if (Addr->getBitWidth() == ByteAddrWidth + 1)
      SS << " }";
  }
 
  if (!WData->empty()) {
    // We need to pipeline the input ports if the read latency is bigger than 1.
    if (getReadLatency() == 1)
      OS << "wire " << WData->getName() << "en = "
         << WData->getName() << "_selector_guard;\n";
    else
      OS << "reg " << WData->getName() << "en;\n";
  }

  // Access the block ram.
  OS.always_ff_begin(false);

  if (!WData->empty()) {
    if (getReadLatency() > 1) {
      OS << WData->getName() << "en" << " <= "
         << WData->getName() << "_selector_guard;\n";
    }

    // Use the enable of the write data as the write enable.
    OS.if_begin(Twine(WData->getName()) + "en");

    for (unsigned i = 0; i < BytesPerWord; ++i) {
      OS.if_() << getByteEnName(PortNum, 1) << "[" << i << "]";
      OS._then() << getArrayName() << "[" << AddrConnection << "]"
             "[" << i << "] <= " << getWDataName(PortNum, 1)
                 << VASTValue::BitRange((i + 1) * 8, i * 8) << ";\n";

      OS.exit_block();
    }

    OS.exit_block();
  }

  OS << getRDataName(PortNum, 2)
     << VASTValue::BitRange(getDataWidth(), 0, true)
     << " <= " << getArrayName() << "[" << AddrConnection << "];\n";

  OS << getRDataName(PortNum, 2)
     << VASTValue::BitRange(getDataWidth() + ByteAddrWidth,
                                 getDataWidth(), true)
     << " <= " << getAddrName(PortNum, 1)
     << VASTValue::BitRange(ByteAddrWidth, 0) << ";\n";

  if (getReadLatency() > 2) {
    assert(getReadLatency() == 3 && "Unsupported read latency!");
    OS << getRDataName(PortNum, 3) << " <= "
       << getRDataName(PortNum, 2) << ";\n";
  }

  OS << "// synthesis translate_off\n";
  OS << "if (" << Addr->getName()
     << VASTValue::BitRange(getAddrWidth(), ByteAddrWidth, true) << ">= "
     << NumWords << ")  $finish(\"Write access out of bound!\");\n";
  OS << "// synthesis translate_on\n";

  OS.always_ff_end(false);
}

static inline
int base_addr_less(const std::pair<GlobalVariable*, unsigned> *P1,
                   const std::pair<GlobalVariable*, unsigned> *P2) {
  return P2->second - P1->second;
}

typedef SmallVector<uint8_t, 1024> ByteBuffer;

static
unsigned FillByteBuffer(ByteBuffer &Buf, uint64_t Val, unsigned SizeInBytes) {
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
    unsigned PtrSizeInBytes = LuaI::Get<VFUMemBus>()->getAddrWidth() / 8;
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

namespace {
struct MemContextWriter {
  raw_ostream &OS;
  typedef SmallVector<std::pair<GlobalVariable*, unsigned>, 8> VarVector;
  VarVector &Vars;
  unsigned WordSizeInBytes;
  unsigned EndByteAddr;
  const Twine &CombROMLHS;

  MemContextWriter(raw_ostream &OS, VarVector &Vars, unsigned WordSizeInBytes,
                   unsigned EndByteAddr, const Twine &CombROMLHS)
    : OS(OS), Vars(Vars), WordSizeInBytes(WordSizeInBytes),
      EndByteAddr(EndByteAddr), CombROMLHS(CombROMLHS) {}

  void padZeroToByteAddr(raw_ostream &OS, unsigned CurByteAddr,
                         unsigned TargetByteAddr) {
    assert(TargetByteAddr % WordSizeInBytes == 0 && "Bad target byte address!");
    assert(CurByteAddr <= TargetByteAddr && "Bad current byte address!");
    while (CurByteAddr != TargetByteAddr) {
      OS << "00";
      ++CurByteAddr;
      if (CurByteAddr % WordSizeInBytes == 0)
        OS << "// " << CurByteAddr << '\n';
    }
  }

  void writeContext();

  static
  void WriteContext(raw_ostream &OS, VarVector &Vars, unsigned WordSizeInBytes,
                    unsigned EndByteAddr, const Twine &CombROMLHS) {
    MemContextWriter MCW(OS, Vars, WordSizeInBytes, EndByteAddr, CombROMLHS);
    MCW.writeContext();
  }
};
}

void MemContextWriter::writeContext() {
  unsigned NumCasesWritten = 0;
  unsigned CurByteAddr = 0;
  ByteBuffer Buffer;

  while (!Vars.empty()) {
    std::pair<GlobalVariable*, unsigned> Var = Vars.pop_back_val();

    GlobalVariable *GV = Var.first;
    DEBUG(dbgs() << GV->getName() << " CurByteAddress " << CurByteAddr << '\n');
    unsigned StartOffset = Var.second;
    if (CombROMLHS.isTriviallyEmpty())
      padZeroToByteAddr(OS, CurByteAddr, StartOffset);
    CurByteAddr = StartOffset;
    DEBUG(dbgs() << "Pad zero to " << StartOffset << '\n');
    OS << "//" << GV->getName() << " start byte address " << StartOffset << '\n';
    if (GV->hasInitializer() && !GV->getInitializer()->isNullValue()) {
      FillByteBuffer(Buffer, GV->getInitializer());
      unsigned BytesToPad = OffsetToAlignment(Buffer.size(), WordSizeInBytes);
      for (unsigned i = 0; i < BytesToPad; ++i)
        Buffer.push_back(0);

      assert(Buffer.size() % WordSizeInBytes == 0 && "Buffer does not padded!");
      for (unsigned i = 0, e = (Buffer.size() / WordSizeInBytes); i != e; ++i) {
        // Write the case assignment for combinational ROM.
        if (!CombROMLHS.isTriviallyEmpty()) {
          OS.indent(2) << CurByteAddr << ": " << CombROMLHS
                       << " = " << WordSizeInBytes * 8 << "'h";
          ++NumCasesWritten;
        }

        for (unsigned j = 0; j < WordSizeInBytes; ++j) {
          // Directly write out the buffer in little endian!
          unsigned Idx = i * WordSizeInBytes + (WordSizeInBytes - j - 1);
          OS << format("%02x", Buffer[Idx]);
          ++CurByteAddr;
        }

        if (!CombROMLHS.isTriviallyEmpty())
          OS << ';';
        OS << "// " << CurByteAddr << '\n';
      }

      assert((CurByteAddr % WordSizeInBytes) == 0 && "Bad ByteBuffer size!");
      DEBUG(dbgs() << "Write initializer: " << CurByteAddr << '\n');
      Buffer.clear();
    }
  }

  if (CombROMLHS.isTriviallyEmpty())
    padZeroToByteAddr(OS, CurByteAddr, EndByteAddr);
  else
    OS.indent(2)
      << "default: " << CombROMLHS << " = "
      << WordSizeInBytes << "'b" << (NumCasesWritten ? 'x' : '0') << ";\n";
}

void VASTMemoryBank::writeInitializeFile(vlang_raw_ostream &OS) const {

  std::string InitFileName = "mem" + utostr_32(Idx) + "ram_init.txt";

  SmallString<256> FullInitFilePath
    = sys::path::parent_path(LuaI::GetString("RTLOutput"));
  sys::path::append(FullInitFilePath, InitFileName);

  std::string ErrorInfo;
  const char *CFullInitFilePath = FullInitFilePath.c_str();
  raw_fd_ostream InitFileO(CFullInitFilePath, ErrorInfo);

  if (ErrorInfo.empty()) {
    DEBUG(dbgs() << "writing" << CFullInitFilePath << '\n');

    OS << "initial  $readmemh(\"";
    OS.write_escaped(FullInitFilePath);
    OS << "\", " << getArrayName() << ");\n";
  } else {
    report_fatal_error("Cannot open file '" + FullInitFilePath.str()
                       + "' for writing block RAM initialize file!\n");
    return;
  }

  SmallVector<std::pair<GlobalVariable*, unsigned>, 8> Vars;

  typedef std::map<GlobalVariable*, unsigned>::const_iterator iterator;
  for (iterator I = BaseAddrs.begin(), E = BaseAddrs.end(); I != E; ++I) {
    Vars.push_back(*I);

    // Print the information about the global variable in the memory.
    OS << "/* Offset: " << I->second << ' ' << *I->first->getType() << ' '
       << I->first->getName() << "*/\n";
  }

  array_pod_sort(Vars.begin(), Vars.end(), base_addr_less);
  MemContextWriter::WriteContext(InitFileO, Vars, getDataWidth() / 8,
                                 EndByteAddr, Twine());
}

void VASTMemoryBank::printBlockRAM(vlang_raw_ostream &OS) const {
  unsigned BytesPerWord = getDataWidth() / 8;
  unsigned ByteAddrWidth = Log2_32_Ceil(BytesPerWord);
  assert((EndByteAddr * 8) % getDataWidth() == 0
         && "CurrentOffset not aligned!");
  unsigned NumWords = (EndByteAddr * 8 / getDataWidth());
  // use a multi-dimensional packed array to model individual bytes within the
  // word. Please note that the bytes is ordered from 0 to 7 ([0:7]) because
  // so that the byte address can access the correct byte.
  OS << "(* ramstyle = \"no_rw_check\", max_depth = " << NumWords << " *) logic"
     << VASTValue::BitRange(getDataWidth())
     //<< VASTValue::BitRange(BytesPerWord) << VASTValue::BitRange(8)
     << ' ' << getArrayName() << "[0:" << NumWords << "-1];\n";

  writeInitializeFile(OS);

  printBlockPort(OS, 0, ByteAddrWidth, NumWords);
  if (isDualPort())
    printBlockPort(OS, 1, ByteAddrWidth, NumWords);
}

void
VASTMemoryBank::printBlockPort(vlang_raw_ostream &OS, unsigned PortNum,
                              unsigned ByteAddrWidth, unsigned NumWords) const {
  VASTSelector *Addr = getAddr(PortNum);
  // The port is not used if the address is not active.
  if (Addr->empty())
    return;

  VASTSelector *RData = getRData(PortNum),
               *WData = getWData(PortNum);

  Addr->printRegisterBlock(OS, 0);
  RData->printRegisterBlock(OS, 0);
  WData->printRegisterBlock(OS, 0);

  if (!WData->empty()) {
    // We need to pipeline the input ports if the read latency is bigger than 1.
    if (getReadLatency() == 1)
      OS << "wire " << WData->getName() << "en = "
         << WData->getName() << "_selector_guard;\n";
    else
      OS << "reg " << WData->getName() << "en;\n";
  }

  // Access the block ram.
  OS.always_ff_begin(false);

  if (!WData->empty()) {
    if (getReadLatency() > 1) {
      OS << WData->getName() << "en" << " <= "
         << WData->getName() << "_selector_guard;\n";
    }

    OS.if_begin(Twine(WData->getName()) + "en");
    OS << getArrayName() << "[" << getAddrName(PortNum, 1)
        << VASTValue::BitRange(getAddrWidth(), ByteAddrWidth, true) << ']'
        << " <= " << getWDataName(PortNum, 1)
        << VASTValue::BitRange(getDataWidth(), 0, false) << ";\n";

    OS.exit_block();
  }

  OS << getRDataName(PortNum, 2)
     << VASTValue::BitRange(getDataWidth(), 0, false) << " <= "
     << ' ' << getArrayName() << "[" << getAddrName(PortNum, 1)
     << VASTValue::BitRange(getAddrWidth(), ByteAddrWidth, true) << "];\n";

  if (getReadLatency() > 2) {
    assert(getReadLatency() == 3 && "Unsupported read latency!");
    OS << getRDataName(PortNum, 3) << " <= "
       << getRDataName(PortNum, 2) << ";\n";
  }

  OS << "// synthesis translate_off\n";
  // Verify the addresses.
  OS << "if (" << Addr->getName()
      << VASTValue::BitRange(getAddrWidth(), ByteAddrWidth, true)
      << ">= "<< NumWords <<") $finish(\"Write access out of bound!\");\n";
  if (ByteAddrWidth)
    OS << "if (" << Addr->getName()
        << VASTValue::BitRange(ByteAddrWidth, 0, true) << " != "
        << ByteAddrWidth << "'b0) $finish(\"Write access out of bound!\");\n";
  OS << "// synthesis translate_on\n";


  OS.always_ff_end(false);
}

void VASTMemoryBank::print(vlang_raw_ostream &OS) const {
  if (isCombinationalROM())
    return;

  if (requireByteEnable())
    printBank(OS);
  else
    printBlockRAM(OS);
}

void VASTMemoryBank::printAsCombROM(const VASTExpr *LHS, VASTValPtr Addr,
                                    raw_ostream &OS) const {
  assert(LHS->hasNameID() && "Unexpected unnamed CombROM Epxr!");

  std::string LHSName;
  raw_string_ostream SS(LHSName);
  LHS->printName(SS);
  SS.flush();

  unsigned DataWidth = LHS->getBitWidth();
  OS << "reg " << VASTValue::BitRange(getDataWidth()) << ' '
     << LHSName << "_comb_rom;\n";
  OS << "always @(*) begin// begin combinational ROM logic\n";

  SmallVector<std::pair<GlobalVariable*, unsigned>, 8> Vars;

  typedef std::map<GlobalVariable*, unsigned>::const_iterator iterator;
  for (iterator I = BaseAddrs.begin(), E = BaseAddrs.end(); I != E; ++I) {
    Vars.push_back(*I);

    // Print the information about the global variable in the memory.
    OS.indent(2) << "/* Offset: " << I->second << ' ' << *I->first->getType()
                 << ' ' << I->first->getName() << "*/\n";
  }

  array_pod_sort(Vars.begin(), Vars.end(), base_addr_less);

  // The width of the byte address in a word.
  unsigned BytesPerWord = DataWidth / 8;
  assert(((getDataWidth() / 8) % BytesPerWord) == 0 &&
         "Bad datawidth of rom lookup!");
  
  OS.indent(2) << VASTNode::FullCaseAttr << ' ' << VASTNode::ParallelCaseAttr
               << "case (";
  Addr.printAsOperand(OS);
  OS << ")";
  MemContextWriter::WriteContext(OS, Vars, BytesPerWord, EndByteAddr,
                                 LHSName + "_comb_rom");
  OS.indent(2) << "endcase //end case\n";
  OS << "end // end combinational ROM logic\n";
  OS << "assign " << LHSName << " = " << LHSName << "_comb_rom;\n";
}
