//===------- CompGraph.h - Compatibility Graph for Binding ------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the interfaces of the Compatibility Graph. The
// compatibility graph represents the compatibilities between live intervals in
// the STG. Based on the compatibilities we can bind the variables with
// compatible live intervals to the same physical unit (register or functional
// unit).
//===----------------------------------------------------------------------===//

#ifndef COMPATIBiLITY_GRAPH_H
#define COMPATIBiLITY_GRAPH_H

#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/ilist.h"
#include <map>

namespace llvm {
class BasicBlock;
class VASTSelector;
class DominatorTree;

class CompGraphNode : public ilist_node<CompGraphNode> {
  BasicBlock *DomBlock;
  // The underlying data.
  SmallVector<VASTSelector*, 3> Sels;
  SparseBitVector<> Defs;
  SparseBitVector<> Reachables;

  typedef SmallPtrSet<CompGraphNode*, 8> NodeVecTy;
  // Predecessors and Successors.
  NodeVecTy Preds, Succs;

  typedef std::map<const CompGraphNode*, float> WeightVecTy;
  WeightVecTy SuccWeights;

  static bool intersects(const SparseBitVector<> &LHSBits,
                         const SparseBitVector<> &RHSBits) {
    return LHSBits.intersects(RHSBits);
  }

public:
  static const int HUGE_NEG_VAL = -1000000000;
  static const int TINY_VAL = 1;

  CompGraphNode() : DomBlock(0) { }

  CompGraphNode(BasicBlock *DomBlock, ArrayRef<VASTSelector*> Sels)
    : DomBlock(DomBlock), Sels(Sels.begin(), Sels.end()) {}

  typedef SmallVectorImpl<VASTSelector*>::iterator sel_iterator;
  sel_iterator begin() { return Sels.begin(); }
  sel_iterator end() { return Sels.end(); }

  typedef SmallVectorImpl<VASTSelector*>::const_iterator const_sel_iterator;
  const_sel_iterator begin() const { return Sels.begin(); }
  const_sel_iterator end() const { return Sels.end(); }

  size_t size() const { return Sels.size(); }
  VASTSelector *getSelector(unsigned Idx) const { return Sels[Idx]; }

  SparseBitVector<> &getDefs() { return Defs; }
  SparseBitVector<> &getReachables() { return Reachables; }

  bool isTrivial() const { return Sels.empty(); }

  void dropAllEdges() {
    Preds.clear();
    Succs.clear();
    SuccWeights.clear();
  }

  void print(raw_ostream &OS) const;

  void dump() const;

  //typedef NodeVecTy::iterator iterator;
  typedef NodeVecTy::const_iterator iterator;

  iterator succ_begin() const { return Succs.begin(); }
  iterator succ_end()   const { return Succs.end(); }
  unsigned num_succ()   const { return Succs.size(); }
  bool     succ_empty() const { return Succs.empty(); }

  iterator pred_begin() const { return Preds.begin(); }
  iterator pred_end()   const { return Preds.end(); }
  unsigned num_pred()   const { return Preds.size(); }
  bool     pred_empty() const { return Preds.empty(); }

  unsigned degree() const { return num_succ() + num_pred(); }

  void merge(const CompGraphNode *RHS, DominatorTree *DT);

  bool isCompatibleWith(const CompGraphNode *RHS) const;

  bool isNeighbor(CompGraphNode *RHS) const {
    return Preds.count(RHS) || Succs.count(RHS);
  }

  int getWeightTo(const CompGraphNode *To) const {
    return SuccWeights.find(To)->second;
  }

  int computeNeighborWeight(CompGraphNode *RHS = 0) const {
    int Weight = 0;

    for (iterator I = pred_begin(), E = pred_end(); I != E; ++I) {
      CompGraphNode *NP = *I;
      if (NP->isTrivial()) continue;

      // RHS is null means we want to compute all neighbor weight, otherwise
      // means we want to compute the common neighbor weight only.
      if (RHS == 0 || NP->isNeighbor(RHS)) Weight += NP->getWeightTo(this);
    }

    for (iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
      CompGraphNode *NS = *I;
      if (NS->isTrivial()) continue;

      // RHS is null means we want to compute all neighbor weight, otherwise
      // means we want to compute the common neighbor weight only.
      if (RHS == 0 || NS->isNeighbor(RHS)) Weight += getWeightTo(NS);
    }

    if (RHS && isNeighbor(RHS)) {
      if (Succs.count(RHS)) Weight += getWeightTo(RHS);
      else                  Weight += RHS->getWeightTo(this);
    }

    return Weight;
  }

  // Unlink the Succ from current node.
  void unlinkSucc(CompGraphNode *Succ) {
    bool deleted = Succs.erase(Succ);
    assert(deleted && "Succ is not the successor of this!");
    SuccWeights.erase(Succ);

    // Current node is not the predecessor of succ node too.
    deleted = Succ->Preds.erase(this);
    assert(deleted && "this is not the predecessor of succ!");
    (void) deleted;
  }

  // Unlink the Pred from current node.
  void unlinkPred(CompGraphNode *Pred) {
    bool deleted = Preds.erase(Pred);
    assert(deleted && "Pred is not the predecessor of this!");

    // Current node is not the successor of pred node too.
    deleted = Pred->Succs.erase(this);
    Pred->SuccWeights.erase(this);
    assert(deleted && "this is not the successor of Pred!");
    (void) deleted;
  }

  void deleteUncommonEdges(CompGraphNode *RHS) {
    // Delete edge from P and Q that are not connected to their common neighbors.
    SmallVector<CompGraphNode*, 8> ToUnlink;
    // Unlink preds.
    for (iterator I = pred_begin(), E = pred_end(); I != E; ++I) {
      CompGraphNode *N = *I;
      if (!RHS->isNeighbor(N)) ToUnlink.push_back(N);
    }

    while (!ToUnlink.empty())
      unlinkPred(ToUnlink.pop_back_val());

    // Unlink succs.
    for (iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
      CompGraphNode *N = *I;
      if (!RHS->isNeighbor(N)) ToUnlink.push_back(N);
    }

    while (!ToUnlink.empty())
      unlinkSucc(ToUnlink.pop_back_val());
  }

  void unlink() {
    while (!succ_empty())
      unlinkSucc(*succ_begin());

    while (!pred_empty())
      unlinkPred(*pred_begin());
  }

  template<typename CompEdgeWeight>
  void updateEdgeWeight(CompEdgeWeight &C) {
    SmallVector<CompGraphNode*, 8> SuccToUnlink;
    for (iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
      CompGraphNode *Succ = *I;
      // Not need to update the weight of the exit edge.
      if (Succ->isTrivial()) {
        int Weigth = C(this, Succ);
        if (Weigth <= HUGE_NEG_VAL) {
          SuccToUnlink.push_back(Succ);
          continue;
        }

        SuccWeights[Succ] = Weigth;
      } else
        // Make find longest path prefer to end with exit if possible.
        SuccWeights[Succ] = TINY_VAL;
    }

    while (!SuccToUnlink.empty())
      unlinkSucc(SuccToUnlink.pop_back_val());
  }

  static bool lt(CompGraphNode *Src, CompGraphNode *Dst, DominatorTree *DT);

  // Make the edge with default weight, we will udate the weight later.
  static
  void MakeEdge(CompGraphNode *Src, CompGraphNode *Dst, DominatorTree *DT) {
    // Make sure source is earlier than destination.
    if (!Src->isTrivial() && !Dst->isTrivial() && !lt(Src, Dst, DT))
      std::swap(Dst, Src);

    Src->Succs.insert(Dst);
    Src->SuccWeights.insert(std::make_pair(Dst, TINY_VAL));
    Dst->Preds.insert(Src);
  }
};

template<> struct GraphTraits<CompGraphNode*> {
  typedef CompGraphNode NodeType;
  typedef NodeType::iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->succ_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->succ_end();
  }
};


class CompGraphBase {
public:
  typedef CompGraphNode NodeTy;

protected:
  typedef ilist<NodeTy> NodeVecTy;
  typedef std::map<VASTSelector*, NodeTy> NodeMapTy;
  // The dummy entry node of the graph.
  NodeTy Entry, Exit;
  // Nodes vector.
  NodeVecTy Nodes;
  DominatorTree *DT;

  void deleteNode(NodeTy *N) {
    N->unlink();
    Nodes.erase(N);
  }
public:
  explicit CompGraphBase(DominatorTree *DT) : Entry(), Exit(), DT(DT) {}

  virtual ~CompGraphBase() {}

  const NodeTy *getEntry() const { return &Entry; }
  const NodeTy *getExit() const { return &Exit; }

  typedef NodeVecTy::iterator iterator;

  // All nodes (except exit node) are successors of the entry node.
  iterator begin() { return Nodes.begin(); }
  iterator end()   { return Nodes.end(); }

  bool hasMoreThanOneNode() const {
    return !Nodes.empty() && &Nodes.front() != &Nodes.back();
  }

  void merge(NodeTy *From, NodeTy *To) {
    To->merge(From, DT);
    deleteNode(From);
  }

  void recomputeCompatibility();

  void verifyTransitive();

  void performBinding();

  void viewGraph();
};

template <> struct GraphTraits<CompGraphBase*>
  : public GraphTraits<CompGraphNode*> {

  typedef CompGraphBase::iterator nodes_iterator;
  static nodes_iterator nodes_begin(CompGraphBase *G) {
    return G->begin();
  }

  static nodes_iterator nodes_end(CompGraphBase *G) {
    return G->end();
  }
};
}

#endif
