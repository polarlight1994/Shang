//===--- CarryChainOpt.cpp - CarryChain Related Optimization  ---*- C++ -*-===//
//
//                      The VAST HLS framework                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Carry Chain related optimization for addition,
// multiplication and comparisions
//
//===----------------------------------------------------------------------===//

#include "BitlevelOpt.h"

#include "vast/VASTModule.h"
#include "vast/VASTHandle.h"

#define DEBUG_TYPE "vast-carry-chain-opt"
#include "llvm/Support/Debug.h"

using namespace vast;

VASTValPtr DatapathBLO::optimizeCarryChain(VASTExpr::Opcode Opcode,
                                           MutableArrayRef<VASTValPtr>  Ops,
                                           unsigned BitWidth) {
  return Builder.buildExpr(Opcode, Ops, BitWidth);
}
