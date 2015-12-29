//===--------------------SIRRTLCodeGen.cpp ----------------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Verilog RTL CodeGen based on the SIR.
//
//===----------------------------------------------------------------------===//

#include "sir/SIR.h"
#include "sir/SIRPass.h"
#include "sir/LangSteam.h"
#include "sir/Passes.h"

// For now, we use the LuaI.h in vast
// to set the output file.
#include "vast/LuaI.h"

#include "llvm/InstVisitor.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Constants.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/Debug.h"


using namespace llvm;
// To use the LUA in VAST
using namespace vast;

namespace llvm {
struct SIRDatapathPrinter : public InstVisitor<SIRDatapathPrinter, void> {
  raw_ostream &OS;
  SIR *SM;
  DataLayout &TD;

  SIRDatapathPrinter(raw_ostream &OS, SIR *SM, DataLayout &TD)
    : OS(OS), SM(SM), TD(TD) {}

  // Visit each BB in the SIR module.
  void visitBasicBlock(BasicBlock *BB);

  // All data-path instructions have been transformed
  // into Shang-Inst, AKA Intrinsic Inst.
  void visitIntrinsicInst(IntrinsicInst &I);

  // The BitCast instructions should be treated differently,
  // since we cannot implement them with Shang intrinsic
  // instructions due to the different type error when we
  // use the replaceAllUsesWith function.
  void visitIntToPtrInst(IntToPtrInst &I);
  void visitPtrToIntInst(PtrToIntInst &I);
  void visitBitCastInst(BitCastInst &I);

  // Functions to print Verilog RTL code
  void printAsOperand(raw_ostream &OS, Value *U, unsigned UB, unsigned LB = 0) {
    unsigned BitWidth = TD.getTypeSizeInBits(U->getType());

    // Print correctly if this value is a ConstantInt.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(U)) {
      // Need to slice the wanted bits.
      if (UB != CI->getBitWidth() || LB == 0) {
        assert(UB <= CI->getBitWidth() && UB > LB  && "Bad bit range!");
        APInt Val = CI->getValue();
        if (UB != CI->getBitWidth())
          Val = Val.trunc(UB);
        if (LB != 0) {
          Val = Val.lshr(LB);
          Val = Val.trunc(UB - LB);
        }
        CI = ConstantInt::get(CI->getContext(), Val);
      }
      OS << "((";
      printConstantIntValue(OS, CI);
      OS << "))";
      return;
    }
    else if (ConstantVector *CV = dyn_cast<ConstantVector>(U)) {
      unsigned Num = CV->getNumOperands();

      OS << "{";

      for (unsigned i = 0; i < Num; i++) {
        Constant *Elem = CV->getAggregateElement(i);

        if (ConstantInt *CI = dyn_cast<ConstantInt>(Elem)) {
          printConstantIntValue(OS, CI);
        }

        if (UndefValue *UV = dyn_cast<UndefValue>(Elem)) {
          OS << TD.getTypeSizeInBits(UV->getType()) << "'hx";
        }

        if (i != Num -1)
          OS << ", ";
      }

      OS << "}";
      return;
    }
    else if (ConstantAggregateZero *CAZ = dyn_cast<ConstantAggregateZero>(U)) {
      OS << "((";
      OS << UB - LB;
      OS << "'h0))";
      return;
    }
    else if (ConstantPointerNull *CPN = dyn_cast<ConstantPointerNull>(U)) {
      OS << "((";
      OS << UB - LB;
      OS << "'hx))";
      return;
    }
    else if (UndefValue *UV = dyn_cast<UndefValue>(U)) {
      OS << "((";
      OS << UB - LB;
      OS << "'hx))";
      return;
    }
    else if (GlobalValue *GV = dyn_cast<GlobalValue>(U)) {
      // Get the enableCosimulation property from the Lua file.
      bool enableCoSimulation = LuaI::GetBool("enableCoSimulation");
      if (enableCoSimulation) {
        // If we are running the co-simulation, then the we should
        // get the address of the GVs from the imported DPI-C SW
        // functions.
        OS << "((" << "`gv" + Mangle(GV->getName()) << "))";

        return;
      } else
        OS << "((" << Mangle(GV->getName());
    }
    // Print correctly if this value is a argument.
    else if (Argument *Arg = dyn_cast<Argument>(U)) {
      OS << "((" << Mangle(Arg->getName());
    }
    // Print correctly if this value is a SeqValue.
    else if (Instruction *Inst = dyn_cast<Instruction>(U)) {
      if (SIRRegister *Reg = SM->lookupSIRReg(Inst))
        OS << "((" << Mangle(Reg->getName());
      else
        OS << "((" << Mangle(Inst->getName());
    }

    unsigned OperandWidth = UB - LB;
    if (UB && BitWidth != 1)
      OS << BitRange(UB, LB, OperandWidth == 1);
    OS << "))";
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

  bool printExpr(IntrinsicInst &I);
  bool printFUAdd(IntrinsicInst &I);  
  bool printBinaryFU(IntrinsicInst &I);  
  bool printSubModuleInstantiation(IntrinsicInst &I);
  void printInvertExpr(ArrayRef<Value *> Ops);
  void printBitRepeat(ArrayRef<Value *> Ops);
  void printBitExtract(ArrayRef<Value *> Ops);
  void printBitCat(ArrayRef<Value *> Ops);
  void printUnaryOps(ArrayRef<Value *>Ops, const char *Opc);
  void printSimpleOp(ArrayRef<Value *> Ops, const char *Opc);
};

struct SIRControlPathPrinter {
  raw_ostream &OS;
  SIR *SM;
  DataLayout &TD;

  SIRControlPathPrinter(raw_ostream &OS, SIR *SM, DataLayout &TD)
    : OS(OS), SM(SM), TD(TD) {}

  /// Basic Print Functions
  void printAsOperand(raw_ostream &OS, Value *U, unsigned UB, unsigned LB = 0) {
    unsigned BitWidth = TD.getTypeSizeInBits(U->getType());

    // Print correctly if this value is a ConstantInt.
    if (ConstantInt *CI = dyn_cast<ConstantInt>(U)) {
      // Need to slice the wanted bits.
      if (UB != CI->getBitWidth() || LB == 0) {
        assert(UB <= CI->getBitWidth() && UB > LB  && "Bad bit range!");
        APInt Val = CI->getValue();
        if (UB != CI->getBitWidth())
          Val = Val.trunc(UB);
        if (LB != 0) {
          Val = Val.lshr(LB);
          Val = Val.trunc(UB - LB);
        }
        CI = ConstantInt::get(CI->getContext(), Val);
      }
      OS << "((";
      printConstantIntValue(OS, CI);
      OS << "))";
      return;
    }
    else if (ConstantVector *CV = dyn_cast<ConstantVector>(U)) {
      unsigned Num = CV->getNumOperands();

      OS << "{";

      for (unsigned i = 0; i < Num; i++) {
        Constant *Elem = CV->getAggregateElement(i);

        if (ConstantInt *CI = dyn_cast<ConstantInt>(Elem)) {
          printConstantIntValue(OS, CI);
        }

        if (UndefValue *UV = dyn_cast<UndefValue>(Elem)) {
          OS << TD.getTypeSizeInBits(UV->getType()) << "'hx";
        }

        if (i != Num -1)
          OS << ", ";
      }

      OS << "}";
      return;
    }
    else if (ConstantAggregateZero *CAZ = dyn_cast<ConstantAggregateZero>(U)) {
      OS << "((";
      OS << UB - LB;
      OS << "'h0))";
      return;
    }
    else if (ConstantPointerNull *CPN = dyn_cast<ConstantPointerNull>(U)) {
      OS << "((";
      OS << UB - LB;
      OS << "'hx))";
      return;
    }
    else if (UndefValue *UV = dyn_cast<UndefValue>(U)) {
      OS << "((";
      OS << UB - LB;
      OS << "'hx))";
      return;
    }
    else if (GlobalValue *GV = dyn_cast<GlobalValue>(U)) {
      // Get the enableCosimulation property from the Lua file.
      bool enableCoSimulation = LuaI::GetBool("enableCoSimulation");
      if (enableCoSimulation) {
        // If we are running the co-simulation, then the we should
        // get the address of the GVs from the imported DPI-C SW
        // functions.
        OS << "((" << "`gv" + GV->getName() << "))";

        return;
      } else
        OS << "((" << Mangle(GV->getName());
    }
    // Print correctly if this value is a argument.
    else if (Argument *Arg = dyn_cast<Argument>(U)) {
      OS << "((" << Mangle(Arg->getName());
    }
    // Print correctly if this value is a SeqValue.
    else if (Instruction *Inst = dyn_cast<Instruction>(U)) {
      if (SIRRegister *Reg = SM->lookupSIRReg(Inst))
        OS << "((" << Mangle(Reg->getName());
      else
        OS << "((" << Mangle(Inst->getName());
    }

    unsigned OperandWidth = UB - LB;
    if (UB && BitWidth != 1)
      OS << BitRange(UB, LB, OperandWidth == 1);
    OS << "))";
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

  /// Functions to print registers
  void printMuxInReg(SIRRegister *Reg, bool UsedAsGuard = false);
  void printRegister(SIRRegister *Reg, bool UsedAsGuard = false);

  /// Functions to print SubModules
  void printInitializeFile(SIRMemoryBank *SMB);
  void printMemoryBankImpl(SIRMemoryBank *SMB, unsigned BytesPerGV,
                           unsigned ByteAddrWidth, unsigned NumWords);
  void printMemoryBank(SIRMemoryBank *SMB);

  void printVirtualMemoryBank(SIRMemoryBank *SMB);

  void generateCodeForRegisters();
  void generateCodeForMemoryBank();
};
}

void SIRControlPathPrinter::printMuxInReg(SIRRegister *Reg, bool UsedAsGuard) {
  // If the register has no Fanin, then ignore it.
  if (Reg->fanin_empty()) return;

  // Register must have been synthesized
  Value *RegVal = Reg->getRegVal();
  Value *RegGuard = Reg->getRegGuard();

  if (RegVal) {
    OS << "// Synthesized MUX\n";
    OS << "wire " << Mangle(Reg->getName()) << "_register_guard = ";
    unsigned Guard_BitWidth = TD.getTypeSizeInBits(RegGuard->getType());
    assert(Guard_BitWidth == 1 && "Bad BitWidth of Guard!");
    printAsOperand(OS, RegGuard, Guard_BitWidth);
    OS << ";\n";

    // If it is used as Guard, we only need the guard signal.
    if (UsedAsGuard) return;

    // Print (or implement) the MUX by:
    // output = (Sel0 & FANNIN0) | (Sel1 & FANNIN1) ...
    OS << "wire " << BitRange(Reg->getBitWidth(), 0, false)
       << Mangle(Reg->getName()) << "_register_wire = ";
    printAsOperand(OS, RegVal, Reg->getBitWidth());
    OS << ";\n";
  }
}

void SIRControlPathPrinter::printRegister(SIRRegister *Reg, bool UsedAsGuard) {
  vlang_raw_ostream VOS(OS);

  if (Reg->fanin_empty()) {
    assert(Reg->isFUInput() && "Unexpected Fanin Empty register!");

    return;
  }

  // Print the selector of the register.
  printMuxInReg(Reg, UsedAsGuard);

  // Print the sequential logic of the register.
  VOS.always_ff_begin();
  // Reset the register.
  VOS << Mangle(Reg->getName()) << " <= "
      << buildLiteralUnsigned(Reg->getInitVal(), Reg->getBitWidth()) << ";\n";

  VOS.else_begin();

  // Print the assignment.
  if (UsedAsGuard) {
    VOS << Mangle(Reg->getName()) << " <= " << Mangle(Reg->getName())
        << "_register_guard" << ";\n";
  } else {
    VOS.if_begin(Twine(Mangle(Reg->getName())) + Twine("_register_guard"));
    VOS << Mangle(Reg->getName()) << " <= " << Mangle(Reg->getName())
        << "_register_wire" << BitRange(Reg->getBitWidth(), 0, false) << ";\n";
    VOS.exit_block();
  }


  VOS.always_ff_end();
}

namespace {
static inline int base_addr_less(const std::pair<GlobalVariable *, unsigned> *P1,
                                 const std::pair<GlobalVariable *, unsigned> *P2) {
  return P2->second - P1->second;
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

struct MemContextWriter {
  raw_ostream &OS;
  typedef SmallVector<std::pair<GlobalVariable *, unsigned>, 8> VarVector;
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

  static void WriteContext(raw_ostream &OS, VarVector &Vars, unsigned WordSizeInBytes,
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
    OS.indent(2) << "default: " << CombROMLHS << " = "
                 << WordSizeInBytes << "'b" << (NumCasesWritten ? 'x' : '0')
                 << ";\n";
}

void SIRControlPathPrinter::printInitializeFile(SIRMemoryBank *SMB) {
  vlang_raw_ostream VOS(OS);

  std::string InitFileName = "mem" + utostr_32(SMB->getNum()) + "ram_init.txt";

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
    OS << "\", " << SMB->getArrayName() << ");\n";
  } else {
    report_fatal_error("Cannot open file '" + FullInitFilePath.str() +
                       "' for writing block RAM initialize file!\n");
    return;
  }

  SmallVector<std::pair<GlobalVariable *, unsigned>, 8> Vars;

  typedef std::map<GlobalVariable*, unsigned>::const_iterator iterator;
  for (iterator I = SMB->const_baseaddrs_begin(), E = SMB->const_baseaddrs_end();
       I != E; ++I) {
    Vars.push_back(*I);

    // Print the information about the global variable in the memory.
    VOS << "/* Offset: " << I->second << ' ' << I->first->getType() << ' '
        << I->first->getName() << "*/\n";
  }

  array_pod_sort(Vars.begin(), Vars.end(), base_addr_less);
  MemContextWriter::WriteContext(InitFileO, Vars, SMB->getDataWidth() / 8,
                                 SMB->getEndByteAddr(), Twine());
}

void SIRControlPathPrinter::printMemoryBankImpl(SIRMemoryBank *SMB, unsigned BytesPerGV,
                                                unsigned ByteAddrWidth, unsigned NumWords) {
  vlang_raw_ostream VOS(OS);

  SIRRegister *Addr = SMB->getAddr();
  if (Addr->fanin_empty()) return;
  SIRRegister *Enable = SMB->getEnable();
  if (Enable->fanin_empty()) return;

  SIRRegister *WData = SMB->getWData();
  SIRRegister *WriteEn = SMB->getWriteEn();

  printRegister(Addr);
  printRegister(Enable, true);
  printRegister(WData);
  printRegister(WriteEn, true);

  if (SMB->requireByteEnable())
    printRegister(SMB->getByteEn());

  // Hack: If the read latency is bigger than 1, we should pipeline the input port.

  // Print the code for the WData and RData.
  VOS.always_ff_begin(false);
  if (!WData->fanin_empty()) {
    if (SMB->requireByteEnable()) {
      VOS.if_begin(SMB->getWriteEnName());

      // Handle a special circumstance when only one GV in memory bank.
      // Then the AddrWidth will be just the same with ByteAddrWidth.
      // So we should just cout the memXram[0];
      if (SMB->getAddrWidth() == ByteAddrWidth) {
        for (unsigned i = 0; i < BytesPerGV; ++i) {
          VOS.if_() << SMB->getByteEnName() << "[" << i << "]";
          VOS._then() << SMB->getArrayName() << "[0]" << "[" << i
                      << "] <= " << SMB->getWDataName()
                      << BitRange((i + 1) * 8, i * 8) << ";\n";

          VOS.exit_block();
        }

        VOS.else_begin();

        VOS << SMB->getRDataName() << BitRange(SMB->getDataWidth()) << " <= "
            << SMB->getArrayName() << "[0];\n";

        VOS.exit_block();

      } else {
        for (unsigned i = 0; i < BytesPerGV; ++i) {
          VOS.if_() << SMB->getByteEnName() << "[" << i << "]";
          VOS._then() << SMB->getArrayName() << "["
                      << SMB->getAddrName()
                      << BitRange(SMB->getAddrWidth(), ByteAddrWidth, true)
                      << "][" << i	<< "] <= " << SMB->getWDataName()
                      << BitRange((i + 1) * 8, i * 8) << ";\n";

          VOS.exit_block();
        }

        VOS.else_begin();

        VOS << SMB->getRDataName() << BitRange(SMB->getDataWidth()) << " <= "
            << SMB->getArrayName() << "[" << SMB->getAddrName()
            << BitRange(SMB->getAddrWidth(), ByteAddrWidth, true) << "];\n";

        VOS.exit_block();
      }
    } else {
      VOS.if_begin(SMB->getWriteEnName());

      // Handle a special circumstance when only one GV in memory bank.
      // Then the AddrWidth will be just the same with ByteAddrWidth.
      // So we should just cout the memXram[0];
      if (SMB->getAddrWidth() == ByteAddrWidth) {
        VOS << SMB->getArrayName() << "[0]"
            << " <= " << SMB->getWDataName() << BitRange(SMB->getDataWidth()) << ";\n";

        VOS.else_begin();

        VOS << SMB->getRDataName() << BitRange(SMB->getDataWidth()) << " <= "
            << SMB->getArrayName() << "[0];\n";

        VOS.exit_block();
      } else {
        VOS << SMB->getArrayName() << "[" << SMB->getAddrName()
            << BitRange(SMB->getAddrWidth(), ByteAddrWidth, true) << "]"
            << " <= " << SMB->getWDataName() << BitRange(SMB->getDataWidth()) << ";\n";

        VOS.else_begin();

        VOS << SMB->getRDataName() << BitRange(SMB->getDataWidth()) << " <= "
            << SMB->getArrayName() << "[" << SMB->getAddrName()
            << BitRange(SMB->getAddrWidth(), ByteAddrWidth, true) << "];\n";

        VOS.exit_block();
      }
    }
  } else {
    // Handle a special circumstance when only one GV in memory bank.
    // Then the AddrWidth will be just the same with ByteAddrWidth.
    // So we should just cout the memXram[0];
    if (SMB->getAddrWidth() == ByteAddrWidth) {
      VOS << SMB->getRDataName() << BitRange(SMB->getDataWidth()) << " <= "
          << SMB->getArrayName() << "[0];\n";
    } else {
      VOS << SMB->getRDataName() << BitRange(SMB->getDataWidth()) << " <= "
          << SMB->getArrayName() << "[" << SMB->getAddrName()
          << BitRange(SMB->getAddrWidth(), ByteAddrWidth, true) << "];\n";
    }
  }
  VOS.always_ff_end(false);
}

void SIRControlPathPrinter::printMemoryBank(SIRMemoryBank *SMB) {
  unsigned EndByteAddr = SMB->getEndByteAddr();
  unsigned BytesPerGV = SMB->getDataWidth() / 8;
  unsigned AddrWidth = Log2_32_Ceil(BytesPerGV);

  assert(EndByteAddr % BytesPerGV == 0 && "Current Offset is not aligned!");
  unsigned NumWords = (EndByteAddr / BytesPerGV);

  // use a multi-dimensional packed array to model individual bytes within the
  // word. Please note that the bytes is ordered from 0 to 7 ([0:7]) because
  // so that the byte address can access the correct byte.
  OS << "(* ramstyle = \"no_rw_check\", max_depth = " << NumWords << " *) logic"
     << BitRange(BytesPerGV) << BitRange(8) << ' ' << SMB->getArrayName()
     << "[0:" << NumWords << "-1];\n";

  printInitializeFile(SMB);

  printMemoryBankImpl(SMB, BytesPerGV, AddrWidth, NumWords);
}

void SIRControlPathPrinter::printVirtualMemoryBank(SIRMemoryBank *SMB) {
  vlang_raw_ostream VOS(OS);

  SIRRegister *Addr = SMB->getAddr();
  if (Addr->fanin_empty()) return;
  SIRRegister *Enable = SMB->getEnable();
  if (Enable->fanin_empty()) return;

  SIRRegister *WData = SMB->getWData();
  SIRRegister *WriteEn = SMB->getWriteEn();

  printRegister(Addr);
  printRegister(Enable, true);
  printRegister(WData);
  printRegister(WriteEn, true);

  if (SMB->requireByteEnable())
    printRegister(SMB->getByteEn());
}

void SIRControlPathPrinter::generateCodeForRegisters() {
  typedef SIR::register_iterator iterator;
  for (iterator I = SM->registers_begin(), E = SM->registers_end();
       I != E; ++I) {
    SIRRegister *Reg = I;

    // Ignore the registers created for FUnits like memory bank, since
    // they will be printed in Function generateCodeForMemoryBank.
    if (Reg->isFUInOut()) continue;

    printRegister(Reg, Reg->isSlot());
  }
}

void SIRControlPathPrinter::generateCodeForMemoryBank() {
  // Get the enableCosimulation property from the Lua file.
  bool enableCoSimulation = LuaI::GetBool("enableCoSimulation");

  typedef SIR::const_submodulebase_iterator iterator;
  for (iterator I = SM->const_submodules_begin(), E = SM->const_submodules_end();
       I != E; ++I) {
    if (SIRMemoryBank *SMB = dyn_cast<SIRMemoryBank>(*I)) {
      if (enableCoSimulation)
        // If we are running the co-simulation, we should handle the
        // CodeGen of the memory bank specially.
        printVirtualMemoryBank(SMB);
      else
        printMemoryBank(SMB);
    }
  }
}

void SIRDatapathPrinter::printSimpleOp(ArrayRef<Value *> Ops, const char *Opc) {
  unsigned BitWidth = TD.getTypeSizeInBits(Ops[0]->getType());

  for (unsigned i = 0; i < Ops.size(); i++)
    assert(BitWidth == TD.getTypeSizeInBits(Ops[i]->getType())
           && "The BitWidth not match!");

  printSimpleOpImpl(OS, Ops, Opc, BitWidth);
}

void SIRDatapathPrinter::printUnaryOps(ArrayRef<Value *>Ops, const char *Opc) {
  assert(Ops.size() == 1 && "Bad operand numbers!");

  OS << Opc;
  unsigned BitWidth = TD.getTypeSizeInBits(Ops[0]->getType());
  printAsOperand(OS, Ops[0], BitWidth);
}

void SIRDatapathPrinter::printInvertExpr(ArrayRef<Value *> Ops) {
  assert(Ops.size() == 1 && "Bad operands number");

  unsigned BitWidth = TD.getTypeSizeInBits(Ops[0]->getType());

  OS << "(~";
  printAsOperand(OS, Ops[0], BitWidth);
  OS << ")";
}

void SIRDatapathPrinter::printBitRepeat(ArrayRef<Value *> Ops) {
  assert(Ops.size() == 2 && "Bad operands number");

  unsigned BitWidth = TD.getTypeSizeInBits(Ops[0]->getType());
  ConstantInt *RepeatTimes = dyn_cast<ConstantInt>(Ops[1]);

  OS << '{' << RepeatTimes->getValue() << '{';
  printAsOperand(OS, Ops[0], BitWidth);
  OS << "}}";
}

void SIRDatapathPrinter::printBitExtract(ArrayRef<Value *> Ops) {
  assert(Ops.size() == 3 && "Bad operands number");

  ConstantInt *UB = dyn_cast<ConstantInt>(Ops[1]);
  ConstantInt *LB = dyn_cast<ConstantInt>(Ops[2]);

  printAsOperand(OS, Ops[0], getConstantIntValue(UB), getConstantIntValue(LB));
}

void SIRDatapathPrinter::printBitCat(ArrayRef<Value *> Ops) {
  assert(Ops.size() == 2 && "Bad operands number");

  OS << "(({";
  printAsOperand(OS, Ops[0], TD.getTypeSizeInBits(Ops[0]->getType()));
  OS << ", ";
  printAsOperand(OS, Ops[1], TD.getTypeSizeInBits(Ops[1]->getType()));
  OS << "}))";
}

bool SIRDatapathPrinter::printFUAdd(IntrinsicInst &I) {
  // Extract the called function
  Function *Callee = I.getCalledFunction();

  assert(Callee->arg_size() >= 2 && Callee->arg_size() <= 3
         && "bad operand number!");

  SmallVector<Value *, 3> Ops;
  typedef CallInst::op_iterator iterator;
  for (iterator i = I.op_begin(); i != I.op_end() - 1; i++)
    Ops.push_back(*i);

  OS << getFUName(I) << "#("
     << TD.getTypeSizeInBits(Ops[0]->getType()) << ", "
     << TD.getTypeSizeInBits(Ops[1]->getType()) << ", "
     << TD.getTypeSizeInBits(Callee->getReturnType()) << ") ";

  // Here need to print a module name, not value name
  printName(OS, I);
  OS << '_' << getFUName(I);

  OS << '(';

  printAsOperand(OS, Ops[0], TD.getTypeSizeInBits(Ops[0]->getType()));
  OS << ", ";
  printAsOperand(OS, Ops[1], TD.getTypeSizeInBits(Ops[1]->getType()));
  OS << ", ";
  if (Ops.size() == 3) {
    assert(TD.getTypeSizeInBits(Ops[2]->getType()) == 1 && "Expected carry bit!");
    printAsOperand(OS, Ops[2], 1);
  } else
    OS << "1'b0";
  OS << ", ";
  printAsOperand(OS, &I, TD.getTypeSizeInBits(Callee->getReturnType()));
  OS << ");\n";
  return true;
}

bool SIRDatapathPrinter::printBinaryFU(IntrinsicInst &I) {
  // Extract the called function
  Function *Callee = I.getCalledFunction();

  assert(Callee->arg_size() == 2 && "Not a binary expression!");

  SmallVector<Value *, 2> Ops;
  typedef CallInst::op_iterator iterator;
  for (iterator i = I.op_begin(); i != I.op_end(); i++)
    Ops.push_back(*i);

  OS << getFUName(I) << "#("
     << TD.getTypeSizeInBits(Ops[0]->getType()) << ", "
     << TD.getTypeSizeInBits(Ops[1]->getType()) << ", "
     << TD.getTypeSizeInBits(Callee->getReturnType()) << ") ";

  // Here need to print a module name, not value name
  printName(OS, I);
  OS << '_' << getFUName(I);

  OS << '(';

  printAsOperand(OS, Ops[0], TD.getTypeSizeInBits(Ops[0]->getType()));
  OS << ", ";
  printAsOperand(OS, Ops[1], TD.getTypeSizeInBits(Ops[1]->getType()));
  OS << ", ";
  printAsOperand(OS, &I, TD.getTypeSizeInBits(Callee->getReturnType()));
  OS << ");\n";
  return true;
}

bool SIRDatapathPrinter::printSubModuleInstantiation(IntrinsicInst &I) {
  Intrinsic::ID ID = I.getIntrinsicID();
  switch (ID) {
  default: break;
  case Intrinsic::shang_add:
  case Intrinsic::shang_addc:
    if (printFUAdd(I)) return true;
    break;
  case Intrinsic::shang_mul:
  case Intrinsic::shang_udiv:
  case Intrinsic::shang_sdiv:
  case Intrinsic::shang_shl:
  case Intrinsic::shang_lshr:
  case Intrinsic::shang_ashr:
  case Intrinsic::shang_sgt:
  case Intrinsic::shang_ugt: 
    if (printBinaryFU(I)) return true;
    break;
  }

  return false;
}

bool SIRDatapathPrinter::printExpr(IntrinsicInst &I) {
  OS << "assign ";

  printName(OS, I);

  unsigned BitWidth = TD.getTypeSizeInBits(I.getType());
  OS << BitRange(BitWidth, 0, BitWidth > 1);

  OS << " = ((";

  // Extract the called function
  Function *Callee = I.getCalledFunction();

  SmallVector<Value *, 3> Ops;
  typedef CallInst::op_iterator iterator;
  for (iterator i = I.op_begin(); i != I.op_end() - 1; i++)
    Ops.push_back(*i);


  switch (I.getIntrinsicID()) {
  case Intrinsic::shang_and:
    printSimpleOp(Ops, " & "); break;
  case Intrinsic::shang_or:
    printSimpleOp(Ops, " | "); break;
  case Intrinsic::shang_xor:
    printSimpleOp(Ops, " ^ "); break;
  case Intrinsic::shang_not:
    printInvertExpr(Ops); break;
  case Intrinsic::shang_rand:
    printUnaryOps(Ops, "&"); break;
  case Intrinsic::shang_rxor:
    printUnaryOps(Ops, "^"); break;
  case Intrinsic::shang_bit_repeat:
    printBitRepeat(Ops); break;
  case Intrinsic::shang_bit_extract:
    printBitExtract(Ops); break;
  case Intrinsic::shang_bit_cat:
    printBitCat(Ops); break;


  default: llvm_unreachable("Unknown datapath opcode!"); break;
  }

  OS << "));\n";
  return true;
}

void SIRDatapathPrinter::visitIntrinsicInst(IntrinsicInst &I) {
  // Skip all the reg_assign instruction since it has nothing
  // to do with the Datapath.
  if (I.getIntrinsicID() == Intrinsic::shang_reg_assign)
    return;

  Intrinsic::ID ID = I.getIntrinsicID();
  switch (ID) {
  default: break;
  case Intrinsic::shang_add:
  case Intrinsic::shang_mul:
  case Intrinsic::shang_udiv:
  case Intrinsic::shang_sdiv:
  case Intrinsic::shang_shl:
  case Intrinsic::shang_ashr:
  case Intrinsic::shang_lshr:
  case Intrinsic::shang_sgt:
  case Intrinsic::shang_ugt:
  case Intrinsic::shang_addc:
    if (printSubModuleInstantiation(I)) return;
    break;

  case Intrinsic::shang_and:
  case Intrinsic::shang_not:
  case Intrinsic::shang_or:
  case Intrinsic::shang_xor:
  case Intrinsic::shang_rand:
  case Intrinsic::shang_rxor:
  case Intrinsic::shang_bit_repeat:
  case Intrinsic::shang_bit_extract:
  case Intrinsic::shang_bit_cat:
    if (printExpr(I)) return;
    break;
  }

  llvm_unreachable("Unexpected opcode!");
}

// The IntToPtr and PtrToInt instruction should be treated specially.
void SIRDatapathPrinter::visitIntToPtrInst(IntToPtrInst &I) {
  unsigned BitWidth = TD.getTypeSizeInBits(I.getType());
  OS << "wire" << BitRange(BitWidth, 0, BitWidth > 1) << ' ';

  printName(OS, I);

  OS << " = ((";

  // In fact, cast operation doesn't change the value.
  // The type of value is defined by reader.
  // For example, 0x00000000 can be interpreted to be integer 0 
  // or float 0.0.
  // We just need to handle the BitWidth here because the width
  // of operand may be larger than we need.
  printAsOperand(OS, I.getOperand(0), BitWidth);

  OS << "));\n";
}

void SIRDatapathPrinter::visitPtrToIntInst(PtrToIntInst &I) {
  unsigned BitWidth = TD.getTypeSizeInBits(I.getType());
  OS << "wire" << BitRange(BitWidth, 0, BitWidth > 1) << ' ';

  printName(OS, I);

  OS << " = ((";

  // In fact, cast operation doesn't change the value.
  // The type of value is defined by reader.
  // For example, 0x00000000 can be interpreted to be integer 0
  // or float 0.0.
  // We just need to handle the BitWidth here because the width
  // of operand may be larger than we need.
  printAsOperand(OS, I.getOperand(0), BitWidth);

  OS << "));\n";
}

void SIRDatapathPrinter::visitBitCastInst(BitCastInst &I) {
  unsigned BitWidth = TD.getTypeSizeInBits(I.getType());
  OS << "wire" << BitRange(BitWidth, 0, BitWidth > 1) << ' ';

  printName(OS, I);

  OS << " = ((";

  // In fact, cast operation doesn't change the value.
  // The type of value is defined by reader.
  // For example, 0x00000000 can be interpreted to be integer 0
  // or float 0.0.
  // We just need to handle the BitWidth here because the width
  // of operand may be larger than we need.
  printAsOperand(OS, I.getOperand(0), BitWidth);

  OS << "));\n";
}

void SIRDatapathPrinter::visitBasicBlock(BasicBlock *BB) {
  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) 
    visit(I);
}

namespace {
struct SIR2RTL : public SIRPass {
  vlang_raw_ostream Out;

  /// @name FunctionPass interface

  static char ID;
  SIR2RTL() : SIRPass(ID), Out() {
    initializeSIR2RTLPass(*PassRegistry::getPassRegistry());
  }

  ~SIR2RTL(){}

  // Should be moved into Control path printer in the future
  void printRegisterBlock(const SIRRegister *Reg, raw_ostream &OS,
                          DataLayout &TD, uint64_t InitVal = 0);
  void printRegisterBlock(const SIRRegister *Reg, vlang_raw_ostream &OS,
                          DataLayout &TD, uint64_t InitVal = 0);

  void printOutPort(const SIROutPort *OutPort, raw_ostream &OS, DataLayout &TD);

  void generateCodeForTopModule(SIR &SM, DataLayout &TD);
  void generateCodeForDecl(SIR &SM, DataLayout &TD);
  void generateCodeForDatapath(SIR &SM, DataLayout &TD);
  void generateCodeForControlpath(SIR &SM, DataLayout &TD);
  void generateCodeForMemoryBank(SIR &SM, DataLayout &TD);
  void generateCodeForOutPort(SIR &SM, DataLayout &TD);

  void generateCodeForGlobalVariableScript(SIR &SM, DataLayout &TD);
  void generateCodeForSCIFScript(SIR &SM, DataLayout &TD);
  void generateCodeForTestsuite(SIR &SM, DataLayout &TD);

  bool runOnSIR(SIR &SM);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(SIRSchedulingID);
    AU.addRequiredID(SIRRegisterSynthesisForCodeGenID);
    /*AU.addRequiredID(SIRTimingScriptGenID);*/
    AU.setPreservesAll();
  }
};
}

void SIR2RTL::generateCodeForTopModule(SIR &SM, DataLayout &TD) {
  bool enableCoSimulation = LuaI::GetBool("enableCoSimulation");
  if (enableCoSimulation)
    generateCodeForGlobalVariableScript(SM, TD);

  const char *FUTemplatePath[] = { "FUs", "CommonTemplate" };
  std::string FUTemplate = LuaI::GetString(FUTemplatePath);
  Out << FUTemplate << "\n";
}

void SIR2RTL::generateCodeForDecl(SIR &SM, DataLayout &TD) {
  // Print code for module declaration.
  Function *F = SM.getFunction();
  Out << "module " << F->getValueName()->getKey();

  Out << "(\n";

  // Print code for port declaration.
  SIRPort *Port = *SM.const_ports_begin();
  Port->printDecl(Out.indent(4));
  typedef SIR::const_port_iterator port_iterator;
  for (port_iterator I = SM.const_ports_begin() + 1, E = SM.const_ports_end();
       I != E; ++I) {
    SIRPort *Port = *I;

    // Assign the ports to virtual pins.
    Out << ",\n (* altera_attribute = \"-name VIRTUAL_PIN on\" *)";
    Port->printDecl(Out.indent(4));
  }

  // Get the enableCosimulation property from the Lua file.
  bool enableCoSimulation = LuaI::GetBool("enableCoSimulation");

  if (enableCoSimulation) {
    // If we are running the co-simulation, we should add the
    // memory bank transfer interface ports with the software part.
    typedef SIR::const_submodulebase_iterator submod_iterator;
    for (submod_iterator I = SM.const_submodules_begin(), E = SM.const_submodules_end();
         I != E; ++I) {
      if (SIRMemoryBank *SMB = dyn_cast<SIRMemoryBank>(*I)) {
        SMB->printVirtualPortDecl(Out);
      }
    }
  }

  Out << ");\n\n\n";

  // Print code for register declaration.
  typedef SIR::register_iterator reg_iterator;
  for (reg_iterator I = SM.registers_begin(), E = SM.registers_end();
       I != E; ++I) {
    SIRRegister *Reg = I;

    // Do not need to declaration registers for the output and FUInOut,
    // since we have do it in MemoryBank declaration part.
    if (Reg->isOutPort() || Reg->isFUInOut()) continue;

    Reg->printDecl(Out.indent(2));
  }

  Out << "\n";

  if (!enableCoSimulation) {
    // If we are running the co-simulation, then all registers used in memory bank will
    // have been declared in Ports, then we do need to re-declared it.
    typedef SIR::const_submodulebase_iterator submod_iterator;
    for (submod_iterator I = SM.const_submodules_begin(), E = SM.const_submodules_end();
         I != E; ++I) {
      if (SIRMemoryBank *SMB = dyn_cast<SIRMemoryBank>(*I)) {
        // Do not need to print declaration for SIRMemoryBank#0,
        // since it is a interface in HW/SW co-simulation and
        // have been declared in ports.
        if (SMB->getNum() != 0)
          SMB->printDecl(Out.indent(2));
      }
    }
  }

  Out << "\n";

  // Print code for wire declaration.
  typedef Function::iterator bb_iterator;
  for (bb_iterator I = F->begin(), E = F->end(); I != E; ++I) {
    BasicBlock *BB = I;

    typedef BasicBlock::iterator inst_iterator;
    for (inst_iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
      Instruction *Inst = I;

      if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst)) {
        if (II->getIntrinsicID() == Intrinsic::shang_reg_assign)
          continue;

        raw_ostream &wireOS = Out.indent(2);
        unsigned BitWidth = TD.getTypeSizeInBits(II->getType());

        wireOS << "wire " << BitRange(BitWidth, 0, false);
        wireOS << Mangle(II->getName()) << ";\n";
      }
    }
  }

  Out << "\n";
}

void SIR2RTL::generateCodeForDatapath(SIR &SM, DataLayout &TD) {
  // Create the DataPathPrinter.
  SIRDatapathPrinter DPP(Out, &SM, TD);

  // Visit the basic block in topological order.
  Function *F = SM.getFunction();

  typedef Function::iterator iterator;
  for (iterator I = F->begin(), E = F->end(); I != E; ++I)
    DPP.visitBasicBlock(I);
}

void SIR2RTL::generateCodeForControlpath(SIR &SM, DataLayout &TD) {
  // Create the ControlPathPrinter.
  SIRControlPathPrinter CPP(Out, &SM, TD);

  // Generate code for registers in sequential logic.
  CPP.generateCodeForRegisters();
}

void SIR2RTL::generateCodeForMemoryBank(SIR &SM, DataLayout &TD) {
  // Create the ControlPathPrinter.
  SIRControlPathPrinter CPP(Out, &SM, TD);

  // Generate code for MemoryBanks.
  CPP.generateCodeForMemoryBank();
}

static void printConstant(raw_ostream &OS, uint64_t Val, Type* Ty, DataLayout *TD) {
  OS << '\'';
  if (TD->getTypeSizeInBits(Ty) == 1)
    OS << (Val ? '1' : '0');
  else {
    std::string FormatS =
      "%0" + utostr_32(TD->getTypeStoreSize(Ty) * 2) + "llx";
    OS << "0x" << format(FormatS.c_str(), Val);
  }
  OS << '\'';
}

static void ExtractConstant(raw_ostream &OS, Constant *C, DataLayout *TD) {
  if (ConstantInt *CI = dyn_cast<ConstantInt>(C)) {
    printConstant(OS, CI->getZExtValue(), CI->getType(), TD);
    return;
  }

  if (isa<ConstantPointerNull>(C)) {
    printConstant(OS, 0, C->getType(), TD);
    return;
  }

  if (ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(C)) {
    ExtractConstant(OS, CDS->getElementAsConstant(0), TD);
    for (unsigned i = 1, e = CDS->getNumElements(); i != e; ++i) {
      OS << ", ";
      ExtractConstant(OS, CDS->getElementAsConstant(i), TD);
    }
    return;
  }

  if (ConstantArray *CA = dyn_cast<ConstantArray>(C)) {
    ExtractConstant(OS, cast<Constant>(CA->getOperand(0)), TD);
    for (unsigned i = 1, e = CA->getNumOperands(); i != e; ++i) {
      OS << ", ";
      ExtractConstant(OS, cast<Constant>(CA->getOperand(i)), TD);
    }
    return;
  }

  llvm_unreachable("Unsupported constant type to bind to script engine!");
  OS << '0';
}

void SIR2RTL::generateCodeForGlobalVariableScript(SIR &SM, DataLayout &TD) {
  SMDiagnostic Err;

  // Put the global variable information to the script engine.
  if (!LuaI::EvalString("GlobalVariables = {}\n", Err))
    llvm_unreachable("Cannot create globalvariable table in scripting pass!");

  // Put the virtual memory bank information to the script engine.
  if (!LuaI::EvalString("VirtualMBs = {}\n", Err))
    llvm_unreachable("Cannot create virtualmb table in scripting pass!");

  Module *M = SM.getModule();

  std::string GVScript;
  raw_string_ostream GVSS(GVScript);
  // Push the global variable information into the script engine.
  for (Module::global_iterator GI = M->global_begin(), E = M->global_end();
       GI != E; ++GI ) {
    GlobalVariable *GV = GI;

    GVSS << "GlobalVariables." << Mangle(GV->getName()) << " = { ";
    GVSS << "isLocal = " << GV->hasLocalLinkage() << ", ";
    GVSS << "AddressSpace = " << GV->getType()->getAddressSpace() << ", ";
    GVSS << "Alignment = " << GV->getAlignment() << ", ";
    Type *Ty = cast<PointerType>(GV->getType())->getElementType();
    // The element type of a scalar is the type of the scalar.
    Type *ElemTy = Ty;
    unsigned NumElem = 1;
    // Try to expand multi-dimension array to single dimension array.
    while (const ArrayType *AT = dyn_cast<ArrayType>(ElemTy)) {
      ElemTy = AT->getElementType();
      NumElem *= AT->getNumElements();
    }
    GVSS << "NumElems = " << NumElem << ", ";

    GVSS << "ElemSize = " << TD.getTypeStoreSizeInBits(ElemTy) << ", ";

    // The initialer table: Initializer = { c0, c1, c2, ... }
    GVSS << "Initializer = ";
    if (!GV->hasInitializer())
      GVSS << "nil";
    else {
      Constant *C = GV->getInitializer();

      GVSS << "{ ";
      if (C->isNullValue()) {
        Constant *Null = Constant::getNullValue(ElemTy);

        ExtractConstant(GVSS, Null, &TD);
        for (unsigned i = 1; i < NumElem; ++i) {
          GVSS << ", ";
          ExtractConstant(GVSS, Null, &TD);
        }
      } else
        ExtractConstant(GVSS, C, &TD);

      GVSS << "}";
    }

    GVSS << '}';

    GVSS.flush();
    if (!LuaI::EvalString(GVScript, Err)) {
      llvm_unreachable("Cannot create globalvariable infomation!");
    }
    GVScript.clear();
  }

  std::string MBScript;
  raw_string_ostream MBSS(MBScript);
  // Push the virtual memory bank information into the script engine.
  for (SIR::const_submodulebase_iterator I = SM.const_submodules_begin(),
    E = SM.const_submodules_end(); I != E; I++) {
      SIRMemoryBank *SMB = dyn_cast<SIRMemoryBank>(*I);

      MBSS << "VirtualMBs." << SMB->getArrayName() << " = { ";
      MBSS << "EnableName = '" << SMB->getEnableName() << "', ";
      MBSS << "WriteEnName = '" << SMB->getWriteEnName() << "', ";
      MBSS << "RequireByteEn = " << SMB->requireByteEnable() << ", ";
      if (SMB->requireByteEnable()) {
        MBSS << "ByteEnName = '" << SMB->getByteEnName() << "', ";
        MBSS << "ByteEnWidth = " << SMB->getByteEnWidth() << ", ";
      }
      MBSS << "RDataName = '" << SMB->getRDataName() << "', ";
      MBSS << "WDataName = '" << SMB->getWDataName() << "', ";
      MBSS << "AddrName = '" << SMB->getAddrName() << "', ";
      MBSS << "DataWidth = " << SMB->getDataWidth() << ", ";
      MBSS << "AddrWidth = " << SMB->getAddrWidth() << " }";

      MBSS.flush();
      if (!LuaI::EvalString(MBScript, Err)) {
        llvm_unreachable("Cannot create virtualmb infomation!");
      }
      MBScript.clear();
  }


  // Run the script against the GlobalVariables table.
  std::string GVCodeGenScript;
  raw_string_ostream GVS(GVCodeGenScript);
  GVS << LuaI::GetString("GlobalVariableCodeGen");

  if (!LuaI::EvalString(GVS.str(), Err))
    report_fatal_error("In Scripting pass" + Err.getMessage());
  GVCodeGenScript.clear();

  std::string GlobalVariableCode = LuaI::GetString("GlobalVariableCode");
  Out << GlobalVariableCode << "\n";
}

void SIR2RTL::generateCodeForSCIFScript(SIR &SM, DataLayout &TD) {
  SMDiagnostic Err;
  const Function *F = SM.getFunction();

  std::string BasicInfo;
  raw_string_ostream BI(BasicInfo);

  BI << "FuncInfo = { ";
  BI << "Name = '" << F->getName() << "', ";

  BI << "ReturnSize = ";
  if (F->getReturnType()->isVoidTy())
    BI << '0';
  else
    BI << TD.getTypeStoreSizeInBits(F->getReturnType());
  BI << ", ";

  BI << "Args = { ";

  if (F->arg_size()) {
    Function::const_arg_iterator I = F->arg_begin();
    BI << "{ Name = '" << I->getName() << "', Size = "
       << TD.getTypeStoreSizeInBits(I->getType()) << "}";
    ++I;

    for (Function::const_arg_iterator E = F->arg_end(); I != E; ++I)
      BI << " , { Name = '" << I->getName() << "', Size = "
         << TD.getTypeStoreSizeInBits(I->getType()) << "}";
  }

  BI << "} }";

  BI.flush();
  if (!LuaI::EvalString(BI.str(), Err))
    llvm_unreachable("Cannot create function infomation!");
  BasicInfo.clear();

  std::string SCIFScript;
  raw_string_ostream SS(SCIFScript);
  SS << LuaI::GetString("SCIFCodeGen");

  if (!LuaI::EvalString(SS.str(), Err))
    report_fatal_error("In Scripting pass" + Err.getMessage());
  SCIFScript.clear();
}

void SIR2RTL::generateCodeForTestsuite(SIR &SM, DataLayout &TD) {
  bool enableCoSimulation = LuaI::GetBool("enableCoSimulation");
  if (enableCoSimulation)
    generateCodeForSCIFScript(SM, TD);
}

bool SIR2RTL::runOnSIR(SIR &SM) {
  // Remove the dead SIR instruction before the CodeGen.
  SM.gc();

  DataLayout &TD = getAnalysis<DataLayout>();

  // Get the output path for Verilog code.
  std::string RTLOutputPath = LuaI::GetString("RTLOutput");
  std::string Error;
  raw_fd_ostream Output(RTLOutputPath.c_str(), Error);
  Out.setStream(Output);

  // Copy the basic modules from LUA script to the Verilog file.
  generateCodeForTopModule(SM, TD);
  // Generate the declarations for module and ports.
  generateCodeForDecl(SM, TD);

  Out.module_begin();

  // Generate the code for data-path.
  generateCodeForDatapath(SM, TD);

  Out << "\n\n";

  // Generate the code for control-path.
  generateCodeForControlpath(SM, TD);

  // Generate the code for memory bank.
  generateCodeForMemoryBank(SM, TD);

  Out.module_end();

  Out.flush();
  Out.setStream(nulls());

  // Generate the code for testsuite.
  generateCodeForTestsuite(SM, TD);

  return false;
}

//===----------------------------------------------------------------------===//
char SIR2RTL::ID = 0;

Pass *llvm::createSIR2RTLPass() {
  return new SIR2RTL();
}

//===----------------------------------------------------------------------===//

INITIALIZE_PASS_BEGIN(SIR2RTL, "shang-sir-verilog-writer",
                      "Write the RTL verilog code to output file.",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRScheduling)
  /*INITIALIZE_PASS_DEPENDENCY(SIRRegisterSynthesisForCodeGen)*/
  INITIALIZE_PASS_DEPENDENCY(SIRTimingScriptGen)
INITIALIZE_PASS_END(SIR2RTL, "shang-sir-verilog-writer",
                    "Write the RTL verilog code to output file.",
                    false, true)

