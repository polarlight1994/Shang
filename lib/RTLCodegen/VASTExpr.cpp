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

#include "vtm/VASTDatapathNodes.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "verilog-ast-expression"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<bool>
  InstSubModForFU("vtm-instantiate-submod-for-fu",
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

static void printSel(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
 printUnsignedOperand(OS, Ops[0]);
 OS << '?';
 printUnsignedOperand(OS, Ops[1]);
 OS << ':';
 printUnsignedOperand(OS, Ops[2]);
}

static void printBitCat(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
  OS << '{';
  printSimpleUnsignedOp(OS, Ops, " , ");
  OS << '}';
}

static void printBitRepeat(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
  OS << '{' << cast<VASTImmediate>((Ops[1]).get())->getAPInt() << '{';
  Ops[0].printAsOperand(OS);
  OS << "}}";
}

static bool printLUT(raw_ostream &OS, ArrayRef<VASTUse> Ops, const char *LUT) {
  // Interpret the sum of product table.
  const char *p = LUT;
  unsigned NumInputs = Ops.size() - 1;
  bool isComplemented = false;
  // The LUT is in form of "Sum of Product", print the left parenthesis of the
  // sum first.
  OS << '(';

  while (*p) {
    OS << '(';
    // Interpret the product.
    for (unsigned i = 0; i < NumInputs; ++i) {
      char c = *p++;
      switch (c) {
      default: llvm_unreachable("Unexpected SOP char!");
      case '-': /*Dont care*/ continue;
      case '1': Ops[i]->printAsOperand(OS, false); break;
      case '0': Ops[i]->printAsOperand(OS, true); break;
      }

      // Perform the AND to build the product.
      if (i < NumInputs - 1) OS << '&';
    }
    // Close the product.
    OS << ')';

    // Inputs and outputs are seperated by blank space.
    assert(*p == ' ' && "Expect the blank space!");
    ++p;

    // Is the output inverted?
    char c = *p++;
    assert((c == '0' || c == '1') && "Unexpected SOP char!");
    isComplemented = (c == '0');

    // Products are separated by new line.
    assert(*p == '\n' && "Expect the new line!");
    ++p;

    // Perform the OR to build the sum.
    if (*p) OS << '|';
  }
  // Close the sum.
  OS << ')';

  // Build the sum;
  return isComplemented;
}

static bool printFUAdd(raw_ostream &OS, const VASTWire *W) {
  assert(!W->getExpr().isInverted() && "Unexpected inverted mux!");
  VASTExpr *E = dyn_cast<VASTExpr>(W->getExpr());
  if (E == 0) return false;

  assert(E->size() >= 2 && E->size() <=3 && "bad operand number!");
  if (E->size() > 3) return false;

  const VASTUse &OpA = E->getOperand(0), &OpB = E->getOperand(1);

  if (OpA.isa<VASTImmediate>() || OpA.isa<VASTSymbol>()) return false;
  if (OpB.isa<VASTImmediate>() || OpB.isa<VASTSymbol>()) return false;

  OS << E->getFUName() << "#("
     << OpA->getBitWidth() << ", "
     << OpB->getBitWidth() << ", "
     << W->getBitWidth() << ") "
     << E->getSubModName() << '(';

  OpA.printAsOperand(OS);
  OS << ", ";
  OpB.printAsOperand(OS);
  OS << ", ";
  if (E->size() == 3) E->getOperand(2).printAsOperand(OS);
  else                OS << "1'b0";
  OS << ", ";
  W->printAsOperand(OS, false);
  OS << ");\n";
  return true;
}

static bool printBinFU(raw_ostream &OS, const VASTWire *W) {
  assert(!W->getExpr().isInverted() && "Unexpected inverted mux!");
  VASTExpr *E = dyn_cast<VASTExpr>(W->getExpr());
  assert(E->size() == 2 && "Not a binary expression!");
  if (E == 0) return false;

  const VASTUse &OpA = E->getOperand(0), &OpB = E->getOperand(1);

  if (OpA.isa<VASTImmediate>() || OpA.isa<VASTSymbol>()) return false;
  if (OpB.isa<VASTImmediate>() || OpB.isa<VASTSymbol>()) return false;

  OS << E->getFUName() << "#("
     << OpA->getBitWidth() << ", "
     << OpB->getBitWidth() << ", "
     << W->getBitWidth() << ") "
     << E->getSubModName() << '(';

  OpA.printAsOperand(OS);
  OS << ", ";
  OpB.printAsOperand(OS);
  OS << ", ";
  W->printAsOperand(OS, false);
  OS << ");\n";
  return true;
}
//===----------------------------------------------------------------------===//

VASTExpr::VASTExpr(Opcode Opc, uint8_t NumOps, unsigned UB, unsigned LB)
  : VASTValue(vastExpr, UB - LB),
    VASTOperandList(reinterpret_cast<VASTUse*>(this + 1), NumOps),
    IsNamed(0), Opc(Opc), UB(UB), LB(LB) {
  Contents.Name = 0;
  assert(NumOps && "Unexpected empty operand list!");
}

bool VASTExpr::isInlinable() const {
  return getOpcode() <= LastInlinableOpc;
}

std::string VASTExpr::getTempName() const {
  return "t" + utohexstr(intptr_t(this)) + "t";
}

void VASTExpr::printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                  unsigned LB) const {
  //assert(((UB == this->UB && LB == this->LB) || hasName())
  //       && "Cannot print bitslice of Expr!");

  if (printAsOperandInteral(OS)) {
    assert(UB == this->UB && LB == this->LB && "Cannot print bitslice of Expr!");
    return;
  }

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
  case dpLUT:
    // Invert the result if the LUT is inverted.
    if (printLUT(OS, getOperands(), getLUT()))
      OS << '^'
         << VASTImmediate::buildLiteral(~UINT64_C(0), getBitWidth(), false);
    break;
  case dpAnd: printSimpleUnsignedOp(OS, getOperands(), " & "); break;

  case dpRAnd:  printUnaryOp(OS, getOperand(0), "&");  break;
  case dpRXor:  printUnaryOp(OS, getOperand(0), "^");  break;

  case dpSGE:   printSimpleSignedOp(OS, getOperands(),  " >= "); break;
  case dpSGT:   printSimpleSignedOp(OS, getOperands(),  " > ");  break;

  case dpUGE:   printSimpleUnsignedOp(OS, getOperands(),  " >= "); break;
  case dpUGT:   printSimpleUnsignedOp(OS, getOperands(),  " > ");  break;

  case dpAdd: printSimpleUnsignedOp(OS, getOperands(), " + "); break;
  case dpMul: printSimpleUnsignedOp(OS, getOperands(), " * "); break;
  case dpShl: printSimpleUnsignedOp(OS, getOperands(), " << ");break;
  case dpSRL: printSimpleUnsignedOp(OS, getOperands(), " >> ");break;
  case dpSRA: printSRAOp(OS, getOperands());                   break;

  case dpSel: printSel(OS, getOperands());                     break;

  case dpAssign: getOperand(0).printAsOperand(OS, UB, LB); break;

  case dpBitCat:    printBitCat(OS, getOperands());    break;
  case dpBitRepeat: printBitRepeat(OS, getOperands()); break;

  default: llvm_unreachable("Unknown datapath opcode!"); break;
  }

  OS << ')';
  return true;
}

const char *VASTExpr::getFUName() const {
  switch (getOpcode()) {
  case dpAdd: return "shang_addc";
  case dpMul: return "shang_mult";
  case dpShl: return "shang_shl";
  case dpSRL: return "shang_sra";
  case dpSRA: return "shang_srl";
  case dpSGE: return "shang_sge";
  case dpSGT: return "shang_sgt";
  case dpUGE: return "shang_uge";
  case dpUGT: return "shang_ugt";
  default: break;
  }

  return 0;
}

const std::string VASTExpr::getSubModName() const {
  const char *FUName = getFUName();

  if (FUName == 0 || !InstSubModForFU) return std::string("");

  std::string Name(FUName);
  raw_string_ostream SS(Name);
  SS << this << 'w' ;
  switch (getOpcode()) {
  default:
    SS << getBitWidth();
    break;
  case dpSGE:
  case dpSGT:
  case dpUGE:
  case dpUGT:
    SS << getOperand(0)->getBitWidth();
    break;
  }

  SS.flush();
  return Name;
}

static char *utobin_buffer(uint64_t X, char *BuffurStart, unsigned NumDigit) {
  char *BufPtr = BuffurStart, *BufferEnd = BuffurStart + NumDigit;

  while (BufPtr < BufferEnd) {
    unsigned char Mod = static_cast<unsigned char>(X) & 1;
    *BufPtr++ = '0' + Mod;
    X >>= 1;
  }

  return BufPtr;
}

// Implementation of LUT related functions.
const char *VASTExpr::getLUT() const {
  // 6 is the maximum supported input number of a single LUT.
  // 3 is for the space, the value of the truth table and the newline char in
  // each row.
  static char SOPBuffer[(6 + 3) /*Columns*/ * (1 << 6) /*Rows*/ + 1];
  unsigned NumInputs = Size - 1;
  unsigned NumRows = 1 << NumInputs;
  unsigned NumCols = NumInputs + 3;
  assert(NumRows * NumCols + 1 <= array_lengthof(SOPBuffer) && "LUT too big!");

  VASTImmediate *TruthImm = cast<VASTImmediate>(getOperand(NumInputs).get());
  uint64_t Truth = TruthImm->getZExtValue();

  unsigned NumRowsAdded = 0;
  for (unsigned i = 0; i < NumRows; ++i) {
    char *CurRow = SOPBuffer + NumRowsAdded * NumCols;

    unsigned char Mod = static_cast<unsigned char>(Truth) & 1;
    Truth >>= 1;
    // Ignore the row with zero value in the LUT.
    if (!Mod) continue;

    utobin_buffer(i, CurRow, NumInputs);

    CurRow[NumInputs + 0] = ' ';
    CurRow[NumInputs + 1] = '1';
    CurRow[NumInputs + 2] = '\n';
    ++NumRowsAdded;
  }

  // Terminate the SOP string.
  SOPBuffer[NumRowsAdded * NumCols] = 0;
  return SOPBuffer;
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

//----------------------------------------------------------------------------//
// Helper function for Verilog RTL printing.

static raw_ostream &printAssign(raw_ostream &OS, const VASTWire *Wire) {
  return printAssign(OS, Wire->getName(), Wire->getBitWidth());
}

static void printCombMux(raw_ostream &OS, const VASTWire *W) {
  assert(!W->getExpr().isInverted() && "Unexpected inverted mux!");
  VASTExpr *E = W->getExpr().get();
  unsigned NumOperands = E->size();
  assert((NumOperands & 0x1) == 0 && "Expect even operand number for CombMUX!");

  printCombMux(OS, E->getOperands(), W->getName(), W->getBitWidth());

  // Assign the temporary signal to the wire.
  printAssign(OS, W) << W->getName() << "_mux_wire;\n";
}

void VASTWire::dropUses() {
  if (getOperand(0).get()) getOperand(0).unlinkUseFromUser();
}

void VASTWire::printAssignment(raw_ostream &OS) const {
  VASTValPtr V = getDriver();
  assert(V && "Cannot print the wire!");

  if (VASTExpr *Expr = dyn_cast<VASTExpr>(V)) {
    switch (Expr->getOpcode()) {
    default: break;
    case VASTExpr::dpAdd:
      if (InstSubModForFU && !Expr->hasName() && printFUAdd(OS, this)) return;
      break;
    case VASTExpr::dpMul:
    case VASTExpr::dpShl:
    case VASTExpr::dpSRA:
    case VASTExpr::dpSRL:
    case VASTExpr::dpSGE:
    case VASTExpr::dpSGT:
    case VASTExpr::dpUGE:
    case VASTExpr::dpUGT:
      if (InstSubModForFU && !Expr->hasName() && printBinFU(OS, this)) return;
      break;
    case VASTExpr::dpMux: printCombMux(OS, this); return;
    }
  }

  printAssign(OS, this);
  V.printAsOperand(OS);
  OS << ";\n";
}

void VASTExpr::dropUses() {
  for (VASTUse *I = Operands, *E = Operands + Size; I != E; ++I)
    I->unlinkUseFromUser();
}
