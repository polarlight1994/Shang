//===------ Strash.cpp - Structural Hash Table for Datapath Nodes ---------===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "Strash.h"

#include "shang/Passes.h"
#include "shang/VASTDatapathNodes.h"
#include "shang/VASTSeqValue.h"

#include "llvm/ADT/STLExtras.h"

using namespace llvm;

void StrashTable::calculateLeafID(VASTValue *Ptr, FoldingSetNodeID &ID) {
  if (VASTNamedValue *NV = dyn_cast<VASTNamedValue>(Ptr)) {
    ID.AddString(NV->getName());
    return;
  }

  VASTImmediate *Imm = cast<VASTImmediate>(Ptr);
  ID.Add(Imm->getAPInt());
}

void StrashTable::calculateExprID(VASTExpr *Expr, FoldingSetNodeID &ID,
                                  DenseMap<VASTValPtr, unsigned> &Cache) {
  VASTExpr::Opcode Opcode = Expr->getOpcode();
  ID.AddInteger(Opcode);
  ID.AddInteger(Expr->UB);
  ID.AddInteger(Expr->LB);
  //ID.AddInteger(Expr->size());

  SmallVector<unsigned, 8> Operands;

  for (unsigned i = 0; i < Expr->size(); ++i) {
    VASTValPtr Operand = Expr->getOperand(i);
    unsigned NodeID = getOrInsertNode(Operand, Cache);
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

void
StrashTable::calculateID(VASTValue *Ptr, FoldingSetNodeID &ID, CacheTy &Cache) {
  if (VASTExpr *E = dyn_cast<VASTExpr>(Ptr)) {
    calculateExprID(E, ID, Cache);
    return;
  }

  calculateLeafID(Ptr, ID);
}

void
StrashTable::calculateID(VASTValPtr Ptr, FoldingSetNodeID &ID, CacheTy &Cache) {
  unsigned NodeType = Ptr->getASTType();
  unsigned Data = (NodeType & 0x1f) | ((Ptr.isInverted() ? 0x1 : 0x0) << 5);
  assert(NodeType == (Data & 0x1f) && "NodeType overflow!");
  ID.AddInteger(Data);
  calculateID(Ptr.get(), ID, Cache);
}

unsigned StrashTable::getOrInsertNode(VASTValPtr Ptr, CacheTy &Cache) {
  if (unsigned NodeId = Cache.lookup(Ptr))
    return NodeId;
  
  // Now look it up in the Hash Table. 
  FoldingSetNodeID ID;
  calculateID(Ptr, ID, Cache);

  void *IP = 0;
  if (Node *N = Set.FindNodeOrInsertPos(ID, IP)) {
    unsigned NodeId = unsigned(*N);
    Cache.insert(std::make_pair(Ptr, NodeId));
    return NodeId;
  }

  Node *N = new (Allocator) Node(ID.Intern(Allocator), ++LastID);
  Set.InsertNode(N, IP);

  unsigned NodeId = unsigned(*N);
  Cache.insert(std::make_pair(Ptr, NodeId));
  return NodeId;
}

StrashTable::StrashTable() : ImmutablePass(ID), LastID(0) {
  initializeStrashTablePass(*PassRegistry::getPassRegistry());
}

char StrashTable::ID = 0;

INITIALIZE_PASS(StrashTable, "shang-strash",
                "The structural hash table for the datapath nodes", false, true)
