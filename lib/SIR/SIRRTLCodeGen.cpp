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
	void printMuxInReg(SIRRegister *Reg, raw_ostream &OS, DataLayout &TD);
  void printRegister(SIRRegister *Reg, vlang_raw_ostream &OS, DataLayout &TD);
  void printRegister(SIRRegister *Reg, raw_ostream &OS, DataLayout &TD);

  void generateCodeForRegisters();
};
}

void SIRControlPathPrinter::printMuxInReg(SIRRegister *Reg, raw_ostream &OS,
                                          DataLayout &TD) {
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

void SIRControlPathPrinter::printRegister(SIRRegister *Reg, vlang_raw_ostream &OS,
                                          DataLayout &TD) {
  if (Reg->assignmentEmpty()) {
    // Print the driver of the output ports.
    // Change the judgment to the type of register!
    if (Reg->getRegisterType() == SIRRegister::OutPort) {
      OS.always_ff_begin();
      OS << Mangle(Reg->getName()) << " <= "
         << buildLiteral(Reg->getInitVal(), Reg->getBitWidth(), false) << ";\n";
      OS.always_ff_end();
    }

    return;
  }

  // Print the selector of the register.
  printMuxInReg(Reg, OS, TD);

  // Print the sequential logic of the register.
  OS.always_ff_begin();
  // Reset the register.
  OS << Mangle(Reg->getName()) << " <= "
     << buildLiteral(Reg->getInitVal(), Reg->getBitWidth(), false) << ";\n";  

  OS.else_begin();

  // Print the assignment.
  if (Reg->isSlot()) {
    OS << Mangle(Reg->getName()) << " <= " << Mangle(Reg->getName()) << "_register_guard"
       << ";\n";
  } else {
    OS.if_begin(Twine(Mangle(Reg->getName())) + Twine("_register_guard"));
    OS << Mangle(Reg->getName()) << " <= " << Mangle(Reg->getName()) << "_register_wire"
       << BitRange(Reg->getBitWidth(), 0, false) << ";\n";
    OS.exit_block();
  }
  

  OS.always_ff_end();
}

void SIRControlPathPrinter::printRegister(SIRRegister *Reg, raw_ostream &OS,
                                          DataLayout &TD) {
  vlang_raw_ostream S(OS);
  printRegister(Reg, S, TD);
}

void SIRControlPathPrinter::generateCodeForRegisters() {  
  for (SIR::const_register_iterator I = SM->registers_begin(), E = SM->registers_end();
       I != E; ++I) {
    printRegister(*I, OS, TD);
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
  void generateCodeForRegisters(SIR &SM, DataLayout &TD);
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
  ReversePostOrderTraversal<BasicBlock*> RPO(&(F->getEntryBlock()));
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    DPP.visitBasicBlock(*I);
}

void SIR2RTL::generateCodeForControlpath(SIR &SM, DataLayout &TD) {
  // Create the ControlPathPrinter.
  SIRControlPathPrinter CPP(Out, &SM, TD);

  // Generate code for registers in sequential logic.
  CPP.generateCodeForRegisters();
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
