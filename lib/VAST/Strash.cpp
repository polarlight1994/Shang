//===------ Strash.cpp - Structural Hash Table for Datapath Nodes ---------===//
//
//                      The Shang HLS frameowrk                               //
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

#include "Strash.h"

#include "shang/Passes.h"
#include "shang/VASTDatapathNodes.h"
#include "shang/VASTSeqValue.h"
#include "llvm/Pass.h"
#include "llvm/ADT/FoldingSet.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Allocator.h"

using namespace llvm;

namespace llvm {
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
    if (VASTImmediate *Imm = dyn_cast<VASTImmediate>(Ptr)) {
      ID.Add(Imm->getAPInt());
      return;
    }

    static_cast<SubClass*>(this)->profileNonTrivialLeaf(Ptr, ID);
  }

  void profileExpr(VASTExpr *Expr, FoldingSetNodeID &ID, CacheTy &Cache) {
    VASTExpr::Opcode Opcode = Expr->getOpcode();
    ID.AddInteger(Opcode);
    ID.AddInteger(Expr->UB);
    ID.AddInteger(Expr->LB);
    //ID.AddInteger(Expr->size());

    SmallVector<unsigned, 8> Operands;

    for (unsigned i = 0; i < Expr->size(); ++i) {
      VASTValPtr Operand = Expr->getOperand(i);
      unsigned NodeID = getOrCreateStrashID(Operand, Cache);
      //unsigned Data = (NodeID << 8) | (Operand->getBitWidth() & 0xff);
      //assert((Data >> 8) == NodeID && "NodeID overflow!");
      Operands.push_back(NodeID);
    }

    // Sort the operands of commutative expressions
    if (Opcode == VASTExpr::dpAnd || Opcode == VASTExpr::dpAdd
        || Opcode == VASTExpr::dpMul)
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

  unsigned getOrCreateStrashID(VASTValPtr Ptr, CacheTy &Cache) {
    if (unsigned NodeId = Cache.lookup(Ptr))
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

  void reset() {
    Set.clear();
    Allocator.Reset();
    LastID = 0;
  }
};

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


// The structural hash table which compute the hash of the datapath node based
// on the structural of the combinational cone rooted on that node, but use a
// different leaf profiling function.
void initializeSequashPass(PassRegistry &Registry);

struct Sequash : public ImmutablePass, public StrashTable<Sequash> {
  static char ID;

  Sequash(): ImmutablePass(ID) {
    initializeSequashPass(*PassRegistry::getPassRegistry());
  }

  void profileNonTrivialLeaf(VASTValue *Ptr, FoldingSetNodeID &ID) {
    if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(Ptr)) {
      if (Value *V = SeqVal->getLLVMValue()) {
        ID.Add(V);
        return;
      }
    }
    
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
}

unsigned CachedStrashTable::getOrCreateStrashID(VASTValPtr Ptr) {
  return Table->getOrCreateStrashID(Ptr, Cache);
}

//===----------------------------------------------------------------------===//
char Sequash::ID = 0;
INITIALIZE_PASS(Sequash, "shang-sequash",
                "The sequential hash table for the datapath nodes",
                false, true)

char CachedSequashTable::ID = 0;
INITIALIZE_PASS_BEGIN(CachedSequashTable, "shang-cached-sequash",
                      "The sequential hash table for the datapath nodes",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(Sequash);
INITIALIZE_PASS_END(CachedSequashTable, "shang-cached-sequash",
                    "The sequential hash table for the datapath nodes",
                    false, true)

CachedSequashTable::CachedSequashTable() : VASTModulePass(ID) {
  initializeCachedStrashTablePass(*PassRegistry::getPassRegistry());
}

void CachedSequashTable::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequiredTransitive<Sequash>();
  AU.setPreservesAll();
}

bool CachedSequashTable::runOnVASTModule(VASTModule &VM) {
  Table = &getAnalysis<Sequash>();
  return false;
}

void CachedSequashTable::releaseMemory() {
  Cache.clear();
}

unsigned CachedSequashTable::getOrCreateSequashID(VASTValPtr Ptr) {
  return Table->getOrCreateStrashID(Ptr, Cache);
}
