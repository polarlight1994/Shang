//===---- STGDistances.h - Calculate the distances in the STG ----*-C++ -*-===//
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
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SparseBitVector.h"

namespace llvm {
class VASTSlot;
class VASTModule;
struct ShortestPathImpl;
struct LongestPathImpl;

class STGDistanceBase {
protected:
  typedef std::pair<unsigned, unsigned> Idx;
  DenseMap<unsigned, DenseMap<unsigned, unsigned> > DistanceMatrix;

  void initialize(VASTModule &VM);

  STGDistanceBase() {}

public:
  bool empty() const { return DistanceMatrix.empty(); }

  unsigned getDistance(unsigned From, unsigned To) const;

  void print(raw_ostream &OS, VASTModule &VM) const;

  static STGDistanceBase CalculateShortestPathDistance(VASTModule &VM);
};

class STGDistances : public VASTModulePass {
  ShortestPathImpl *SPImpl;
  // Setup the landing slot map.
  std::map<unsigned, SparseBitVector<> > LandingMap;

  VASTModule *VM;
public:
  static const unsigned Inf = UINT16_MAX;

  static char ID;

  STGDistances();
  ~STGDistances() { releaseMemory(); }

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
  void print(raw_ostream &OS) const;

  unsigned getShortestPath(unsigned From, unsigned To) const;

  unsigned getIntervalFromDef(const VASTSeqValue *V, VASTSlot *ReadSlot) const;
  unsigned getIntervalFromDef(const VASTSeqValue *V,
                              ArrayRef<VASTSlot*> ReadSlots) const;
};
}

#endif
