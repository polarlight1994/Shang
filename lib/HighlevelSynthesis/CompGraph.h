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
public:
  const unsigned Idx;
private:
  unsigned Order;
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
  
  friend class CompGraphBase;
public:
  static const int HUGE_NEG_VAL = -1000000000;
  static const int TINY_VAL = 1;

  CompGraphNode() : Idx(0), Order(0), DomBlock(0) { }

  CompGraphNode(unsigned Idx, BasicBlock *DomBlock, ArrayRef<VASTSelector*> Sels)
    : Idx(Idx), Order(UINT32_MAX), DomBlock(DomBlock), Sels(Sels.begin(), Sels.end()) {}

  void updateOrder(unsigned NewOrder) {
    Order = std::min(Order, NewOrder);
  }

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
  typedef std::map<CompGraphNode*, unsigned> BindingMapTy;

protected:
  typedef ilist<NodeTy> NodeVecTy;
  typedef std::map<VASTSelector*, NodeTy> NodeMapTy;
  // The dummy entry node of the graph.
  NodeTy Entry, Exit;
  // Nodes vector.
  NodeVecTy Nodes;
  DominatorTree *DT;
  BindingMapTy BindingMap;

  void deleteNode(NodeTy *N) {
    N->unlink();
    Nodes.erase(N);
  }

  std::map<BasicBlock*, unsigned> DTDFSOrder;
  void initalizeDTDFSOrder();

  unsigned getDTDFSOrder(BasicBlock *BB) const {
    std::map<BasicBlock*, unsigned>::const_iterator I = DTDFSOrder.find(BB);
    assert(I != DTDFSOrder.end() && "DFS order not defined?");
    return I->second;
  }
public:
  explicit CompGraphBase(DominatorTree *DT) : Entry(), Exit(), DT(DT) {
    initalizeDTDFSOrder();
  }

  virtual ~CompGraphBase() {}

  const NodeTy *getEntry() const { return &Entry; }
  NodeTy *getEntry() { return &Entry; }
  const NodeTy *getExit() const { return &Exit; }
  NodeTy *getExit() { return &Exit; }

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

  void fixTransitive();

  unsigned performBinding();

  typedef BindingMapTy::const_iterator binding_iterator;
  binding_iterator binding_begin() const {
    return BindingMap.begin();
  }

  binding_iterator binding_end() const {
    return BindingMap.end();
  }

  unsigned getBinding(CompGraphNode *N) const {
    binding_iterator I = BindingMap.find(N);
    return I == BindingMap.end() ? 0 : I->second;
  }

  bool hasbinding() const { return !BindingMap.empty(); }

  void viewGraph();
  

  bool isBefore(CompGraphNode *Src, CompGraphNode *Dst);

  // Make the edge with default weight, we will udate the weight later.
  void makeEdge(CompGraphNode *Src, CompGraphNode *Dst) {
    // Make sure source is earlier than destination.
    if (!Src->isTrivial() && !Dst->isTrivial() && !isBefore(Src, Dst))
      std::swap(Dst, Src);

    Src->Succs.insert(Dst);
    Src->SuccWeights.insert(std::make_pair(Dst, NodeTy::TINY_VAL));
    Dst->Preds.insert(Src);
  }
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
