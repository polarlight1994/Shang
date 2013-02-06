//===------------- VLang.h - Verilog HDL writing engine ---------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VLang class, with provide funtions to complete
// common Verilog HDL writing task.
//
//===----------------------------------------------------------------------===//
#include "shang/VASTDatapathNodes.h"
#include "shang/VASTModule.h"
#include "shang/Utilities.h"

#define DEBUG_TYPE "vast-misc"
#include "llvm/Support/Debug.h"

#include <sstream>

using namespace llvm;
//===----------------------------------------------------------------------===//
// Value and type printing
std::string
VASTImmediate::buildLiteral(uint64_t Value, unsigned bitwidth, bool isMinValue) {
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

void VASTImmediate::printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                       unsigned LB) const {
  assert(UB == getBitWidth() && LB == 0 && "Cannot print bitslice of Expr!");
  OS << getBitWidth() << "'h" << Int.toString(16, false);
}

void VASTImmediate::Profile(FoldingSetNodeID& ID) const {
  Int.Profile(ID);
}

VASTImmediate *VASTImmediate::True = 0;
VASTImmediate *VASTImmediate::False = 0;

//===----------------------------------------------------------------------===//

VASTSymbol::VASTSymbol(const char *Name, unsigned BitWidth)
  : VASTNamedValue(VASTNode::vastSymbol, Name, BitWidth) {}

void VASTNamedValue::printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                        unsigned LB) const{
  OS << getName();
  if (UB) OS << VASTValue::printBitRange(UB, LB, getBitWidth() > 1);
}

//===----------------------------------------------------------------------===//
VASTLLVMValue::VASTLLVMValue(const Value *V, unsigned Size)
  : VASTValue(vastLLVMValue, Size)
{
  Contents.LLVMValue = V;
}

void VASTLLVMValue::printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                       unsigned LB) const {
  if (isa<GlobalVariable>(getValue())) {
    assert(LB == 0 && UB == getBitWidth() && "Cannot print bitslice of GV!");
    OS << "(`gv" << ShangMangle(getValue()->getName()) << ')';
    return;
  }

  OS << "LLVM IR " << *getValue() << "<Dont know how to print the value!>";
}

//===----------------------------------------------------------------------===//
VASTSignal::VASTSignal(VASTTypes DeclType, const char *Name, unsigned BitWidth)
  : VASTNamedValue(DeclType, Name, BitWidth) {}

void VASTSignal::anchor() const {}

VASTUse::VASTUse(VASTNode *U, VASTValPtr V) : User(*U), V(V) {
  linkUseToUser();
}

void VASTUse::unlinkUseFromUser() {
  get()->removeUseFromList(this);
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

void VASTUse::PinUser() const {
  if (VASTWire *S = getAsLValue<VASTWire>())
    S->Pin();
}

ArrayRef<VASTUse> VASTOperandList::getOperands() const {
  return ArrayRef<VASTUse>(Operands, Size);
}


VASTOperandList *VASTOperandList::GetDatapathOperandList(VASTNode *N) {
  if (VASTExpr *E = dyn_cast_or_null<VASTExpr>(N))
    return E;

  return dyn_cast_or_null<VASTWire>(N);
}

VASTOperandList::VASTOperandList(unsigned Size)
  : Operands(0), Size(Size) {
  if (Size)
    Operands = reinterpret_cast<VASTUse*>(::operator new(sizeof(VASTUse) * Size));
}

VASTOperandList::~VASTOperandList() {
  if (Operands) ::operator delete(Operands);
}

void VASTOperandList::dropOperands() {
  for (VASTUse *I = Operands, *E = Operands + Size; I != E; ++I)
    if (I->get()) I->unlinkUseFromUser();
}

//===----------------------------------------------------------------------===//
