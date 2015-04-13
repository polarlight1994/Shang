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
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "sir-rtl-codegen"

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

  // Some instructions should be treated differently.
  void visitIntToPtrInst(IntToPtrInst &I);
  void visitPtrToIntInst(PtrToIntInst &I);

  // Functions to print Verilog RTL code
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

  /// Functions to print registers
	void printMuxInReg(SIRRegister *Reg);
  void printRegister(SIRRegister *Reg);

	/// Functions to print SubModules
	void printInitializeFile(SIRMemoryBank *SMB);
	void printMemoryBankImpl(SIRMemoryBank *SMB, unsigned BytesPerGV,
		                       unsigned ByteAddrWidth, unsigned NumWords);
	void printMemoryBank(SIRMemoryBank *SMB);

  void generateCodeForRegisters();
	void generateCodeForMemoryBank();
};
}

void SIRControlPathPrinter::printMuxInReg(SIRRegister *Reg) {
  // If the register has no Fanin, then ignore it.
  if (Reg->assignmentEmpty()) return;

  // Register must have been synthesized
  Value *RegVal = Reg->getRegVal();
  Value *RegGuard = Reg->getRegGuard();

  if (RegVal) {
    OS << "// Synthesized MUX\n";
    OS << "wire " << Mangle(Reg->getName()) << "_register_guard = ";
    unsigned Guard_BitWidth = TD.getTypeSizeInBits(RegGuard->getType());
    assert(Guard_BitWidth == 1 && "Bad BitWidth of Guard!");
    SM->printAsOperand(OS, RegGuard, Guard_BitWidth);
    OS << ";\n";

    // If it is slot register, we only need the guard signal.
    if (Reg->isSlot()) return;

    // Print (or implement) the MUX by:
    // output = (Sel0 & FANNIN0) | (Sel1 & FANNIN1) ...
    OS << "wire " << BitRange(Reg->getBitWidth(), 0, false)
       << Mangle(Reg->getName()) << "_register_wire = ";
    SM->printAsOperand(OS, RegVal, Reg->getBitWidth());
    OS << ";\n";
  }
}

void SIRControlPathPrinter::printRegister(SIRRegister *Reg) {
  vlang_raw_ostream VOS(OS);

	if (Reg->assignmentEmpty()) {
		// Print the driver of the output ports.
		// Change the judgment to the type of register!
		if (Reg->getRegisterType() == SIRRegister::OutPort) {
			VOS.always_ff_begin();
			VOS << Mangle(Reg->getName()) << " <= "
				<< buildLiteralUnsigned(Reg->getInitVal(), Reg->getBitWidth()) << ";\n";
			VOS.always_ff_end();
		}

		return;
	}

	// Print the selector of the register.
	printMuxInReg(Reg);

	// Print the sequential logic of the register.
	VOS.always_ff_begin();
	// Reset the register.
	VOS << Mangle(Reg->getName()) << " <= "
		<< buildLiteralUnsigned(Reg->getInitVal(), Reg->getBitWidth()) << ";\n";  

	VOS.else_begin();

	// Print the assignment.
	if (Reg->isSlot()) {
		VOS << Mangle(Reg->getName()) << " <= " << Mangle(Reg->getName()) << "_register_guard"
			<< ";\n";
	} else {
		VOS.if_begin(Twine(Mangle(Reg->getName())) + Twine("_register_guard"));
		VOS << Mangle(Reg->getName()) << " <= " << Mangle(Reg->getName()) << "_register_wire"
			<< BitRange(Reg->getBitWidth(), 0, false) << ";\n";
		VOS.exit_block();
	}


	VOS.always_ff_end();
}

namespace {
static inline
int base_addr_less(const std::pair<GlobalVariable *, unsigned> *P1,
                   const std::pair<GlobalVariable *, unsigned> *P2) {
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

  static
  void WriteContext(raw_ostream &OS, VarVector &Vars, unsigned WordSizeInBytes,
                    unsigned EndByteAddr, const Twine &CombROMLHS) {
    MemContextWriter MCW(OS, Vars, WordSizeInBytes, EndByteAddr, CombROMLHS);
    MCW.writeContext();
  }
};
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
		report_fatal_error("Cannot open file '" + FullInitFilePath.str()
			+ "' for writing block RAM initialize file!\n");
		return;
	}

	SmallVector<std::pair<GlobalVariable *, unsigned>, 8> Vars;

  typedef std::map<GlobalVariable*, unsigned>::const_iterator iterator;
  for (iterator I = SMB->const_baseaddrs_begin(), E = SMB->const_baseaddrs_end(); I != E; ++I) {
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
	if (Addr->assign_empty()) return;

	SIRRegister *WData = SMB->getWData();
	
	printRegister(Addr);
	printRegister(WData);

	// Hack: If the read latency is bigger than 1, we should pipeline the input port.
	if (!WData->assign_empty()) 
		assert(SMB->getReadLatency() == 1 && "Need to pipeline input port!");

	VOS.always_ff_begin(false);

	if (!WData->assign_empty()) {
		VOS.if_begin(Twine(WData->getName()) + "en");

		VOS << SMB->getArrayName() << "[" << SMB->getAddrName()
			  << BitRange(SMB->getAddrWidth(), ByteAddrWidth, true) << "]"
			  << " <= " << SMB->getWDataName() << BitRange(SMB->getDataWidth()) << ";\n";

		VOS.exit_block();		
	}

	VOS << SMB->getRDataName() << BitRange(SMB->getDataWidth()) << " <= "
		  << SMB->getArrayName() << "[" << SMB->getAddrName()
			<< BitRange(SMB->getAddrWidth(), ByteAddrWidth, true) << "];\n";

	VOS.always_ff_end(false);
}

void SIRControlPathPrinter::printMemoryBank(SIRMemoryBank *SMB) {
	unsigned EndByteAddr = SMB->getEndByteAddr();
	unsigned BytesPerGV = SMB->getDataWidth() / 8;
	unsigned AddrWidth = Log2_32_Ceil(BytesPerGV);

	assert(EndByteAddr % BytesPerGV == 0 && "Current Offset is not aligned!");
	unsigned NumWords = (EndByteAddr / BytesPerGV);\

		// use a multi-dimensional packed array to model individual bytes within the
		// word. Please note that the bytes is ordered from 0 to 7 ([0:7]) because
		// so that the byte address can access the correct byte.
		OS << "(* ramstyle = \"no_rw_check\", max_depth = " << NumWords << " *) logic"
		   << BitRange(BytesPerGV) << BitRange(8) << ' ' << SMB->getArrayName()
		   << "[0:" << NumWords << "-1];\n";

	printInitializeFile(SMB);

	printMemoryBankImpl(SMB, BytesPerGV, AddrWidth, NumWords);
}

void SIRControlPathPrinter::generateCodeForRegisters() {  
  for (SIR::const_register_iterator I = SM->const_registers_begin(), E = SM->const_registers_end();
       I != E; ++I) {
    printRegister(*I);
  }
}

void SIRControlPathPrinter::generateCodeForMemoryBank() {
	for (SIR::const_submodulebase_iterator I = SM->submodules_begin(), E = SM->submodules_end();
		   I != E; ++I) {
		if(SIRMemoryBank *SMB = dyn_cast<SIRMemoryBank>(*I)) 
			printMemoryBank(SMB);	
	}		
}

void SIRDatapathPrinter::printSimpleOp(ArrayRef<Value *> Ops, const char *Opc) {
  
  unsigned BitWidth = TD.getTypeSizeInBits(Ops[0]->getType());
  
  for (int i = 0; i < Ops.size(); i++) {
    assert(BitWidth == TD.getTypeSizeInBits(Ops[i]->getType())
           && "The BitWidth not match!");
  }

  SM->printSimpleOpImpl(OS, Ops, Opc, BitWidth);
}

void SIRDatapathPrinter::printUnaryOps(ArrayRef<Value *>Ops, const char *Opc) {
  assert(Ops.size() == 1 && "Bad operand numbers!");
  OS << Opc;
  unsigned BitWidth = TD.getTypeSizeInBits(Ops[0]->getType());
  SM->printAsOperand(OS, Ops[0], BitWidth);
}

void SIRDatapathPrinter::printInvertExpr(ArrayRef<Value *> Ops) {
  assert(Ops.size() == 1 && "Bad operands number");
  unsigned BitWidth = TD.getTypeSizeInBits(Ops[0]->getType());

  OS << "(~";
  SM->printAsOperand(OS, Ops[0], BitWidth);
  OS << ")";
}

void SIRDatapathPrinter::printBitRepeat(ArrayRef<Value *> Ops) {
  assert(Ops.size() == 2 && "Bad operands number");
  unsigned BitWidth = TD.getTypeSizeInBits(Ops[0]->getType());
  ConstantInt *RepeatTimes = dyn_cast<ConstantInt>(Ops[1]);

  OS << '{' << RepeatTimes->getValue() << '{';
  SM->printAsOperand(OS, Ops[0], BitWidth);
  OS << "}}";
}

void SIRDatapathPrinter::printBitExtract(ArrayRef<Value *> Ops) {
  assert(Ops.size() == 3 && "Bad operands number");
  ConstantInt *UB = dyn_cast<ConstantInt>(Ops[1]);
  ConstantInt *LB = dyn_cast<ConstantInt>(Ops[2]);

  SM->printAsOperandImpl(OS, Ops[0], getConstantIntValue(UB), getConstantIntValue(LB));
}

void SIRDatapathPrinter::printBitCat(ArrayRef<Value *> Ops) {
  assert(Ops.size() == 2 && "Bad operands number");

  OS << "(({";
  SM->printAsOperand(OS, Ops[0], TD.getTypeSizeInBits(Ops[0]->getType()));
  OS << ", ";
  SM->printAsOperand(OS, Ops[1], TD.getTypeSizeInBits(Ops[1]->getType()));
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

  SM->printAsOperand(OS, Ops[0], TD.getTypeSizeInBits(Ops[0]->getType()));
  OS << ", ";
  SM->printAsOperand(OS, Ops[1], TD.getTypeSizeInBits(Ops[1]->getType()));
  OS << ", ";
  if (Ops.size() == 3) {
    assert(TD.getTypeSizeInBits(Ops[2]->getType()) == 1 && "Expected carry bit!");
    SM->printAsOperand(OS, Ops[2], 1);
  } else
    OS << "1'b0";
  OS << ", ";
  SM->printAsOperand(OS, &I, TD.getTypeSizeInBits(Callee->getReturnType()));
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

  SM->printAsOperand(OS, Ops[0], TD.getTypeSizeInBits(Ops[0]->getType()));
  OS << ", ";
  SM->printAsOperand(OS, Ops[1], TD.getTypeSizeInBits(Ops[1]->getType()));
  OS << ", ";
  SM->printAsOperand(OS, &I, TD.getTypeSizeInBits(Callee->getReturnType()));
  OS << ");\n";
  return true;
}

bool SIRDatapathPrinter::printSubModuleInstantiation(IntrinsicInst &I) {
  OS << ";\n";

  Intrinsic::ID ID = I.getIntrinsicID();
  switch (ID) {
  default: break;
  case Intrinsic::shang_add:
	case Intrinsic::shang_addc:
    if (printFUAdd(I)) return true;
    break;
  case Intrinsic::shang_mul:
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
  // Skip all the pseudo instruction.
  if (I.getIntrinsicID() == Intrinsic::shang_pseudo)
    return;

  unsigned BitWidth = TD.getTypeSizeInBits(I.getType());
  OS << "wire" << BitRange(BitWidth, 0, BitWidth > 1) << ' ';

  printName(OS, I);

  Intrinsic::ID ID = I.getIntrinsicID();
  switch (ID) {
  default: break;
  case Intrinsic::shang_add:
  case Intrinsic::shang_mul:
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
  case Intrinsic::shang_rand:
  case Intrinsic::shang_rxor:
  case Intrinsic::shang_bit_repeat:
  case Intrinsic::shang_bit_extract:
  case Intrinsic::shang_bit_cat:
    if (printExpr(I)) return;
    break;
  }
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
  // We just need to handle the bitwidth here because the width 
  // of operand may be larger than we need.
  SM->printAsOperand(OS, I.getOperand(0), BitWidth);

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
  SM->printAsOperand(OS, I.getOperand(0), BitWidth);

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
  void printRegisterBlock(const SIRRegister *Reg, raw_ostream &OS, DataLayout &TD,
                          uint64_t InitVal = 0);
  void printRegisterBlock(const SIRRegister *Reg, vlang_raw_ostream &OS, DataLayout &TD,
                          uint64_t InitVal = 0);

  void printOutPort(const SIROutPort *OutPort, raw_ostream &OS, DataLayout &TD);

	void generateCodeForTopModule();
  void generateCodeForDecl(SIR &SM);
  void generateCodeForDatapath(SIR &SM, DataLayout &TD);
  void generateCodeForControlpath(SIR &SM, DataLayout &TD);
  void generateCodeForMemoryBank(SIR &SM, DataLayout &TD);
  void generateCodeForOutPort(SIR &SM, DataLayout &TD);

	bool runOnSIR(SIR &SM);
	
	void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
		AU.addRequiredID(SIRSchedulingID);
    AU.addRequiredID(SIRRegisterSynthesisForCodeGenID);
		AU.setPreservesAll();
	}
};
}

void SIR2RTL::generateCodeForTopModule() {
  const char *FUTemplatePath[] = { "FUs", "CommonTemplate" };
  std::string FUTemplate = LuaI::GetString(FUTemplatePath);
  Out << FUTemplate << "\n";
}

void SIR2RTL::generateCodeForDecl(SIR &SM) {
  // Print code for module declaration.
  SM.printModuleDecl(Out);

	// Print code for register declaration.
	SM.printRegDecl(Out);
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

bool SIR2RTL::runOnSIR(SIR &SM) {
	// Remove the dead SIR instruction before the CodeGen.
	SM.gc();

  DataLayout &TD = getAnalysis<DataLayout>();
  Function &F = *(SM.getFunction());

  // Get the output path for Verilog code.
  std::string RTLOutputPath = LuaI::GetString("RTLOutput");
  std::string Error;
  raw_fd_ostream Output(RTLOutputPath.c_str(), Error);
  Out.setStream(Output);

  Out << "//Welcome to SIR framework\n";
  // Copy the basic modules from LUA script to the Verilog file.
  generateCodeForTopModule();
  // Generate the declarations for module and ports.
  generateCodeForDecl(SM);
  
  Out.module_begin();

  // Generate the code for data-path.
  generateCodeForDatapath(SM, TD);

  Out << "\n\n";

  // Generate the code for control-path.
  generateCodeForControlpath(SM, TD);

  Out.module_end();

  Out.flush();
  Out.setStream(nulls());
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
  INITIALIZE_PASS_DEPENDENCY(SIRRegisterSynthesisForCodeGen)
INITIALIZE_PASS_END(SIR2RTL, "shang-sir-verilog-writer",
                    "Write the RTL verilog code to output file.",
                    false, true)

