//===--- VASTExprBuilder.cpp - Building Verilog AST Expressions -*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Verilog AST Expressions building and optimizating
// functions.
//
//===----------------------------------------------------------------------===//

#include "vast/VASTExprBuilder.h"
#include "vast/Utilities.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"
#define DEBUG_TYPE "vast-expr-builder"
#include "llvm/Support/Debug.h"

using namespace llvm;

//===--------------------------------------------------------------------===//
VASTConstant *VASTExprBuilderContext::getConstant(const APInt &Value) {
  llvm_unreachable("reach Unimplemented function of VASTExprBuilderContext!");
  return 0;
}

VASTValPtr VASTExprBuilderContext::createExpr(VASTExpr::Opcode Opc,
                                              ArrayRef<VASTValPtr> Ops,
                                              unsigned BitWidth) {
  llvm_unreachable("reach Unimplemented function of VASTExprBuilderContext!");
  return None;
}

VASTValPtr VASTExprBuilderContext::createBitExtract(VASTValPtr Op,
                                                  unsigned UB, unsigned LB) {
  llvm_unreachable("reach Unimplemented function of VASTExprBuilderContext!");
  return None;
}

VASTValPtr VASTExprBuilderContext::createROMLookUp(VASTValPtr Addr,
                                                   VASTMemoryBank *Bank,
                                                   unsigned BitWidth) {
  llvm_unreachable("reach Unimplemented function of VASTExprBuilderContext!");
  return None;
}

VASTValPtr VASTExprBuilderContext::createLUT(ArrayRef<VASTValPtr> Ops,
                                             unsigned Bitwidth, StringRef SOP) {
  llvm_unreachable("reach Unimplemented function of VASTExprBuilderContext!");
  return None;
}

void VASTExprBuilderContext::deleteContenxt(VASTValue *V) {
}

void VASTExprBuilderContext::replaceAllUseWith(VASTValPtr From, VASTValPtr To) {
  llvm_unreachable("Function not implemented!");
}

//===--------------------------------------------------------------------===//

MinimalExprBuilderContext::MinimalExprBuilderContext(DatapathContainer &DP)
  : Datapath(DP) {
  DP.pushContext(this);
}

VASTConstant *
MinimalExprBuilderContext::getConstant(const APInt &Value) {
  return Datapath.getConstantImpl(Value);
}

VASTValPtr MinimalExprBuilderContext::createExpr(VASTExpr::Opcode Opc,
                                                 ArrayRef<VASTValPtr> Ops,
                                                 unsigned Bitwidth) {
  return Datapath.createExprImpl(Opc, Ops, Bitwidth);
}

VASTValPtr MinimalExprBuilderContext::createBitExtract(VASTValPtr Op,
                                                     unsigned UB, unsigned LB) {
  return Datapath.createBitExtractImpl(Op, UB, LB);
}

VASTValPtr MinimalExprBuilderContext::createROMLookUp(VASTValPtr Addr,
                                                      VASTMemoryBank *Bank,
                                                      unsigned BitWidth) {
  return Datapath.createROMLookUpImpl(Addr, Bank, BitWidth);
}


VASTValPtr MinimalExprBuilderContext::createLUT(ArrayRef<VASTValPtr> Ops,
                                                unsigned Bitwidth, StringRef SOP) {
  return Datapath.createLUTImpl(Ops, Bitwidth, SOP);
}

void MinimalExprBuilderContext::replaceAllUseWith(VASTValPtr From,
                                                  VASTValPtr To) {
  Datapath.replaceAllUseWithImpl(From, To);
}

MinimalExprBuilderContext::~MinimalExprBuilderContext() {
  Datapath.popContext(this);
  Datapath.gc();
}

VASTValPtr VASTExprBuilder::buildNotExpr(VASTValPtr U) {
  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(U))
    return getConstant(~C.getAPInt());

  return U.invert();
}

VASTValPtr VASTExprBuilder::buildBitMask(VASTValPtr Op, APInt Mask) {
  unsigned BitWidth = Op->getBitWidth();
  assert(BitWidth == Mask.getBitWidth() && "Bitwidth of mask dosen't match!");
  VASTValPtr Ops[] = { Op, getConstant(Mask) };
  return Context.createExpr(VASTExpr::dpBitMask, Ops, BitWidth);
}

VASTValPtr VASTExprBuilder::buildBitCatExpr(ArrayRef<VASTValPtr> Ops,
                                            unsigned BitWidth) {
  return Context.createExpr(VASTExpr::dpBitCat, Ops, BitWidth);
}

VASTValPtr VASTExprBuilder::buildBitExtractExpr(VASTValPtr U, unsigned UB,
                                                unsigned LB) {
  assert(UB <= U->getBitWidth() && UB > LB && "Bad bit range!");
  return Context.createBitExtract(U, UB, LB);
}

VASTValPtr VASTExprBuilder::buildReduction(VASTExpr::Opcode Opc,VASTValPtr Op) {
  return Context.createExpr(Opc, Op, 1);
}

VASTValPtr
VASTExprBuilder::buildCommutativeExpr(VASTExpr::Opcode Opc,
                                      MutableArrayRef<VASTValPtr> Ops,
                                      unsigned BitWidth) {
  std::sort(Ops.begin(), Ops.end(), VASTValPtr::type_less);
  return Context.createExpr(Opc, Ops, BitWidth);
}

VASTValPtr VASTExprBuilder::buildBitRepeat(VASTValPtr Op, unsigned RepeatTimes){
  if (RepeatTimes == 1)
    return Op;

  return Context.createExpr(VASTExpr::dpBitRepeat, Op,
                            RepeatTimes * Op->getBitWidth());
}

VASTValPtr VASTExprBuilder::buildSelExpr(VASTValPtr Cnd, VASTValPtr TrueV,
                                         VASTValPtr FalseV, unsigned BitWidth) {
  assert(Cnd->getBitWidth() == 1 && "Bad condition width!");
  assert(TrueV->getBitWidth() == FalseV->getBitWidth()
         && TrueV->getBitWidth() == BitWidth && "Bad bitwidth!");

  if (VASTConstPtr C = dyn_cast<VASTConstPtr>(Cnd))
    return C.getBoolValue() ? TrueV : FalseV;

  Cnd = buildBitRepeat(Cnd, BitWidth);
  VASTValPtr V = buildOrExpr(buildAndExpr(Cnd, TrueV, BitWidth),
                             buildAndExpr(buildNotExpr(Cnd), FalseV, BitWidth),
                             BitWidth);

  return V;
}

VASTValPtr VASTExprBuilder::buildExpr(VASTExpr::Opcode Opc, VASTValPtr LHS,
                                      VASTValPtr RHS, unsigned BitWidth) {
  VASTValPtr Ops[] = { LHS, RHS };
  return buildExpr(Opc, Ops, BitWidth);
}

VASTValPtr VASTExprBuilder::buildExpr(VASTExpr::Opcode Opc, VASTValPtr Op0,
                                       VASTValPtr Op1, VASTValPtr Op2,
                                       unsigned BitWidth) {
  VASTValPtr Ops[] = { Op0, Op1, Op2 };
  return buildExpr(Opc, Ops, BitWidth);
}

VASTValPtr VASTExprBuilder::buildExpr(VASTExpr::Opcode Opc,
                                      ArrayRef<VASTValPtr> Ops,
                                      unsigned BitWidth) {
  switch (Opc) {
  // Directly create the loop up table.
  case VASTExpr::dpAdd:  return buildAddExpr(Ops, BitWidth);
  case VASTExpr::dpMul:  return buildMulExpr(Ops, BitWidth);
  case VASTExpr::dpAnd:  return buildAndExpr(Ops, BitWidth);
  case VASTExpr::dpBitCat: return buildBitCatExpr(Ops, BitWidth);
  case VASTExpr::dpBitMask:
    assert(Ops.size() == 2 && "Incorrect operand number!");
    return buildBitMask(Ops[0], cast<VASTConstPtr>(Ops[1]).getAPInt());
  case VASTExpr::dpShl:
  case VASTExpr::dpAshr:
  case VASTExpr::dpLshr:
    assert(Ops.size() == 2 && "Bad Operand input!");
    return buildShiftExpr(Opc, Ops[0], Ops[1], BitWidth);
  case VASTExpr::dpSGT:
  case VASTExpr::dpUGT:
    assert(Ops.size() == 2 && "Bad Operand input!");
    assert(BitWidth == 1 && "Bitwidth of ICmp should be 1!");
    return buildICmpExpr(Opc, Ops[0], Ops[1]);
  case VASTExpr::dpRAnd:
  case VASTExpr::dpRXor:
    assert(Ops.size() == 1 && "Unexpected more than 1 operands for reduction!");
    assert(BitWidth == 1 && "Bitwidth of reduction should be 1!");
    return buildReduction(Opc, Ops[0]);
  case VASTExpr::dpKeep:
    assert(Ops.size() == 1 && "Unexpected more than 1 operands for reduction!");
    assert(BitWidth == Ops[0]->getBitWidth() && "Bad bitwidth!");
    return buildKeep(Ops[0]);
  default:
    llvm_unreachable("Unexpected opcode!");
    break;
  }

  return None;
}

VASTValPtr VASTExprBuilder::buildExpr(VASTExpr::Opcode Opc, VASTValPtr Op,
                                      unsigned BitWidth) {
  switch (Opc) {
  case VASTExpr::dpRAnd:
  case VASTExpr::dpRXor:
    assert(BitWidth == 1 && "Bitwidth of reduction should be 1!");
    return buildReduction(Opc, Op);
  case VASTExpr::dpKeep:
    assert(BitWidth == Op->getBitWidth() && "Bad bitwidth!");
    return buildKeep(Op);
  default:
    llvm_unreachable("Unexpected opcode!");
    break;
  }

  return None;
}

VASTValPtr VASTExprBuilder::copyExpr(VASTExpr *Expr, ArrayRef<VASTValPtr> Ops) {
  VASTExpr::Opcode Opcode = Expr->getOpcode();
  switch (Opcode) {
  default: break;
  case VASTExpr::dpBitExtract:
    assert(Ops.size() == 1 && "Wrong operand number!");
    return buildBitExtractExpr(Ops[0], Expr->getUB(), Expr->getLB());
  case VASTExpr::dpBitRepeat:
    assert(Ops.size() == 1 && "Wrong operand number!");
    return buildBitRepeat(Ops[0], Expr->getRepeatTimes());
  case VASTExpr::dpLUT:
    return buildLUTExpr(Ops, Expr->getBitWidth(), Expr->getLUT());
  }

  return buildExpr(Expr->getOpcode(), Ops, Expr->getBitWidth());
}

VASTValPtr VASTExprBuilder::buildKeep(VASTValPtr V) {
  VASTExpr *Expr = dyn_cast<VASTExpr>(V.get());

  // Only keep expressions!
  if (!Expr)
    return V;

  bool IsInverted = V.isInverted();
  
  switch (Expr->getOpcode()) {
  default:break;
    // No need to keep twice!
  case VASTExpr::dpKeep:
    return V;
  case VASTExpr::dpBitRepeat:
    return buildBitRepeat(buildKeep(Expr->getOperand(0)).invert(IsInverted),
                          Expr->getRepeatTimes());
  case VASTExpr::dpBitCat: {
    typedef VASTExpr::op_iterator iterator;
    SmallVector<VASTValPtr, 4> Ops;
    for (iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I)
      Ops.push_back(buildKeep(*I).invert(IsInverted));
    return buildBitCatExpr(Ops, Expr->getBitWidth());
  }
  }

  VASTValPtr Ops[] = { V.get() };
  VASTValPtr K = Context.createExpr(VASTExpr::dpKeep, Ops, V->getBitWidth());
  return K.invert(IsInverted);
}

VASTValPtr VASTExprBuilder::buildROMLookUp(VASTValPtr Addr, VASTMemoryBank *Bank,
                                           unsigned Bitwidth) {
  return Context.createROMLookUp(Addr, Bank, Bitwidth);
}

VASTValPtr VASTExprBuilder::buildLUTExpr(ArrayRef<VASTValPtr> Ops, unsigned Bitwidth,
                                         StringRef SOP) {
  return Context.createLUT(Ops, Bitwidth, SOP);
}

VASTValPtr VASTExprBuilder::buildOrExpr(ArrayRef<VASTValPtr> Ops,
                                        unsigned BitWidth) {
  if (Ops.size() == 1) return Ops[0];

  assert (Ops.size() > 1 && "There should be more than one operand!!");

  SmallVector<VASTValPtr, 4> NotExprs;
  // Build the operands of Or operation into not Expr.
  for (unsigned i = 0; i < Ops.size(); ++i) {
    VASTValPtr V = buildNotExpr(Ops[i]);
    NotExprs.push_back(V);
  }

  // Build Or operation with the And Inverter Graph (AIG).
  return buildNotExpr(buildAndExpr(NotExprs, BitWidth));
}

VASTValPtr VASTExprBuilder::buildXorExpr(ArrayRef<VASTValPtr> Ops,
                                         unsigned BitWidth) {
  assert (Ops.size() == 2 && "There should be more than one operand!!");

  // Build the Xor Expr with the And Inverter Graph (AIG).
  return buildAndExpr(buildOrExpr(Ops, BitWidth),
                      buildNotExpr(buildAndExpr(Ops, BitWidth)),
                      BitWidth);
}

VASTValPtr VASTExprBuilder::buildShiftExpr(VASTExpr::Opcode Opc, 
                                           VASTValPtr LHS, 
                                           VASTValPtr RHS, 
                                           unsigned BitWidth) {
  // Limit the shift amount so keep the behavior of the hardware the same as
  // the corresponding software.
  unsigned RHSMaxSize = Log2_32_Ceil(LHS->getBitWidth());
  if (RHS->getBitWidth() > RHSMaxSize)
    RHS = buildBitExtractExpr(RHS, RHSMaxSize, 0);

  VASTValPtr Ops[] = { LHS, RHS };
  return Context.createExpr(Opc, Ops, BitWidth);
}

VASTValPtr VASTExprBuilder::buildZExtExpr(VASTValPtr V, unsigned DstBitWidth) {
  assert(DstBitWidth > V->getBitWidth() && "Unexpected DstBitWidth!");
  unsigned ZeroBits = DstBitWidth - V->getBitWidth();

  VASTValPtr Zeros = Context.getConstant( UINT64_C(0), ZeroBits);
  VASTValPtr Ops[] = { Zeros, V };
  return buildBitCatExpr(Ops, DstBitWidth);
}

VASTValPtr VASTExprBuilder::buildSExtExpr(VASTValPtr V, unsigned DstBitWidth) {
  assert(DstBitWidth > V->getBitWidth() && "Unexpected DstBitWidth!");
  unsigned NumExtendBits = DstBitWidth - V->getBitWidth();
  VASTValPtr SignBit = getSignBit(V);

  VASTValPtr ExtendBits = buildBitRepeat(SignBit, NumExtendBits);
  VASTValPtr Ops[] = { ExtendBits, V };
  return buildBitCatExpr(Ops, DstBitWidth);
}

VASTValPtr VASTExprBuilder::buildMulExpr(ArrayRef<VASTValPtr> Ops,
                                         unsigned BitWidth) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1)
    return Ops[0];

  SmallVector<VASTValPtr, 8> NewOps(Ops.begin(), Ops.end());
  return buildCommutativeExpr(VASTExpr::dpMul, NewOps, BitWidth);
}

VASTValPtr VASTExprBuilder::buildNegative(VASTValPtr Op) {
  return buildAddExpr(buildNotExpr(Op), getConstant(1, Op->getBitWidth()),
                      Op->getBitWidth());
}

VASTValPtr VASTExprBuilder::buildAddExpr(ArrayRef<VASTValPtr> Ops,
                                         unsigned BitWidth) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1)
    return Ops[0];

  // Make sure the carry bit located in the last operand in the operand list.
  SmallVector<VASTValPtr, 8> NewOps;
  VASTValPtr Carry = None;
  for (unsigned i = 0; i < Ops.size(); ++i) {
    if (Carry == None && Ops[i]->getBitWidth() == 1) {
      Carry = Ops[i];
      continue;
    }

    NewOps.push_back(Ops[i]);
  }

  std::sort(NewOps.begin(), NewOps.end(), VASTValPtr::type_less);
  if (Carry != None)
    NewOps.push_back(Carry);

  return Context.createExpr(VASTExpr::dpAdd, NewOps, BitWidth);
}

VASTValPtr VASTExprBuilder::buildAndExpr(ArrayRef<VASTValPtr> Ops,
                                         unsigned BitWidth) {
  // Handle the trivial case trivially.
  if (Ops.size() == 1)
    return Ops[0];

  SmallVector<VASTValPtr, 8> NewOps(Ops.begin(), Ops.end());
  return buildCommutativeExpr(VASTExpr::dpAnd, NewOps, BitWidth);
}

VASTValPtr VASTExprBuilder::buildICmpOrEqExpr(VASTExpr::Opcode Opc,
                                              VASTValPtr LHS, VASTValPtr RHS) {
  return buildOrExpr(buildICmpExpr(Opc, LHS, RHS), buildEQ(LHS, RHS), 1);
}
VASTValPtr VASTExprBuilder::buildICmpExpr(VASTExpr::Opcode Opc,
                                          VASTValPtr LHS, VASTValPtr RHS) {
  assert(RHS->getBitWidth() == LHS->getBitWidth() && "Bad icmp bitwidth!");
  VASTValPtr Ops[] = { LHS, RHS };
  return Context.createExpr(Opc, Ops, 1);
}
