//=--- TimingNetlist.cpp - The Netlist for Delay Estimation -------*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the interface timing netlist.
//
//===----------------------------------------------------------------------===//

#include "TimingNetlist.h"

#include "shang/FUInfo.h"

#include "llvm/ADT/SetOperations.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "timing-netlist"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<double>
DelayFactor("vtm-delay-estimation-factor",
             cl::desc("Factor to be multipied to the estimated delay"),
             cl::init(1.0));

void TimingNetlist::annotateDelay(VASTSeqValue *Src, VASTValue *Dst,
                                  delay_type delay) {
  PathInfoTy::iterator path_end_at = PathInfo.find(Dst);
  assert(path_end_at != PathInfo.end() && "Path not exist!");
  SrcInfoTy::iterator path_start_from = path_end_at->second.find(Src);
  assert(path_start_from != path_end_at->second.end() && "Path not exist!");
  path_start_from->second = delay / VFUs::Period * DelayFactor;
}

TimingNetlist::delay_type
TimingNetlist::getDelay(VASTSeqValue *Src, VASTValue *Dst) const {
  PathInfoTy::const_iterator path_end_at = PathInfo.find(Dst);
  assert(path_end_at != PathInfo.end() && "Path not exist!");
  SrcInfoTy::const_iterator path_start_from = path_end_at->second.find(Src);
  assert(path_start_from != path_end_at->second.end() && "Path not exist!");
  return path_start_from->second;
}

void TimingNetlist::createDelayEntry(VASTValue *Dst, VASTSeqValue *Src) {
  assert(Src && Dst && "Bad pointer!");

  PathInfo[Dst][Src] = 0;
}

void TimingNetlist::createPathFromSrc(VASTValue *Dst, VASTValue *Src) {
  assert(Dst != Src && "Unexpected cycle!");

  // Forward the Src terminator of the path from SrcReg.
  PathInfoTy::iterator at = PathInfo.find(Src);

  // No need to worry about this path.
  if (at == PathInfo.end()) return;


  // Otherwise forward the source nodes reachable to SrcReg to DstReg.
  set_union(PathInfo[Dst], at->second);
}

