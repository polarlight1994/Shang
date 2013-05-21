//===- STGDistances.h - Calculate the distances in the STG -*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the STGDistances pass. The STGDistances compute the
// shortest path distance bewteen the states (VASTSlots) in the State-transition
// graph.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_STG_SHORTEST_PATH_H
#define VAST_STG_SHORTEST_PATH_H

#include "shang/VASTModulePass.h"

#include "llvm/ADT/DenseMap.h"

namespace llvm {
class VASTSlot;
class VASTModule;
struct ShortestPathImpl;

class STGDistances : public VASTModulePass {
  ShortestPathImpl *STPImpl;
  VASTModule *VM;
public:
  static const unsigned Inf;

  static char ID;

  STGDistances();
  ~STGDistances() { releaseMemory(); }

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
  void print(raw_ostream &OS) const;

  unsigned getShortestPath(unsigned From, unsigned To) const;
};
}

#endif
