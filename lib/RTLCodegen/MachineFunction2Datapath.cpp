//===- MachineFunction2Datapath.cpp - MachineFunction <-> VAST --*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the classes which convert MachineFunction to VASTExprs and
// vice versa.
//
//===----------------------------------------------------------------------===//

#include "MachineFunction2Datapath.h"
#include "llvm/Target/TargetRegisterInfo.h"

using namespace llvm;

VASTValPtr DatapathBuilder::createCnd(MachineOperand Op) {
  // Is there an always true predicate?
  if (VInstrInfo::isAlwaysTruePred(Op)) return getBoolImmediate(true);

  bool isInverted = VInstrInfo::isPredicateInverted(Op);
  // Fix the bitwidth, the bitwidth of condition is always 1.
  VInstrInfo::setBitWidth(Op, 1);

  // Otherwise it must be some signal.
  VASTValPtr C = getAsOperand(Op);

  if (isInverted) C = buildNotExpr(C);

  return C;
}

VASTValPtr DatapathBuilder::buildDatapathExpr(MachineInstr *MI) {
  switch (MI->getOpcode()) {
  case VTM::VOpBitSlice:  return buildBitSlice(MI);
  case VTM::VOpBitCat:
    return buildBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpBitCat>);
  case VTM::VOpBitRepeat:
    return buildBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpBitRepeat>);

  case VTM::VOpAdd:     return buildAdd(MI);
  case VTM::VOpICmp:    return buildICmp(MI);

  case VTM::VOpSHL:
    return buildBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpShl>);
  case VTM::VOpSRA:
    return buildBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpSRA>);
  case VTM::VOpSRL:
    return buildBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpSRL>);

  case VTM::VOpMultLoHi:
  case VTM::VOpMult:
    return buildBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpMul>);

  case VTM::VOpSel:       return buildSel(MI);

  case VTM::VOpLUT:       return buildLUT(MI);

  case VTM::VOpXor:       return buildBinaryOp(MI, VASTExprBuilder::buildXor);
  case VTM::VOpAnd:
    return buildBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpAnd>);
  case VTM::VOpOr:        return buildBinaryOp(MI, VASTExprBuilder::buildOr);
  case VTM::VOpNot:       return buildInvert(MI);
  case VTM::VOpROr:       return buildReduceOr(MI);
  case VTM::VOpRAnd:      return buildUnaryOp(MI, VASTExpr::dpRAnd);
  case VTM::VOpRXor:      return buildUnaryOp(MI, VASTExpr::dpRXor);
  default:  assert(0 && "Unexpected opcode!");    break;
  }

  return VASTValPtr(0);
}

VASTValPtr DatapathBuilder::buildAdd(MachineInstr *MI) {
  return buildExpr(VASTExpr::dpAdd, getAsOperand(MI->getOperand(1)),
                                    getAsOperand(MI->getOperand(2)),
                                    getAsOperand(MI->getOperand(3)),
                   VInstrInfo::getBitWidth(MI->getOperand(0)));
}

VASTValPtr DatapathBuilder::buildICmp(MachineInstr *MI) {
  unsigned CndCode = MI->getOperand(3).getImm();
  if (CndCode == VFUs::CmpSigned)
    return buildBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpSCmp>);

  // else
  return buildBinaryOp(MI, VASTExprBuilder::buildExpr<VASTExpr::dpUCmp>);
}

VASTValPtr DatapathBuilder::buildInvert(MachineInstr *MI) {
  return buildNotExpr(getAsOperand(MI->getOperand(1)));
}

VASTValPtr DatapathBuilder::buildReduceOr(MachineInstr *MI) {
  // A | B .. | Z = ~(~A & ~B ... & ~Z).
  VASTValPtr V = buildNotExpr(getAsOperand(MI->getOperand(1)));
  V = buildNotExpr(buildExpr(VASTExpr::dpRAnd, V, 1));
  return V;
}

VASTValPtr DatapathBuilder::buildUnaryOp(MachineInstr *MI,
                                          VASTExpr::Opcode Opc) {
  return buildExpr(Opc, getAsOperand(MI->getOperand(1)),
                            VInstrInfo::getBitWidth(MI->getOperand(0)));
}

VASTValPtr DatapathBuilder::buildSel(MachineInstr *MI) {
  VASTValPtr Ops[] = { createCnd(MI->getOperand(1)),
                       getAsOperand(MI->getOperand(2)),
                       getAsOperand(MI->getOperand(3)) };

  return buildExpr(VASTExpr::dpSel, Ops,
                   VInstrInfo::getBitWidth(MI->getOperand(0)));
}

VASTValPtr DatapathBuilder::buildLUT(MachineInstr *MI) {
  unsigned SizeInBits = VInstrInfo::getBitWidth(MI->getOperand(0));

  SmallVector<VASTValPtr, 8> Operands;
  for (unsigned i = 4, e = MI->getNumOperands(); i < e; ++i)
    Operands.push_back(getAsOperand(MI->getOperand(i)));

  // The truth table goes last.
  Operands.push_back(getAsOperand(MI->getOperand(1)));

  return buildExpr(VASTExpr::dpLUT, Operands, SizeInBits);
}

VASTValPtr DatapathBuilder::buildBitSlice(MachineInstr *MI) {
  // Get the range of the bit slice, Note that the
  // bit at upper bound is excluded in VOpBitSlice
  unsigned UB = MI->getOperand(2).getImm(),
           LB = MI->getOperand(3).getImm();

  // RHS should be a register.
  MachineOperand &MO = MI->getOperand(1);
  return buildBitSliceExpr(getAsOperand(MO), UB, LB);
}

VASTValPtr DatapathBuilder::createAndIndexExpr(MachineInstr *MI,
                                               bool mayExisted) {
  unsigned RegNo = MI->getOperand(0).getReg();
  // Ignore the dead data-path.
  if (RegNo == 0) return 0;
  
  assert(TargetRegisterInfo::isVirtualRegister(RegNo)
         && "Expected virtual register!");
  VASTValPtr &Expr = Idx2Expr[RegNo];
  assert((!Expr || mayExisted) && "Expression had already been created!");

  if (Expr) return Expr;
  
  return (Expr = buildDatapathExpr(MI));
}

VASTValPtr DatapathBuilder::lookupExpr(unsigned RegNo) const {
   assert(TargetRegisterInfo::isVirtualRegister(RegNo)
          && "Expect virtual register!");
   RegIdxMapTy::const_iterator at = Idx2Expr.find(RegNo);
   return at == Idx2Expr.end() ? 0 : at->second;
 }

VASTValPtr DatapathBuilder::indexVASTExpr(unsigned RegNo, VASTValPtr V) {
  assert(TargetRegisterInfo::isVirtualRegister(RegNo)
    && "Expect physical register!");
  bool inserted = Idx2Expr.insert(std::make_pair(RegNo, V)).second;
  assert(inserted && "RegNum already indexed some value!");

  return V;
}

void DatapathBuilder::forgetIndexedExpr(unsigned RegNo) {
  Idx2Expr.erase(RegNo);
}
