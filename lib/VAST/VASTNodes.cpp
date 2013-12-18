//===----------------- VASTNodes.cpp - Nodes in VAST  -----------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements various (simple) nodes in the Verilog Abstract Syntax
// Tree.
//
//===----------------------------------------------------------------------===//
#include "LangSteam.h"
#include "vast/Strash.h"
#include "vast/VASTHandle.h"
#include "vast/VASTDatapathNodes.h"
#include "vast/VASTMemoryBank.h"
#include "vast/VASTModule.h"
#include "vast/Utilities.h"

#define DEBUG_TYPE "vast-node"
#include "llvm/Support/Debug.h"

using namespace llvm;

//----------------------------------------------------------------------------//
void VASTNode::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

void VASTNode::dropUses() {
  dbgs() << "Current Type " << unsigned(getASTType()) << '\n';
  llvm_unreachable("Subclass should implement this function!");
}

static std::string GetSTAObjectName(const VASTMemoryBank *RAM) {
  std::string Name;
  raw_string_ostream OS(Name);

  OS << " *" << RAM->getArrayName() << "* ";
  return OS.str();
}

static std::string GetSTAObjectName(const VASTSelector *Sel) {
  std::string Name;
  raw_string_ostream OS(Name);

  if (Sel->isFUOutput()) {
    if (const VASTMemoryBank *RAM = dyn_cast<VASTMemoryBank>(Sel->getParent()))
      return GetSTAObjectName(RAM);
  }

  OS << ' ' << Sel->getName() << "* ";

  return OS.str();
}

static std::string GetSTAObjectName(const VASTValue *V) {
  std::string Name;
  raw_string_ostream OS(Name);
  if (const VASTNamedValue *NV = dyn_cast<VASTNamedValue>(V)) {
    if (const VASTSeqValue *SV = dyn_cast<VASTSeqValue>(NV))
      return GetSTAObjectName(SV->getSelector());

    // The block RAM should be printed as Prefix + ArrayName in the script.
    if (const char *N = NV->getName()) {
      OS << ' ' << N << "* ";
      return OS.str();
    }
  } else if (const VASTExpr *E = dyn_cast<VASTExpr>(V)) {
    std::string Name = E->getSubModName();
    if (!Name.empty()) {
      OS << ' ' << Name << "* ";
      return OS.str();
    } else if (E->hasName()) {
      OS << ' ' << E->getTempName() << "* ";
      return OS.str();
    }
  }

  return "";
}

std::string VASTNode::getSTAObjectName() const {
  if (const VASTValue *V = dyn_cast<VASTValue>(this))
    return GetSTAObjectName(V);

  if (const VASTSelector *Sel = dyn_cast<VASTSelector>(this))
    return GetSTAObjectName(Sel);

  if (const VASTMemoryBank *RAM = dyn_cast<VASTMemoryBank>(this))
    return GetSTAObjectName(RAM);

  if (const VASTOutPort *Port = dyn_cast<VASTOutPort>(this))
    return GetSTAObjectName(Port->getSelector());

  if (const VASTRegister *Reg = dyn_cast<VASTRegister>(this))
    return GetSTAObjectName(Reg->getSelector());

  return "";
}

std::string VASTNode::DirectClkEnAttr = "";
std::string VASTNode::ParallelCaseAttr = "";
std::string VASTNode::FullCaseAttr = "";

//----------------------------------------------------------------------------//
void VASTValue::print(raw_ostream &OS) const {
  printAsOperandImpl(OS);
}

void VASTValue::printAsOperand(raw_ostream &OS, unsigned UB, unsigned LB,
                               bool isInverted) const{
  if (isInverted) OS << "(~";
  OS << '(';
  printAsOperandImpl(OS, UB, LB);
  OS << ')';
  if (isInverted) OS << ')';
}

void VASTValue::printAsOperand(raw_ostream &OS, bool isInverted) const {
  if (isInverted) OS << "(~";
  OS << '(';
  printAsOperandImpl(OS);
  OS << ')';
  if (isInverted) OS << ')';
}

void VASTValue::printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                   unsigned LB) const {
  assert(0 && "VASTValue::printAsOperand should not be called!");
}

std::string VASTValue::printBitRange(unsigned UB, unsigned LB, bool printOneBit){
  std::string ret;
  assert(UB && UB > LB && "Bad bit range!");
  --UB;
  if (UB != LB)
    ret = "[" + utostr_32(UB) + ":" + utostr_32(LB) + "]";
  else if(printOneBit)
    ret = "[" + utostr_32(LB) + "]";

  return ret;
}

bool VASTValue::extractSupportingSeqVal(std::set<VASTSeqValue*> &SeqVals,
                                        bool StopAtTimingBarrier) {
  VASTValue *Root = this;

  std::set<VASTExpr*> Visited;
  VASTExpr *Expr = dyn_cast<VASTExpr>(Root);
  // The entire tree had been visited.
  if (Expr == 0) {
    // If ChildNode is a not data-path operand list, it may be the SeqVal.
    if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(Root))
      SeqVals.insert(SeqVal);

    return !SeqVals.empty();
  }

  typedef VASTOperandList::op_iterator ChildIt;
  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(Expr, Expr->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == Node->op_end()) {
      VisitStack.pop_back();
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTExpr *ChildExpr = dyn_cast<VASTExpr>(ChildNode)) {
      if (StopAtTimingBarrier && ChildExpr->isTimingBarrier())
        continue;

      // ChildNode has a name means we had already visited it.
      if (!Visited.insert(ChildExpr).second) continue;

      VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));
      continue;
    }

    // If ChildNode is a not data-path operand list, it may be the SeqVal.
    if (VASTSeqValue *SeqVal = dyn_cast_or_null<VASTSeqValue>(ChildNode)) {
      SeqVals.insert(SeqVal);
      continue;
    }
  }

  return !SeqVals.empty();
}

VASTValue::VASTValue(VASTTypes T, unsigned BitWidth) : VASTNode(T) {
  assert(T >= vastFirstValueType && T <= vastLastValueType
         && "Bad DeclType!");
  Contents16.ValueBitwidth = BitWidth;
}

VASTValue::~VASTValue() {
  // Do not call the destructor of UseList. They are deleted by the
  // VASTOperandList.
  // We should check if use list is empty if necessary.
  assert(use_empty() && "Something is still using this value!");
  // UseList.clearAndLeakNodesUnsafely();
}

//===----------------------------------------------------------------------===//
// Value and type printing
std::string
VASTConstant::buildLiteral(uint64_t Value, unsigned bitwidth, bool isMinValue) {
  std::string ret;
  ret = utostr_32(bitwidth) + '\'';
  if (bitwidth == 1) ret += "b";
  else               ret += "h";
  // Mask the value that small than 4 bit to prevent printing something
  // like 1'hf out.
  if (bitwidth < 4) Value &= (1 << bitwidth) - 1;

  if(isMinValue) {
    ret += utohexstr(Value);
    return ret;
  }

  std::string ss = utohexstr(Value);
  unsigned int uselength = (bitwidth/4) + (((bitwidth&0x3) == 0) ? 0 : 1);
  if(uselength < ss.length())
    ss = ss.substr(ss.length() - uselength, uselength);
  ret += ss;

  return ret;
}

void VASTConstant::printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                       unsigned LB) const {
  APInt Operand = Int;
  if (UB != getBitWidth() || LB != 0) {
    assert(UB <= getBitWidth() && UB > LB  && "Bad bit range!");
    if (UB != getBitWidth())  Operand = Operand.trunc(UB);
    if (LB != 0)              Operand = Operand.lshr(LB);
  }

  OS << (UB - LB) << "'h" << Operand.toString(16, false);
}

void VASTConstant::Profile(FoldingSetNodeID& ID) const {
  Int.Profile(ID);
}

VASTConstant VASTConstant::TrueValue(APInt(1, 1));
VASTConstant VASTConstant::FalseValue(APInt(1, 0));
VASTConstant *const VASTConstant::True = &VASTConstant::TrueValue;
VASTConstant *const VASTConstant::False = &VASTConstant::FalseValue;

//===----------------------------------------------------------------------===//

VASTSymbol::VASTSymbol(const char *Name, unsigned BitWidth)
  : VASTNamedValue(VASTNode::vastSymbol, Name, BitWidth) {}

void VASTSymbol::printAsOperandImpl(raw_ostream &OS,
                                    unsigned UB, unsigned LB) const {
  OS << getName();
  if (UB != getBitWidth() || LB != 0)
    OS << VASTValue::printBitRange(UB, LB, getBitWidth() > 1);
}

void VASTNamedValue::printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                        unsigned LB) const{
  OS << getName();
  if (UB) OS << VASTValue::printBitRange(UB, LB, getBitWidth() > 1);
}

void VASTNamedValue::PrintDecl(raw_ostream &OS, const Twine &Name,
                               unsigned BitWidth, bool declAsRegister,
                               const char *Terminator /* = ";" */) {
  if (declAsRegister)
    OS << "reg";
  else
    OS << "wire";

  OS << VASTValue::printBitRange(BitWidth, 0, false);

  OS << ' ' << Name;

  OS << Terminator;
}

void VASTNamedValue::printDecl(raw_ostream &OS, bool declAsRegister,
                               const char *Terminator) const {
  PrintDecl(OS, getName(), getBitWidth(), declAsRegister, Terminator);
}

//===----------------------------------------------------------------------===//
VASTUse::VASTUse(VASTNode *U, VASTValPtr V) : User(*U), V(V) {
  linkUseToUser();
}

void VASTUse::unlinkUseFromUser() {
  get()->removeUseFromList(this);
  V.setFromOpaqueValue(0);
}

void VASTUse::linkUseToUser() {
  if (VASTValue *Use = V.get()) {
    assert(Use != &User && "Unexpected cycle!");
    Use->addUseToList(this);
  }
}

bool VASTUse::operator==(const VASTValPtr RHS) const {
  return V == RHS;
}

//===----------------------------------------------------------------------===//
void VASTHandle::print(raw_ostream &OS) const {
  if (U.isInvalid()) OS << "<null>";
  else               OS << U.unwrap();
}

//===----------------------------------------------------------------------===//
ArrayRef<VASTUse> VASTOperandList::getOperands() const {
  return ArrayRef<VASTUse>(Operands, Size);
}

VASTOperandList::VASTOperandList(unsigned Size)
  : Operands(0), Size(Size) {
  if (Size)
    // Avoid calling the constructor of VASTUse by calling the generic new.
    Operands = reinterpret_cast<VASTUse*>(::operator new(sizeof(VASTUse) * Size));
}

VASTOperandList::~VASTOperandList() {
  if (Operands) ::operator delete(Operands);
}

void VASTOperandList::dropOperands() {
  for (VASTUse *I = Operands, *E = Operands + Size; I != E; ++I)
    if (I->unwrap()) I->unlinkUseFromUser();
}
