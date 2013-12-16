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
MinimalDatapathContext::getAsOperandImpl(Value *Op) {
  if (ConstantInt *Int = dyn_cast<ConstantInt>(Op))
    return getOrCreateImmediate(Int->getValue());

  if (VASTValPtr V = lookupExpr(Op))
    return V;

  // Else we need to create a leaf node for the expression tree.
  llvm_unreachable("Cannot create VASTValPtr for Value!");
  return None;
}

void MinimalDatapathContext::replaceAllUseWith(VASTValPtr From, VASTValPtr To) {
  Datapath.replaceAllUseWithImpl(From, To);
}

MinimalDatapathContext::MinimalDatapathContext(DatapathContainer &Datapath,
                                               DataLayout *TD)
  : DatapathBuilderContext(TD), Datapath(Datapath) {
  Datapath.pushContext(this);
}

MinimalDatapathContext::~MinimalDatapathContext() {
  Datapath.popContext(this);
  // Free all dead VASTExprs.
  Datapath.gc();
}
