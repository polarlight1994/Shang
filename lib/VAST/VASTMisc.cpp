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
#include "shang/VASTHandle.h"
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
  APInt Operand = Int;
  if (UB != getBitWidth() || LB != 0) {
    assert(UB <= getBitWidth() && UB > LB  && "Bad bit range!");
    if (UB != getBitWidth())  Operand = Operand.trunc(UB);
    if (LB != 0)              Operand = Operand.lshr(LB);
  }

  OS << (UB - LB) << "'h" << Operand.toString(16, false);
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

void VASTNamedValue::printDecl(raw_ostream &OS, bool declAsRegister,
                               const char *Terminator) const {
  if (declAsRegister)
    OS << "reg";
  else
    OS << "wire";

  OS << VASTValue::printBitRange(getBitWidth(), 0, false);

  OS << ' ' << getName();

  OS << Terminator;

  return;
}

//===----------------------------------------------------------------------===//
VASTLLVMValue::VASTLLVMValue(Value *V, unsigned Size)
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

void VASTUse::PinUser() const {
  if (VASTWire *S = getAsLValue<VASTWire>())
    S->Pin();
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

VASTOperandList *VASTOperandList::GetDatapathOperandList(VASTNode *N) {
  if (VASTExpr *E = dyn_cast_or_null<VASTExpr>(N))
    return E;

  if (VASTWire *W = dyn_cast_or_null<VASTWire>(N))
    if (W->getDriver()) return W;

  return 0;
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
    if (I->unwrap()) I->unlinkUseFromUser();
}

//===----------------------------------------------------------------------===//
void VASTModule::gc() {
  // Clear up the dead VASTSeqValues.
  for (seqval_iterator I = seqval_begin(); I != seqval_end(); /*++I*/) {
    VASTSeqValue *V = I++;

    if (!V->use_empty() || V->getValType() != VASTSeqValue::Data) continue;;

    SmallVector<VASTSeqOp*, 4> DeadOps;
    for (VASTSeqValue::iterator I = V->begin(), E = V->end(); I != E; ++I) {
      VASTLatch U = *I;
      DeadOps.push_back(U.Op);
    }

    while (!DeadOps.empty()) {
      VASTSeqOp *Op = DeadOps.pop_back_val();
      Op->removeFromParent();
      eraseSeqOp(Op);
    }

    eraseSeqVal(V);
  }

  // Release the dead seqops, this happen when we fold the SeqOp through the
  // false paths.
  for (seqop_iterator I = seqop_begin(); I != seqop_end(); /*++I*/) {
    VASTSeqOp *Op = I++;
    if (Op->getPred() == VASTImmediate::False) {
      Op->removeFromParent();
      eraseSeqOp(Op);
    }
  }

  // At last clear up the dead VASTExprs.
  Datapath->gc();
}

void DatapathContainer::gc() {
  // Please note that recursivelyDeleteTriviallyDeadExprs will not invalid the
  // VASTExprs in the workllist while we are deleting other expressions. Because
  // we do not perform any replacement.
  std::vector<VASTExpr*> Worklist;
  typedef DatapathContainer::expr_iterator expr_iterator;
  for (expr_iterator I = expr_begin(); I != expr_end(); ++I)
    if (I->use_empty()) Worklist.push_back(I);

  while (!Worklist.empty()) {
    VASTExpr *E = Worklist.back();
    Worklist.pop_back();

    recursivelyDeleteTriviallyDeadExprs(E);
  }
}
