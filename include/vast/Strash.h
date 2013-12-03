//===-------- Strash.h - Structural Hash Table for Datapath Nodes ---------===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the interface of StrashTable, which calculate the ID of the
// nodes in datapath based on their "structure".
//
//===----------------------------------------------------------------------===//

#ifndef STRUCTRURAL_HASH_TABLE_H
#define STRUCTRURAL_HASH_TABLE_H

#include "vast/VASTNodeBases.h"
#include "vast/VASTModulePass.h"
#include <map>

namespace llvm {
// The structural hash table and the cached version.
struct Strash;
class CachedStrashTable : public VASTModulePass {
public:
  typedef std::map<VASTValPtr, unsigned> CacheTy;
private:
  CacheTy Cache;
  Strash *Table;
public:
  static char ID;

  CachedStrashTable();

  unsigned getOrCreateStrashID(VASTValPtr Ptr);
  unsigned getOrCreateStrashID(VASTSelector *Sel);

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
};

class CombPatterns;
// The combinational pattern analysis.
class CombPatternTable : public VASTModulePass {
public:
  typedef std::pair<VASTSelector*, VASTSelector*> LeafDelta;

  struct DeltaResult {
    bool IsAlwaysDifferent;
    bool IsAlwaysIdentical;
    SmallVector<LeafDelta, 8> Deltas;

    DeltaResult(bool IsAlwaysDifferent = false, bool IsAlwaysIdentical = false)
      : IsAlwaysDifferent(IsAlwaysDifferent),
        IsAlwaysIdentical(IsAlwaysIdentical) {}

    bool isUnintialized() const {
      return !IsAlwaysDifferent && !IsAlwaysIdentical && Deltas.empty();
    }

    typedef SmallVector<LeafDelta, 8>::const_iterator iterator;
    iterator begin() const { return Deltas.begin(); }
    iterator end() const { return Deltas.end(); }
  };
private:
  CombPatterns *Table;
  std::map<std::pair<VASTExpr*, VASTExpr*>, DeltaResult> CachedDeltas;
  static const DeltaResult AlwaysDifferent, AlwaysIdentical;
public:
  static char ID;

  CombPatternTable();

  unsigned getOrCreatePatternID(VASTValPtr Ptr);
  ArrayRef<VASTSelector*> getLeaves(VASTExpr *Expr) const;

  const DeltaResult &getLeavesDelta(VASTExpr *LHS, VASTExpr *RHS);

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
};
}
#endif
