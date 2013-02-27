//===- STGShortestPath.h - Shortest Path Distance between States -*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the STGShortestPath pass. The STGShortestPath compute the
// shortest path distance bewteen the states (VASTSlots) in the State-transition
// graph.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_STG_SHORTEST_PATH_H
#define VAST_STG_SHORTEST_PATH_H

#include "shang/VASTModulePass.h"

#include <map>

namespace llvm {
class VASTSlot;
class VASTModule;

class STGShortestPath : public VASTModulePass {
  std::map<std::pair<unsigned, unsigned>, unsigned> STPMatrix;
  typedef std::pair<unsigned, unsigned> Idx;
public:
  enum InfType {
    Inf = UINT16_MAX
  };

  static char ID;

  STGShortestPath();

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
  void print(raw_ostream &OS) const;

  unsigned getShortestPath(unsigned From, unsigned To) const;
};
}

#endif
