//===--- VASTExprBuilder.cpp - Building Verilog AST Expressions -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
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
                                              unsigned UB, unsigned LB) {
  llvm_unreachable("reach Unimplemented function of VASTExprBuilderContext!");
  return 0;
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
                                                 unsigned UB, unsigned LB) {
  return Datapath.createExprImpl(Opc, Ops, UB, LB);
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
  return U.invert();
}

VASTValPtr VASTExprBuilder::buildBitCatExpr(ArrayRef<VASTValPtr> Ops,
                                            unsigned BitWidth) {
  return createExpr(VASTExpr::dpBitCat, Ops, BitWidth, 0);
}

VASTValPtr VASTExprBuilder::buildBitSliceExpr(VASTValPtr U, uint8_t UB,
                                              uint8_t LB) {
  assert(UB <= U->getBitWidth() && UB > LB && "Bad bit range!");
  VASTValPtr Ops[] = { U };
  return createExpr(VASTExpr::dpAssign, Ops, UB, LB);
}

VASTValPtr VASTExprBuilder::buildReduction(VASTExpr::Opcode Opc,VASTValPtr Op) {
  return createExpr(Opc, Op, 1, 0);
}

VASTValPtr
VASTExprBuilder::buildCommutativeExpr(VASTExpr::Opcode Opc,
                                      MutableArrayRef<VASTValPtr> Ops,
                                      unsigned BitWidth) {
  std::sort(Ops.begin(), Ops.end(), VASTValPtr::type_less);
  return createExpr(Opc, Ops, BitWidth, 0);
}

VASTValPtr VASTExprBuilder::buildBitRepeat(VASTValPtr Op, unsigned RepeatTimes){
  if (RepeatTimes == 1) return Op;

  return createExpr(VASTExpr::dpBitRepeat, Op, getConstant(RepeatTimes, 8),
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
  default: break;
  case VASTExpr::dpAdd:  return buildAddExpr(Ops, BitWidth);
  case VASTExpr::dpMul:  return buildMulExpr(Ops, BitWidth);
  case VASTExpr::dpAnd:  return buildAndExpr(Ops, BitWidth);
  case VASTExpr::dpBitCat: return buildBitCatExpr(Ops, BitWidth);
  case VASTExpr::dpShl:
  case VASTExpr::dpSRA:
  case VASTExpr::dpSRL:
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
  case VASTExpr::dpBitRepeat: {
    assert(Ops.size() == 2 && "Bad expression size!");
    VASTConstPtr C = cast<VASTConstPtr>(Ops[1]);
    unsigned Times = C.getZExtValue();
    assert(Times * Ops[0]->getBitWidth() == BitWidth && "Bitwidth not match!");
    return buildBitRepeat(Ops[0], Times);
  }
  case VASTExpr::dpKeep:
    assert(Ops.size() == 1 && "Unexpected more than 1 operands for reduction!");
    assert(BitWidth == Ops[0]->getBitWidth() && "Bad bitwidth!");
    return buildKeep(Ops[0]);
  }

  return createExpr(Opc, Ops, BitWidth, 0);
}

VASTValPtr VASTExprBuilder::buildExpr(VASTExpr::Opcode Opc, VASTValPtr Op,
                                      unsigned BitWidth) {
  switch (Opc) {
  default: break;
  case VASTExpr::dpRAnd:
  case VASTExpr::dpRXor:
    assert(BitWidth == 1 && "Bitwidth of reduction should be 1!");
    return buildReduction(Opc, Op);
  case VASTExpr::dpKeep:
    assert(BitWidth == Op->getBitWidth() && "Bad bitwidth!");
    return buildKeep(Op);
  }

  VASTValPtr Ops[] = { Op };
  return createExpr(Opc, Ops, BitWidth, 0);
}

VASTValPtr VASTExprBuilder::buildKeep(VASTValPtr V) {
  VASTExprPtr Expr = dyn_cast<VASTExprPtr>(V);

  // Only keep expressions!
  if (!Expr)
    return V;
  
  switch (Expr->getOpcode()) {
  default:break;
    // No need to keep twice!
  case VASTExpr::dpKeep:
    return V;
  case VASTExpr::dpBitRepeat:
    return buildExpr(VASTExpr::dpBitRepeat, buildKeep(Expr.getOperand(0)),
                     Expr->getOperand(1), Expr->getBitWidth());
  case VASTExpr::dpBitCat: {
    typedef VASTExpr::op_iterator iterator;
    SmallVector<VASTValPtr, 4> Ops;
    for (iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I)
      Ops.push_back(buildKeep(*I));
    return buildBitCatExpr(Ops, Expr->getBitWidth());
  }
  }

  VASTValPtr Ops[] = { V };
  return createExpr(VASTExpr::dpKeep, Ops, V->getBitWidth(), 0);
}

VASTValPtr VASTExprBuilder::padHeadOrTail(VASTValPtr V, unsigned BitWidth,
                                          bool ByOnes, bool PadTail) {
  assert(BitWidth >= V->getBitWidth() && "Bad bitwidth!");
  unsigned ZeroBits = BitWidth - V->getBitWidth();

  if (ZeroBits == 0) return V;

  VASTValPtr Pader =
    Context.getConstant(ByOnes ? ~UINT64_C(0) : UINT64_C(0), ZeroBits);

  VASTValPtr Hi = PadTail ? V : Pader, Lo = PadTail ? Pader : V;

  VASTValPtr Ops[] = { Hi, Lo};
  return buildBitCatExpr(Ops, BitWidth);
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
    RHS = buildBitSliceExpr(RHS, RHSMaxSize, 0);

  VASTValPtr Ops[] = { LHS, RHS }; 
  return createExpr(Opc, Ops, BitWidth, 0);
}

VASTValPtr VASTExprBuilder::buildZExtExpr(VASTValPtr V, unsigned DstBitWidth) {
  assert(DstBitWidth > V->getBitWidth() && "Unexpected DstBitWidth!");
  return padHigherBits(V, DstBitWidth, false);
}

VASTValPtr VASTExprBuilder::buildSExtExpr(VASTValPtr V, unsigned DstBitWidth) {
  assert(DstBitWidth > V->getBitWidth() && "Unexpected DstBitWidth!");
  unsigned NumExtendBits = DstBitWidth - V->getBitWidth();
  VASTValPtr SignBit = getSignBit(V);

  VASTValPtr ExtendBits = buildExpr(VASTExpr::dpBitRepeat, SignBit,
                                    getConstant(NumExtendBits, 8),
                                    NumExtendBits);
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
  if (Carry)
    NewOps.push_back(Carry);

  return createExpr(VASTExpr::dpAdd, NewOps, BitWidth, 0);
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
  return createExpr(Opc, Ops, 1, 0);
}
