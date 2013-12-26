//===- DemandedOpt.cpp - Eliminate the bits are not used --------*- C++ -*-===//
//
//                      The VAST HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement DemandedBitsOpt class, it try to eliminate all known bits
// in the datapath.
//
//===----------------------------------------------------------------------===//
#include "BitlevelOpt.h"

#define DEBUG_TYPE "vast-demanded-opt"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace vast;

namespace {

}
