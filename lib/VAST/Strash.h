//===-------- Strash.h - Structural Hash Table for Datapath Nodes ---------===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#ifndef STRUCTRURAL_HASH_TABLE_H
#define STRUCTRURAL_HASH_TABLE_H

#include "llvm/Pass.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/Allocator.h"

namespace llvm {
class VASTValue;
template<typename T> struct PtrInvPair;
typedef PtrInvPair<VASTValue> VASTValPtr;
class VASTExpr;

class StrashTable : public ImmutablePass {
  struct Node : public FoldingSetNode {
    /// FastID - A reference to an Interned FoldingSetNodeID for this node.
    /// The StrashTable's BumpPtrAllocator holds the data.
    FoldingSetNodeIDRef FastID;

    unsigned ID;

    Node(const FoldingSetNodeIDRef IDRef, unsigned ID)
      : FastID(IDRef), ID(ID) {}

    operator unsigned() const { return ID; }
  };
  friend struct FoldingSetTrait<StrashTable::Node>;

  unsigned LastID;
  FoldingSet<Node> Set;
  BumpPtrAllocator Allocator;

  void calculateLeafID(VASTValue *Ptr, FoldingSetNodeID &ID);
  void calculateExprID(VASTExpr *Expr, FoldingSetNodeID &ID);
  void calculateID(VASTValue *Ptr, FoldingSetNodeID &ID);
  void calculateID(VASTValPtr Ptr, FoldingSetNodeID &ID);

public:
  static char ID;

  StrashTable() : ImmutablePass(ID), LastID(0) {}

  unsigned getOrInsertNode(VASTValPtr Ptr);
};

// Specialize FoldingSetTrait for StrashNode to avoid needing to compute
// temporary FoldingSetNodeID values.
template<> struct FoldingSetTrait<StrashTable::Node> {
  static void Profile(const StrashTable::Node &X, FoldingSetNodeID& ID) {
    ID = X.FastID;
  }
  static bool Equals(const StrashTable::Node &X, const FoldingSetNodeID &ID,
                     unsigned IDHash, FoldingSetNodeID &TempID) {
    return ID == X.FastID;
  }

  static
  unsigned ComputeHash(const StrashTable::Node &X, FoldingSetNodeID &TempID) {
    return X.FastID.ComputeHash();
  }
};
}
#endif
