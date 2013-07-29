//===----- CompGraph.cpp - Compatibility Graph for Binding ------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the interfaces of the Compatibility Graph. The
// compatibility graph represents the compatibilities between live intervals in
// the STG. Based on the compatibilities we can bind the variables with
// compatible live intervals to the same physical unit (register or functional
// unit).
//===----------------------------------------------------------------------===//

#include "CompGraph.h"

#include "shang/VASTSeqValue.h"

#include "llvm/Analysis/Dominators.h"

#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "shang-compatibility-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;

void CompGraphNode::print(raw_ostream &OS) const {
  ::dump(Reachables, OS);
}

void CompGraphNode::dump() const {
  print(dbgs());
}

void CompGraphNode::merge(const CompGraphNode *RHS, DominatorTree *DT) {
  Defs        |= RHS->Defs;
  Reachables  |= RHS->Reachables;
  DomBlock = DT->findNearestCommonDominator(DomBlock, RHS->DomBlock);
}

bool CompGraphNode::isCompatibleWith(const CompGraphNode *RHS) const {
  if (Sels.size() != RHS->Sels.size())
    return false;

  // Bitwidth should be the same.
  for (unsigned i = 0, e = size(); i != e; ++i)
    if (getSelector(i)->getBitWidth() != RHS->getSelector(i)->getBitWidth())
      return false;

  // Defines should not intersects.
  if (intersects(Defs, RHS->Defs))
    return false;

  // Alives should not intersects.
  if (intersects(Reachables, RHS->Reachables))
    return false;

  return true;
}

bool CompGraphNode::lt(CompGraphNode *Src, CompGraphNode *Dst,
                         DominatorTree *DT) {
  assert(!Src->isTrivial() && !Dst->isTrivial() && "Unexpected trivial node!");
  if (DT->properlyDominates(Src->DomBlock, Dst->DomBlock))
    return true;

  if (DT->properlyDominates(Dst->DomBlock, Src->DomBlock))
    return true;

  return Src < Dst;
}

void CompGraphBase::verifyTransitive() {
  // Check a -> b and b -> c implies a -> c;
  typedef NodeVecTy::iterator node_iterator;
  for (node_iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
    NodeTy *Node = I;

    for (NodeTy::iterator SI = Node->succ_begin(), SE = Node->succ_end();
         SI != SE; ++SI) {
      NodeTy *Succ = *SI;

      if (Succ->isTrivial())
        continue;

      for (NodeTy::iterator SSI = Succ->succ_begin(), SSE = Succ->succ_end();
           SSI != SSE; ++SSI) {
        NodeTy *SuccSucc = *SI;

        if (SuccSucc->isTrivial())
          continue;

        if (!Node->isCompatibleWith(SuccSucc))
          llvm_unreachable("Compatible graph is not transitive!");
      }
    }
  }
}

void CompGraphBase::recomputeCompatibility() {
  Entry.dropAllEdges();
  Exit.dropAllEdges();

  for (iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I)
    I->dropAllEdges();

  for (iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
    NodeTy *Node = I;

    // And insert the node into the graph.
    for (NodeTy::iterator I = Entry.succ_begin(), E = Entry.succ_end(); I != E; ++I) {
      NodeTy *Other = *I;

      // Make edge between compatible nodes.
      if (Node->isCompatibleWith(Other))
        NodeTy::MakeEdge(Node, Other, DT);
    }

    // There will always edge from entry to a node and from node to exit.
    NodeTy::MakeEdge(&Entry, Node, 0);
    NodeTy::MakeEdge(Node, &Exit, 0);
  }
}

namespace llvm {
template<> struct DOTGraphTraits<CompGraphBase*> : public DefaultDOTGraphTraits{
  typedef CompGraphBase GraphTy;
  typedef GraphTy::NodeTy NodeTy;
  typedef NodeTy::iterator NodeIterator;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getEdgeSourceLabel(const NodeTy *Node,NodeIterator I){
    return "0";
  }

  std::string getNodeLabel(const NodeTy *Node, const GraphTy *Graph) {
    return Node->isTrivial() ? "<null>" : "node";
  }

  static std::string getNodeAttributes(const NodeTy *Node,
                                       const GraphTy *Graph) {
    return "shape=Mrecord";
  }
};
}

void CompGraphBase::viewGraph() {
  ViewGraph(this, "CompatibilityGraph");
}
