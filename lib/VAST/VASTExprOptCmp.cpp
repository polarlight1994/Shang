//===- VASTExprOptCmp.cpp - Optimize the Integer Comparisons -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement optimizations on the Integer Comparisons.
//
//===----------------------------------------------------------------------===//

#include "VASTExprBuilder.h"

#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-expr-opt-cmp"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;
STATISTIC(ConstICmpLowered, "Number of Constant ICmp lowered");

VASTValPtr VASTExprBuilder::buildICmpExpr(VASTExpr::Opcode Opc,
                                          VASTValPtr LHS, VASTValPtr RHS) {

  VASTValPtr Ops[] = { LHS, RHS };
  return createExpr(Opc, Ops, 1, 0);
}
