//===---- OverlappedSlots.h - Identify the overlapped slots ------*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the OverlapSlots analysis. The analysis identifies the
// non-mutually exclusive slots with overlapped timeframes. This can happened
// after we relax the control dependencies from/to the boundaries of the basic
// blocks.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_OVERLAP_SLOTS_H
#define VAST_OVERLAP_SLOTS_H

#include "shang/VASTModulePass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class VASTSlot;
class VASTModule;
class STGShortestPath;

class OverlappedSlots : public VASTModulePass {
  STGShortestPath *STP;
  DenseMap<unsigned, SparseBitVector<> > Overlappeds;

  void buildOverlappedMap(VASTSlot *S);
  void buildOverlappedMap(VASTSlot *S, ArrayRef<VASTSlot*> StraightFlow);
public:
  static const unsigned Inf;

  static char ID;

  OverlappedSlots();

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
  void print(raw_ostream &OS) const;
};

}

#endif
