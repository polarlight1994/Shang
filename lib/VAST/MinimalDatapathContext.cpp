//===- VASTModuleDatapathContext.cpp - Minimal Datapath Context -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Datapath builder context based on VASTModule.
//
//===----------------------------------------------------------------------===//
#include "MinimalDatapathContext.h"

using namespace llvm;

//===--------------------------------------------------------------------===//
// Implement the functions of EarlyDatapathBuilderContext.
VASTImmediate *
MinimalDatapathContext::getOrCreateImmediate(const APInt &Value) {
  return Datapath.getOrCreateImmediateImpl(Value);
}

VASTValPtr
MinimalDatapathContext::createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                                   unsigned UB, unsigned LB) {
  return Datapath.createExprImpl(Opc, Ops, UB, LB);;
}

VASTValPtr 
MinimalDatapathContext::getAsOperandImpl(Value *Op, bool GetAsInlineOperand) {
  unsigned NumBits = getValueSizeInBits(Op);

  if (ConstantInt *Int = dyn_cast<ConstantInt>(Op))
    return getOrCreateImmediate(Int->getValue());

  if (VASTValPtr V = lookupExpr(Op)) {
    // Try to inline the operand if user ask to.
    if (GetAsInlineOperand) V = V.getAsInlineOperand();
    return V;
  }

  // Else we need to create a leaf node for the expression tree.
  VASTLLVMValue *ValueOp = new (Datapath.getAllocator()) VASTLLVMValue(Op, NumBits);

  // Remember the newly create VASTLLVMValue, so that it will not be created
  // again.
  indexVASTExpr(Op, ValueOp);
  return ValueOp;
}

MinimalDatapathContext::MinimalDatapathContext(DatapathContainer &Datapath,
                                               DataLayout *TD)
  : DatapathBuilderContext(TD), Datapath(Datapath) {}
