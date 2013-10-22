//===--------- VASTExpr.cpp - The expressions in Verilog AST ----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VASTExpr class. This class represents the expressions
// in the verilog abstract syntax tree.
//
//===----------------------------------------------------------------------===//
#include "shang/VASTDatapathNodes.h"
#include "shang/VASTMemoryPort.h"
#include "shang/Utilities.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "verilog-ast-expression"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<bool>
  InstSubModForFU("shang-instantiate-submod-for-fu",
  cl::desc("Instantiate submodule for each functional unit"),
  cl::init(true));

//===----------------------------------------------------------------------===//
static
raw_ostream &printAssign(raw_ostream &OS, const Twine &Name, unsigned BitWidth){
  OS << "assign " << Name
     << VASTValue::printBitRange(BitWidth, 0, false)
     << " = ";
  return OS;
}

template<typename OperandT>
static void printCombMux(raw_ostream &OS, ArrayRef<OperandT> Ops,
                         const Twine &LHSName, unsigned LHSWidth) {
  bool IsSimpleAssignment = (Ops.size() == 2);
  // Create the temporary signal.
  OS << "// Combinational MUX\n"
     << (IsSimpleAssignment ? "wire " : "reg ")
     << VASTValue::printBitRange(LHSWidth, 0, false)
     << ' ' << LHSName << "_mux_wire;\n";

  // Handle the trivial case trivially: Only 1 input.
  if (IsSimpleAssignment) {
    printAssign(OS, LHSName + "_mux_wire", LHSWidth);
    Ops[1].printAsOperand(OS);
    OS << ";\n\n";
    return;
  }

  // Print the mux logic.
  OS << "always @(*)begin  // begin mux logic\n";
  OS.indent(2) << VASTNode::ParallelCaseAttr << " case (1'b1)\n";
  for (unsigned i = 0; i < Ops.size(); i+=2) {
    OS.indent(4);
    Ops[i].printAsOperand(OS);
    OS << ": " << LHSName << "_mux_wire = ";
    Ops[i + 1].printAsOperand(OS);
    OS << ";\n";
  }

  // Write the default condition, otherwise latch will be inferred.
  OS.indent(4) << "default: " << LHSName << "_mux_wire = "
               << LHSWidth << "'bx;\n";
  OS.indent(2) << "endcase\nend  // end mux logic\n\n";
}

//----------------------------------------------------------------------------//
// Operand printing helper functions.
static void printSignedOperand(raw_ostream &OS, const VASTUse &U) {
  OS << "$signed(";
  U.printAsOperand(OS);
  OS << ")";
}

static void printUnsignedOperand(raw_ostream &OS, const VASTUse &U) {
  OS << "$unsigned(";
  U.printAsOperand(OS);
  OS << ")";
}

static void printOperand(raw_ostream &OS, const VASTUse &U) {
  U.printAsOperand(OS);
}

template<typename PrintOperandFN>
static void printSimpleOp(raw_ostream &OS, ArrayRef<VASTUse> Ops,
                          const char *Opc, PrintOperandFN &FN) {
  unsigned NumOps = Ops.size();
  assert(NumOps && "Unexpected zero operand!");
  FN(OS, Ops[0]);

  for (unsigned i = 1; i < NumOps; ++i) {
    OS << Opc;
    FN(OS, Ops[i]);
  }
}

static void printSimpleUnsignedOp(raw_ostream &OS, ArrayRef<VASTUse> Ops,
                                  const char *Opc) {
  printSimpleOp(OS, Ops, Opc, printUnsignedOperand);
}

static void printSimpleSignedOp(raw_ostream &OS, ArrayRef<VASTUse> Ops,
                                const char *Opc) {
  printSimpleOp(OS, Ops, Opc, printSignedOperand);
}

static void printSimpleOp(raw_ostream &OS, ArrayRef<VASTUse> Ops, const char *Opc) {
  printSimpleOp(OS, Ops, Opc, printOperand);
}

//----------------------------------------------------------------------------//
// Generic datapath printing helper function.
static void printUnaryOp(raw_ostream &OS, const VASTUse &U, const char *Opc) {
  OS << Opc;
  U.printAsOperand(OS);
}

static void printSRAOp(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
  printSignedOperand(OS, Ops[0]);
  OS << " >>> ";
  Ops[1].printAsOperand(OS);
}

static void printBitCat(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
  OS << '{';
  printSimpleOp(OS, Ops, " , ");
  OS << '}';
}

static void printBitRepeat(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
  OS << '{' << cast<VASTImmediate>((Ops[1]).get())->getAPInt() << '{';
  Ops[0].printAsOperand(OS);
  OS << "}}";
}

static void printLUT(raw_ostream &OS, ArrayRef<VASTUse> Ops, const char *LUT) {
  // Interpret the sum of product table.
  const char *p = LUT;
  unsigned NumInputs = Ops.size() - 1;
  // The LUT is in form of "Sum of Product", print the left parenthesis of the
  // sum first.
  OS << '(';

  while (*p) {
    OS << '(';
    bool AnyOperandPrinted = false;
    // Interpret the product.
    for (unsigned i = 0; i < NumInputs; ++i) {
      char c = *p++;
      if (c == '-') continue;

      // Connect the printed operands with '&' to build the product.
      if (AnyOperandPrinted) OS << '&';

      assert((c == '0' || c == '1') && "Unexpected SOP char!");
      // Print the operand, invert it if necessary.
      Ops[i].invert(c == '0').printAsOperand(OS);
      AnyOperandPrinted = true;
    }
    // Close the product.
    OS << ')';

    // Inputs and outputs are seperated by blank space.
    assert(*p == ' ' && "Expect the blank space!");
    ++p;

    // Is the output inverted?
    char c = *p++;
    assert((c == '0' || c == '1') && "Unexpected SOP char!");

    // Products are separated by new line.
    assert(*p == '\n' && "Expect the new line!");
    ++p;

    // Perform the OR to build the sum.
    if (*p) OS << '|';
  }
  // Close the sum.
  OS << ')';
}

static bool printFUAdd(raw_ostream &OS, const VASTExpr *E) {
  assert(E->size() >= 2 && E->size() <=3 && "bad operand number!");
  if (E->size() > 3) return false;

  const VASTUse &OpA = E->getOperand(0), &OpB = E->getOperand(1);

  OS << E->getFUName() << "#("
     << OpA->getBitWidth() << ", "
     << OpB->getBitWidth() << ", "
     << E->getBitWidth() << ") "
     << E->getSubModName() << '(';

  OpA.printAsOperand(OS);
  OS << ", ";
  OpB.printAsOperand(OS);
  OS << ", ";
  if (E->size() == 3) E->getOperand(2).printAsOperand(OS);
  else                OS << "1'b0";
  OS << ", ";
  E->printAsOperand(OS, false);
  OS << ");\n";
  return true;
}

static bool printBinaryFU(raw_ostream &OS, const VASTExpr *E) {
  assert(E->size() == 2 && "Not a binary expression!");
  const VASTUse &OpA = E->getOperand(0), &OpB = E->getOperand(1);

  OS << E->getFUName() << "#("
     << OpA->getBitWidth() << ", "
     << OpB->getBitWidth() << ", "
     << E->getBitWidth() << ") "
     << E->getSubModName() << '(';

  OpA.printAsOperand(OS);
  OS << ", ";
  OpB.printAsOperand(OS);
  OS << ", ";
  E->printAsOperand(OS, false);
  OS << ");\n";
  return true;
}

static bool printUnaryFU(raw_ostream &OS, const VASTExpr *E) {
  assert(E->size() == 1 && "Not a unary expression!");
  const VASTUse &Op = E->getOperand(0);

  OS << E->getFUName() << "#(" << Op->getBitWidth() << ") "
     << E->getSubModName() << '(';
  Op.printAsOperand(OS);
  OS << ", ";
  E->printAsOperand(OS, false);
  OS << ");\n";

  return true;
}
//===----------------------------------------------------------------------===//

VASTExpr::VASTExpr(Opcode Opc, uint8_t NumOps, unsigned UB, unsigned LB)
  : VASTValue(vastExpr, UB - LB), VASTOperandList(NumOps),
    Opc(Opc), UB(UB), LB(LB) {
  Contents.Name = 0;
  assert(NumOps && "Unexpected empty operand list!");
}

VASTExpr::VASTExpr()
  : VASTValue(vastExpr, 0), VASTOperandList(0), Opc(-1),
    UB(0), LB(0) {
  Contents.Name = 0;
}

VASTExpr::~VASTExpr() {}

bool VASTExpr::isAnonymous() const {
  return getOpcode() <= LastAnonymousOpc;
}

bool VASTExpr::hasName() const {
  return Contents.Name != 0;
}

void VASTExpr::nameExpr(const char *Name) {
  Contents.Name = Name;
}

const char *VASTExpr::getTempName() const {
  return Contents.Name;
}

void
VASTExpr::printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const {
  if (printAsOperandInteral(OS)) {
    // Warn the illegal code!
    if (UB != getBitWidth() || LB != 0)
      OS << UB << ':' << LB << "illegal bitslice of expr!";
    return;
  }

  assert(UB <= getBitWidth() && "Bad bit range!");
  OS << VASTValue::printBitRange(UB, LB, getBitWidth() > 1);
}

bool VASTExpr::printAsOperandInteral(raw_ostream &OS) const {
  if (hasName()) {
    OS << getTempName();
    // Only printed the temp name, subexpression is not printed.
    return false;
  }

  OS << '(';
  typedef ArrayRef<VASTUse> UseArray;

  switch (getOpcode()) {
  case dpLUT: {
    // Invert the result if the LUT is inverted.
    // Please note that we had encoded the comment flag of the SOP into the
    // invert flag of the LUT string.
    if (getOperand(size() - 1).isInverted())  OS << '~';
    printLUT(OS, getOperands(), getLUT());
    break;
  }
  case dpAnd: printSimpleOp(OS, getOperands(), " & "); break;

  case dpRAnd:  printUnaryOp(OS, getOperand(0), "&");  break;
  case dpRXor:  printUnaryOp(OS, getOperand(0), "^");  break;

  case dpSGT:   printSimpleSignedOp(OS, getOperands(),  " > ");  break;

  case dpUGT:   printSimpleUnsignedOp(OS, getOperands(),  " > ");  break;

  case dpAdd: printSimpleOp(OS, getOperands(), " + "); break;
  case dpMul: printSimpleUnsignedOp(OS, getOperands(), " * "); break;
  case dpShl: printSimpleUnsignedOp(OS, getOperands(), " << ");break;
  case dpSRL: printSimpleUnsignedOp(OS, getOperands(), " >> ");break;
  case dpSRA: printSRAOp(OS, getOperands());                   break;

  case dpAssign: getOperand(0).printAsOperand(OS, UB, LB); break;

  case dpBitCat:    printBitCat(OS, getOperands());    break;
  case dpBitRepeat: printBitRepeat(OS, getOperands()); break;

  case dpKeep:
    getOperand(0).printAsOperand(OS, UB, LB);
    break;

  default: llvm_unreachable("Unknown datapath opcode!"); break;
  }

  OS << ')';
  return true;
}

const char *VASTExpr::getFUName() const {
  switch (getOpcode()) {
  case dpAdd:   return "shang_addc";
  case dpMul:   return "shang_mult";
  case dpShl:   return "shang_shl";
  case dpSRL:   return "shang_srl";
  case dpSRA:   return "shang_sra";
  case dpSGT:   return "shang_sgt";
  case dpUGT:   return "shang_ugt";
  case dpRAnd:  return "shang_rand";
  case dpRXor:  return "shang_rxor";
  case dpCROM:  return "shang_comb_rom";
  default: break;
  }

  return 0;
}

const std::string VASTExpr::getSubModName() const {
  const char *FUName = getFUName();

  if (FUName == 0 || !InstSubModForFU) return std::string("");

  std::string Name(FUName);
  raw_string_ostream SS(Name);
  SS << getTempName() << 'w' ;
  switch (getOpcode()) {
  default:
    SS << getBitWidth();
    break;
  case dpSGT:
  case dpUGT:
  case dpRAnd:
  case dpRXor:
    SS << getOperand(0)->getBitWidth();
    break;
  }

  SS << 'b';

  SS.flush();
  return Name;
}

bool VASTExpr::printFUInstantiation(raw_ostream &OS) const {
  switch (getOpcode()) {
  default: break;
  case VASTExpr::dpAdd:
    if (InstSubModForFU && hasName() && printFUAdd(OS, this))
      return true;
    break;
  case VASTExpr::dpMul:
  case VASTExpr::dpShl:
  case VASTExpr::dpSRA:
  case VASTExpr::dpSRL:
  case VASTExpr::dpSGT:
  case VASTExpr::dpUGT:
    if (InstSubModForFU && hasName() && printBinaryFU(OS, this))
      return true;
    break;
  case VASTExpr::dpRXor:
  case VASTExpr::dpRAnd:
    if (InstSubModForFU && hasName() && printUnaryFU(OS, this))
      return true;
    break;
  case VASTExpr::dpCROM: {
    const VASTWrapper *TableWrapper = cast<VASTWrapper>(getOperand(1).get());
    VASTMemoryBus *Table = cast<VASTMemoryBus>(TableWrapper->getVASTNode());
    Table->printAsCombROM(this, getOperand(0), OS);
    return true;
  }
  }

  return false;
}

// Implementation of LUT related functions.
const char *VASTExpr::getLUT() const {
  assert(getOpcode() == VASTExpr::dpLUT && "Call getLUT on the wrong Expr type!");
  // The LUT is in the last operand.
  return getOperand(size() - 1).getAsLValue<VASTSymbol>()->getName();
}

void VASTExpr::Profile(FoldingSetNodeID& ID) const {
  ID.AddInteger(getOpcode());
  ID.AddInteger(UB);
  ID.AddInteger(LB);
  typedef VASTExpr::const_op_iterator op_iterator;
  for (op_iterator OI = op_begin(), OE = op_end(); OI != OE; ++OI) {
    VASTValPtr Operand = *OI;
    ID.AddPointer(Operand);
  }
}

void VASTExpr::dropUses() {
  dropOperands();
}

//----------------------------------------------------------------------------//
bool VASTWrapper::isX() const {
  Value *V = getLLVMValue();

  return V && isa<UndefValue>(V);
}

void VASTWrapper::printDecl(raw_ostream &OS) const {
  if (use_empty()) return;

  // Print the wrapper for the LLVM Values.
  if (Value *V = getLLVMValue()) {
    VASTNamedValue::printDecl(OS, false, " = ");
    if (isa<GlobalVariable>(V))
      OS << "(`gv" << ShangMangle(V->getName()) << ')';
    else if (isa<UndefValue>(V))
      OS << getBitWidth() << "'bx";

    OS << ";\n";
    return;
  }
}
