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

#include "vast/VASTModulePass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SparseBitVector.h"

namespace llvm {
class VASTSlot;
class VASTCtrlRgn;
struct ShortestPathImpl;
struct LongestPathImpl;

class STGDistanceBase {
protected:
  typedef std::pair<unsigned, unsigned> Idx;
  std::map<unsigned, std::map<unsigned, unsigned> > DistanceMatrix;

  void initialize(VASTCtrlRgn &R);

  STGDistanceBase() {}

public:
  bool empty() const { return DistanceMatrix.empty(); }

  unsigned getDistance(unsigned From, unsigned To) const;

  void print(raw_ostream &OS, VASTCtrlRgn &R) const;

  static STGDistanceBase *CalculateShortestPathDistance(VASTCtrlRgn &R);
};

class STGDistances : public VASTModulePass {
  ShortestPathImpl *SPImpl;
  // Setup the landing slot map.
  std::map<unsigned, SparseBitVector<> > LandingMap;

  VASTModule *VM;

  unsigned getIntervalFromDef(const VASTLatch &L, unsigned ReadSlotNum) const;
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

  unsigned getIntervalFromDef(const VASTSeqValue *V, VASTSlot *ReadSlot) const;
  unsigned getIntervalFromDef(const VASTSelector *Sel, VASTSlot *ReadSlot) const;

  template<typename T>
  unsigned getIntervalFromDef(const T *V, ArrayRef<VASTSlot*> ReadSlots) const {
    unsigned PathInterval = STGDistances::Inf;
    typedef ArrayRef<VASTSlot*>::iterator iterator;
    for (iterator I = ReadSlots.begin(), E = ReadSlots.end(); I != E; ++I)
      PathInterval = std::min(PathInterval, getIntervalFromDef(V, *I));

    return PathInterval;
  }
};
}

#endif
