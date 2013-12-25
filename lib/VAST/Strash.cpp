//===------ Strash.cpp - Structural Hash Table for Datapath Nodes ---------===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the interface of StrashTable, which calculate the ID of
// the nodes in datapath based on their "structure".
//
//===----------------------------------------------------------------------===//

#include "vast/Passes.h"
#include "vast/VASTDatapathNodes.h"
#include "vast/VASTSeqValue.h"
#include "vast/Strash.h"
#include "llvm/Pass.h"
#include "llvm/ADT/FoldingSet.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Allocator.h"
#define DEBUG_TYPE "vast-structural-hashing"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace vast {
using namespace llvm;

struct StrashNode : public FoldingSetNode {
  /// FastID - A reference to an Interned FoldingSetNodeID for this node.
  /// The StrashTable's BumpPtrAllocator holds the data.
  FoldingSetNodeIDRef FastID;

  unsigned ID;

  StrashNode(const FoldingSetNodeIDRef IDRef, unsigned ID)
    : FastID(IDRef), ID(ID) {}

  operator unsigned() const { return ID; }
};

template<typename SubClass>
class StrashTable {
  typedef CachedStrashTable::CacheTy CacheTy;

  friend struct FoldingSetTrait<StrashNode>;

  unsigned LastID;
  FoldingSet<StrashNode> Set;
  BumpPtrAllocator Allocator;

  void profileLeaf(VASTValue *Ptr, FoldingSetNodeID &ID) {
    if (VASTConstant *C = dyn_cast<VASTConstant>(Ptr)) {
      ID.Add(C->getAPInt());
      return;
    }

    static_cast<SubClass*>(this)->profileNonTrivialLeaf(Ptr, ID);
  }

  void profileExpr(VASTExpr *Expr, FoldingSetNodeID &ID, CacheTy &Cache) {
    Expr->ProfileWithoutOperands(ID);

    SmallVector<unsigned, 8> Operands;
    for (unsigned i = 0; i < Expr->size(); ++i) {
      VASTValPtr Operand = Expr->getOperand(i);
      unsigned NodeID = getOrCreateStrashID(Operand, Cache);
      Operands.push_back(NodeID);
    }

    // Sort the operands of commutative expressions
    if (Expr->isCommutative())
      array_pod_sort(Operands.begin(), Operands.end());  

    for (unsigned i = 0, e = Operands.size(); i < e; ++i)
      ID.AddInteger(Operands[i]);
  }

  void profile(VASTValue *Ptr, FoldingSetNodeID &ID, CacheTy &Cache) {
    if (VASTExpr *E = dyn_cast<VASTExpr>(Ptr)) {
      profileExpr(E, ID, Cache);
      return;
    }

    profileLeaf(Ptr, ID);
  }

  void profile(VASTValPtr Ptr, FoldingSetNodeID &ID, CacheTy &Cache) {
    unsigned NodeType = Ptr->getASTType();
    unsigned Data = (NodeType & 0x1f) | ((Ptr.isInverted() ? 0x1 : 0x0) << 5);
    assert(NodeType == (Data & 0x1f) && "NodeType overflow!");
    ID.AddInteger(Data);
    profile(Ptr.get(), ID, Cache);
  }

public:
  StrashTable() : LastID(0) {}

  unsigned lookupCache(VASTValPtr Ptr, CacheTy &Cache) const {
    CacheTy::const_iterator I = Cache.find(Ptr);
    return I == Cache.end() ? 0 : I->second;
  }

  unsigned getOrCreateStrashID(VASTValPtr Ptr, CacheTy &Cache) {
    if (unsigned NodeId = lookupCache(Ptr, Cache))
      return NodeId;

    // Now look it up in the Hash Table. 
    FoldingSetNodeID ID;
    profile(Ptr, ID, Cache);

    void *IP = 0;
    if (StrashNode *N = Set.FindNodeOrInsertPos(ID, IP)) {
      unsigned NodeId = unsigned(*N);
      assert(NodeId && "Bad ID!");
      Cache.insert(std::make_pair(Ptr, NodeId));
      return NodeId;
    }

    StrashNode *N = new (Allocator) StrashNode(ID.Intern(Allocator), ++LastID);
    Set.InsertNode(N, IP);

    unsigned NodeId = unsigned(*N);
    assert(NodeId && "Bad ID!");
    Cache.insert(std::make_pair(Ptr, NodeId));
    return NodeId;
  }

  // Add the selector to the table according to its name.
  unsigned getOrCreateStrashID(VASTSelector *Sel) {
    FoldingSetNodeID ID;
    ID.AddString(Sel->getName());

    void *IP = 0;
    if (StrashNode *N = Set.FindNodeOrInsertPos(ID, IP)) {
      unsigned NodeId = unsigned(*N);
      assert(NodeId && "Bad ID!");
      return NodeId;
    }

    StrashNode *N = new (Allocator) StrashNode(ID.Intern(Allocator), ++LastID);
    Set.InsertNode(N, IP);

    unsigned NodeId = unsigned(*N);

    return NodeId;
  }

  void reset() {
    Set.clear();
    Allocator.Reset();
    LastID = 0;
  }
};
} // end namespace

namespace llvm {
using namespace vast;
// Specialize FoldingSetTrait for StrashNode to avoid needing to compute
// temporary FoldingSetNodeID values.
template<> struct FoldingSetTrait<StrashNode> {
  static void Profile(const StrashNode &X, FoldingSetNodeID& ID) {
    ID = X.FastID;
  }
  static bool Equals(const StrashNode &X, const FoldingSetNodeID &ID,
                     unsigned IDHash, FoldingSetNodeID &TempID) {
    return ID == X.FastID;
  }

  static
  unsigned ComputeHash(const StrashNode &X, FoldingSetNodeID &TempID) {
    return X.FastID.ComputeHash();
  }
};

// The structural hash table which compute the hash of the datapath node based
// on the structural of the combinational cone rooted on that node.
void initializeStrashPass(PassRegistry &Registry);
} // end namespace llvm

namespace vast {
using namespace llvm;

struct Strash : public ImmutablePass, public StrashTable<Strash> {
  static char ID;

  Strash() : ImmutablePass(ID) {
    initializeStrashPass(*PassRegistry::getPassRegistry());
  }

  void profileNonTrivialLeaf(VASTValue *Ptr, FoldingSetNodeID &ID) {
    VASTNamedValue *NV = cast<VASTNamedValue>(Ptr);
    ID.AddString(NV->getName());
  }
};
}
//===----------------------------------------------------------------------===//

char Strash::ID = 0;
INITIALIZE_PASS(Strash, "shang-strash",
                "The structural hash table for the datapath nodes",
                false, true)

char CachedStrashTable::ID = 0;
INITIALIZE_PASS_BEGIN(CachedStrashTable, "shang-cached-strash",
                      "The structural hash table for the datapath nodes",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(Strash);
INITIALIZE_PASS_END(CachedStrashTable, "shang-cached-strash",
                    "The structural hash table for the datapath nodes",
                    false, true)

CachedStrashTable::CachedStrashTable() : VASTModulePass(ID) {
  initializeCachedStrashTablePass(*PassRegistry::getPassRegistry());
}

void CachedStrashTable::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequiredTransitive<Strash>();
  AU.setPreservesAll();
}

bool CachedStrashTable::runOnVASTModule(VASTModule &VM) {
  Table = &getAnalysis<Strash>();
  return false;
}

void CachedStrashTable::releaseMemory() {
  Cache.clear();
  Table->releaseMemory();
}

unsigned CachedStrashTable::getOrCreateStrashID(VASTValPtr Ptr) {
  return Table->getOrCreateStrashID(Ptr, Cache);
}

unsigned CachedStrashTable::getOrCreateStrashID(VASTSelector *Sel) {
  return Table->getOrCreateStrashID(Sel);
}

namespace vast {
using namespace llvm;

class CombPatterns : public StrashTable<CombPatterns> {
  typedef std::map<VASTValPtr, unsigned> CacheTy;
  CacheTy Cache;

  typedef std::map<VASTExpr*, std::vector<VASTSelector*> > LeavesMapTy;
  LeavesMapTy LeavesMap;

  unsigned getOrCreatePatternID(VASTExpr *Ptr, bool isInverted);
  unsigned getOrCreatePatternID(VASTExpr *Ptr);
public:
  CombPatterns() {}

  void profileNonTrivialLeaf(VASTValue *Ptr, FoldingSetNodeID &ID) {
    VASTNamedValue *NV = cast<VASTNamedValue>(Ptr);

    // We consider all VASTSeqValues are identical.
    if (isa<VASTSeqValue>(NV))
      return;

    ID.AddString(NV->getName());
  }

  unsigned lookupCache(VASTValPtr Ptr) const {
    CacheTy::const_iterator I = Cache.find(Ptr);
    return I == Cache.end() ? 0 : I->second;
  }

  unsigned getOrCreatePatternID(VASTValPtr Ptr);

  ArrayRef<VASTSelector*> getLeaves(VASTExpr *Expr) const {
    LeavesMapTy::const_iterator I = LeavesMap.find(Expr);
    assert(I != LeavesMap.end() && "Leaves not available?");
    return I->second;
  }
};
}

char CombPatternTable::ID = 0;
INITIALIZE_PASS_BEGIN(CombPatternTable, "shang-combinational-pattern",
                      "The pattern identification on the combinational logic",
                      false, true)
INITIALIZE_PASS_END(CombPatternTable, "shang-combinational-pattern",
                    "The pattern identification on the combinational logic",
                    false, true)

CombPatternTable::CombPatternTable() : VASTModulePass(ID), Table(0) {
  initializeCombPatternTablePass(*PassRegistry::getPassRegistry());
}

void CombPatternTable::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.setPreservesAll();
}

bool CombPatternTable::runOnVASTModule(VASTModule &VM) {
  Table = new CombPatterns();
  return false;
}

void CombPatternTable::releaseMemory() {
  delete Table;
  CachedDeltas.clear();
}

unsigned CombPatternTable::getOrCreatePatternID(VASTValPtr Ptr) {
  return Table->getOrCreatePatternID(Ptr);
}

ArrayRef<VASTSelector*> CombPatternTable::getLeaves(VASTExpr *Expr) const {
  return Table->getLeaves(Expr);
}

const CombPatternTable::DeltaResult CombPatternTable::AlwaysDifferent(true);
const CombPatternTable::DeltaResult CombPatternTable::AlwaysIdentical(false, true);

const CombPatternTable::DeltaResult &
CombPatternTable::getLeavesDelta(VASTExpr *LHS, VASTExpr *RHS) {
  if (getOrCreatePatternID(LHS) != getOrCreatePatternID(RHS))
    return AlwaysDifferent;

  DeltaResult &Delta = CachedDeltas[std::make_pair(LHS, RHS)];
  if (!Delta.isUnintialized())
    return Delta;

  ArrayRef<VASTSelector*> LHSLeaves = getLeaves(LHS), RHSLeaves = getLeaves(RHS);
  if (LHSLeaves.size() != RHSLeaves.size() || LHSLeaves.size() == 0)
    return (Delta = AlwaysDifferent);

  for (unsigned i = 0, e = LHSLeaves.size(); i != e; ++i) {
    VASTSelector *LHSLeaf = LHSLeaves[i];
    VASTSelector *RHSLeaf = RHSLeaves[i];
    if (LHSLeaf != RHSLeaf)
      Delta.Deltas.push_back(LeafDelta(LHSLeaf, RHSLeaf));
  }

  if (Delta.Deltas.empty())
    Delta.IsAlwaysIdentical = true;

  return Delta;
}

unsigned CombPatterns::getOrCreatePatternID(VASTValPtr Ptr) {
  if (unsigned ID = lookupCache(Ptr)) {
    assert((!isa<VASTExpr>(Ptr.get()) ||
            LeavesMap.count(cast<VASTExpr>(Ptr.get()))) &&
           "Id exists with out leaves?");
    return ID;
  }

  VASTExpr *ExprPtr = dyn_cast<VASTExpr>(Ptr);
  if (ExprPtr == NULL)
    return getOrCreateStrashID(Ptr, Cache);

  return getOrCreatePatternID(ExprPtr, Ptr.isInverted());
}

unsigned CombPatterns::getOrCreatePatternID(VASTExpr *Ptr, bool isInverted) {
  if (unsigned ID = lookupCache(VASTValPtr(Ptr, isInverted))) {
    assert(LeavesMap.count(cast<VASTExpr>(Ptr)) &&
           "Id exists with out leaves?");
    return ID;
  }

  // Build the pattern for the underlying expression first.
  getOrCreatePatternID(VASTValPtr(Ptr, isInverted));
  return getOrCreateStrashID(Ptr, Cache);
}

unsigned CombPatterns::getOrCreatePatternID(VASTExpr* Expr) {
  std::vector<VASTSelector*> &Leaves = LeavesMap[Expr];
  assert(Leaves.empty() && "Ptr is visited?");

  for (unsigned i = 0; i < Expr->size(); ++i) {
    VASTValPtr Operand = Expr->getOperand(i);
    getOrCreatePatternID(Operand);
    if (VASTExpr *SubExpr = dyn_cast<VASTExpr>(Operand.get())) {
      ArrayRef<VASTSelector*> SubExprLeaves = getLeaves(SubExpr);
      Leaves.insert(Leaves.end(), SubExprLeaves.begin(), SubExprLeaves.end());
    } else if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(Operand.get()))
      Leaves.push_back(SeqVal->getSelector());
  }

  // If we get a huge cone, simply not cache their leaves
  if (Leaves.size() > 256)
    Leaves.clear();

  return getOrCreateStrashID(Expr, Cache);
}
