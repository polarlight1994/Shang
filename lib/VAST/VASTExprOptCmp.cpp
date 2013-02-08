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

#include "shang/Utilities.h"

#include "llvm/Support/ErrorHandling.h"
#define DEBUG_TYPE "vast-expr-opt-cmp"
#include "llvm/Support/Debug.h"

using namespace llvm;
