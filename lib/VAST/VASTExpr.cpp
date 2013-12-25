//===--------- VASTExpr.cpp - The expressions in Verilog AST ----*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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
#include "vast/VASTDatapathNodes.h"
#include "vast/VASTMemoryBank.h"
#include "vast/Utilities.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "verilog-ast-expression"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<bool>
  InstSubModForFU("shang-instantiate-submod-for-fu",
  cl::desc("Instantiate submodule for each functional unit"),
  cl::init(false));

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

static void printAshrOp(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
  printSignedOperand(OS, Ops[0]);
  OS << " >>> ";
  Ops[1].printAsOperand(OS);
}

static void printBitCat(raw_ostream &OS, ArrayRef<VASTUse> Ops) {
  OS << '{';
  printSimpleOp(OS, Ops, " , ");
  OS << '}';
}

static
void printBitRepeat(raw_ostream &OS, VASTValPtr Pattern, unsigned RepeatTimes) {
  OS << '{' << RepeatTimes << '{';
  Pattern.printAsOperand(OS);
  OS << "}}";
}

static bool IsComplementSop(const char *SOP) {
  for (; *SOP; ++SOP)
    if (*SOP == '\n')
      return (*(SOP - 1) == '0' || *(SOP - 1) == 'n');

  llvm_unreachable("Broken SOP!");
  return false;
}

static void printLUT(raw_ostream &OS, ArrayRef<VASTUse> Ops, const char *LUT) {
  // Invert the result if the LUT is inverted.
  // Please note that we had encoded the comment flag of the SOP into the
  // invert flag of the LUT string.
  if (IsComplementSop(LUT))
    OS << '~';

  // Interpret the sum of product table.
  const char *p = LUT;
  unsigned NumInputs = Ops.size();
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
  if (E->size() > 3)
   return false;

  const VASTUse &OpA = E->getOperand(0), &OpB = E->getOperand(1);

  OS << E->getFUName() << "#("
     << OpA->getBitWidth() << ", "
     << OpB->getBitWidth() << ", "
     << E->getBitWidth() << ") ";
  E->printSubModName(OS);
  OS << '(';

  OpA.printAsOperand(OS);
  OS << ", ";
  OpB.printAsOperand(OS);
  OS << ", ";
  if (E->size() == 3) {
    assert(E->getOperand(2)->getBitWidth() == 1 && "Expected carry bit!");
    E->getOperand(2).printAsOperand(OS);
  } else
    OS << "1'b0";
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
     << E->getBitWidth() << ") ";
  E->printSubModName(OS);
  OS << '(';

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

  OS << E->getFUName() << "#(" << Op->getBitWidth() << ") ";
  E->printSubModName(OS);
  OS << '(';
  Op.printAsOperand(OS);
  OS << ", ";
  E->printAsOperand(OS, false);
  OS << ");\n";

  return true;
}
//===----------------------------------------------------------------------===//
void VASTExpr::initializeOperands(ArrayRef<VASTValPtr> Ops) {
  assert(Ops.size() && "Unexpected empty operand list!");
  // Construct the uses that use the operand.
  for (unsigned i = 0; i < Ops.size(); ++i) {
    assert(Ops[i].get() && "Unexpected null VASTValPtr!");
    (void) new (Operands + i) VASTUse(this, Ops[i]);
  }
}

VASTExpr::VASTExpr(Opcode Opc, ArrayRef<VASTValPtr> Ops, unsigned BitWidth)
  : VASTMaskedValue(vastExpr, BitWidth), VASTOperandList(Ops.size()) {
  Contents64.Bank = NULL;
  Contents32.ExprNameID = 0;
  Contents16.ExprContents.Opcode = Opc;
  Contents16.ExprContents.LB = 0;
  initializeOperands(Ops);
}

VASTExpr::VASTExpr(ArrayRef<VASTValPtr> Ops, unsigned BitWidth, const char *SOP)
  : VASTMaskedValue(vastExpr, BitWidth), VASTOperandList(Ops.size()) {
  Contents64.SOP = SOP;
  Contents32.ExprNameID = 0;
  Contents16.ExprContents.Opcode = VASTExpr::dpLUT;
  Contents16.ExprContents.LB = 0;
  initializeOperands(Ops);
}

VASTExpr::VASTExpr(VASTValPtr Op, unsigned UB, unsigned LB)
  : VASTMaskedValue(vastExpr, UB - LB), VASTOperandList(1) {
  Contents64.Bank = NULL;
  Contents32.ExprNameID = 0;
  Contents16.ExprContents.Opcode = VASTExpr::dpBitExtract;
  Contents16.ExprContents.LB = LB;
  initializeOperands(Op);
}

VASTExpr::VASTExpr(VASTValPtr Addr, VASTMemoryBank *Bank, unsigned BitWidth)
  : VASTMaskedValue(vastExpr, BitWidth), VASTOperandList(1) {
  Contents64.Bank = Bank;
  Contents32.ExprNameID = 0;
  Contents16.ExprContents.Opcode = VASTExpr::dpROMLookUp;
  Contents16.ExprContents.LB = 0;
  initializeOperands(Addr);
}

VASTExpr::VASTExpr() : VASTMaskedValue(vastExpr, 1), VASTOperandList(0) {}

VASTExpr::~VASTExpr() {
}

bool VASTExpr::hasNameID() const {
  return Contents32.ExprNameID != 0;
}

void VASTExpr::assignNameID(unsigned NameID) {
  Contents32.ExprNameID = NameID;
}

unsigned VASTExpr::getNameID() const {
  return Contents32.ExprNameID;
}

void VASTExpr::printName(raw_ostream &OS) const {
  char Prefix = isTimingBarrier() ? 'k' : 'w';
  OS << Prefix << getNameID() << Prefix;
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
  OS << VASTValue::BitRange(UB, LB, getBitWidth() > 1);
}

bool VASTExpr::printAsOperandInteral(raw_ostream &OS) const {
  if (hasNameID()) {
    printName(OS);
    // Only printed the temp name, subexpression is not printed.
    return false;
  }

  OS << '(';
  typedef ArrayRef<VASTUse> UseArray;

  switch (getOpcode()) {
  case dpLUT: {
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
  case dpLshr: printSimpleUnsignedOp(OS, getOperands(), " >> "); break;
  case dpAshr: printAshrOp(OS, getOperands());                   break;

  case dpBitExtract: getOperand(0).printAsOperand(OS, getUB(), getLB()); break;

  case dpBitCat:    printBitCat(OS, getOperands());    break;
  case dpBitRepeat: printBitRepeat(OS, getOperand(0), getRepeatTimes()); break;

  case dpKeep:
    getOperand(0).printAsOperand(OS, getUB(), getLB());
    OS << "/*keep*/";
    break;

  case dpROMLookUp:
    OS << "[Combinational ROM]";
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
  case dpLshr:  return "shang_lshr";
  case dpAshr:  return "shang_ashr";
  case dpSGT:   return "shang_sgt";
  case dpUGT:   return "shang_ugt";
  case dpRAnd:  return "shang_rand";
  case dpRXor:  return "shang_rxor";
  case dpROMLookUp:  return "shang_comb_rom";
  default: break;
  }

  return 0;
}

bool VASTExpr::isInstantiatedAsSubModule() const {
  // Combinational ROM lookup shuld always be instantiated as submodule.
  return getOpcode() == dpROMLookUp ||
         (InstSubModForFU && getFUName() != NULL && hasNameID());
}

void VASTExpr::printSubModName(raw_ostream &OS) const {
  assert(isInstantiatedAsSubModule() && "Cannot print submodule name!");

  printName(OS);
  OS << '_' << getFUName();
}

bool VASTExpr::printFUInstantiation(raw_ostream &OS) const {
  switch (getOpcode()) {
  default: break;
  case VASTExpr::dpAdd:
    if (InstSubModForFU && hasNameID() && printFUAdd(OS, this))
      return true;
    break;
  case VASTExpr::dpMul:
  case VASTExpr::dpShl:
  case VASTExpr::dpAshr:
  case VASTExpr::dpLshr:
  case VASTExpr::dpSGT:
  case VASTExpr::dpUGT:
    if (InstSubModForFU && hasNameID() && printBinaryFU(OS, this))
      return true;
    break;
  case VASTExpr::dpRXor:
  case VASTExpr::dpRAnd:
    if (InstSubModForFU && hasNameID() && printUnaryFU(OS, this))
      return true;
    break;
  case VASTExpr::dpROMLookUp: {
    getROMContent()->printAsCombROM(this, getOperand(0), OS);
    return true;
  }
  }

  return false;
}

// Implementation of LUT related functions.
const char *VASTExpr::getLUT() const {
  assert(getOpcode() == VASTExpr::dpLUT && "Call getLUT on the wrong Expr type!");
  // The LUT is in the last operand.
  return Contents64.SOP;
}

bool VASTExpr::isComplementSOP() const {
  return IsComplementSop(getLUT());
}

void VASTExpr::ProfileWithoutOperands(FoldingSetNodeID& ID) const {
  VASTExpr::Opcode Opcode = getOpcode();
  ID.AddInteger(Opcode);
  switch (Opcode) {
  default:
    ID.AddInteger(getBitWidth());
    break;
  case VASTExpr::dpBitExtract:
    ID.AddInteger(getUB());
    ID.AddInteger(getLB());
    break;
  case VASTExpr::dpLUT:
    ID.AddPointer(getLUT());
    ID.AddInteger(getBitWidth());
    break;
  case VASTExpr::dpROMLookUp:
    ID.AddPointer(getROMContent());
    ID.AddInteger(getBitWidth());
    break;
  }
}

void VASTExpr::Profile(FoldingSetNodeID& ID) const {
  ProfileWithoutOperands(ID);

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
      OS << "(`gv" << VASTNamedValue::Mangle(V->getName()) << ')';
    else if (isa<UndefValue>(V))
      OS << getBitWidth() << "'bx";

    OS << ";\n";
    return;
  }
}
