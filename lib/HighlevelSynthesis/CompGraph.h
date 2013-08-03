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

class CompGraphNodeBase : public ilist_node<CompGraphNodeBase> {
public:
  const unsigned Idx : 31;
  const unsigned IsTrivial : 1;
private:
  unsigned Order;
  BasicBlock *DomBlock;
  SparseBitVector<> Defs;
  SparseBitVector<> Reachables;

  typedef SmallPtrSet<CompGraphNodeBase*, 8> NodeVecTy;
  // Predecessors and Successors.
  NodeVecTy Preds, Succs;

  typedef std::map<const CompGraphNodeBase*, float> CostVecTy;
  CostVecTy SuccCosts;

  static bool intersects(const SparseBitVector<> &LHSBits,
                         const SparseBitVector<> &RHSBits) {
    return LHSBits.intersects(RHSBits);
  }
  
  friend class CompGraphBase;
protected:
  virtual bool isCompatibleWithInternal(const CompGraphNodeBase *RHS) const {
    return true;
  }
public:

  CompGraphNodeBase() : Idx(0), IsTrivial(true), Order(0), DomBlock(0) { }

  CompGraphNodeBase(unsigned Idx, BasicBlock *DomBlock, ArrayRef<VASTSelector*> Sels)
    : Idx(Idx), IsTrivial(false), Order(UINT32_MAX), DomBlock(DomBlock) {}

  BasicBlock *getDomBlock() const { return DomBlock; }

  void updateOrder(unsigned NewOrder) {
    Order = std::min(Order, NewOrder);
  }

  void dropAllEdges() {
    Preds.clear();
    Succs.clear();
    SuccCosts.clear();
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

  void merge(const CompGraphNodeBase *RHS, DominatorTree *DT);

  bool isCompatibleWith(const CompGraphNodeBase *RHS) const;

  SparseBitVector<> &getDefs() { return Defs; }
  SparseBitVector<> &getReachables() { return Reachables; }

  bool isNeighbor(CompGraphNodeBase *RHS) const {
    return Preds.count(RHS) || Succs.count(RHS);
  }

  float getCostTo(const CompGraphNodeBase *To) const;

  void setCost(const CompGraphNodeBase *To, float Cost);

  // Unlink the Succ from current node.
  void unlinkSucc(CompGraphNodeBase *Succ) {
    bool deleted = Succs.erase(Succ);
    assert(deleted && "Succ is not the successor of this!");
    SuccCosts.erase(Succ);

    // Current node is not the predecessor of succ node too.
    deleted = Succ->Preds.erase(this);
    assert(deleted && "this is not the predecessor of succ!");
    (void) deleted;
  }

  // Unlink the Pred from current node.
  void unlinkPred(CompGraphNodeBase *Pred) {
    bool deleted = Preds.erase(Pred);
    assert(deleted && "Pred is not the predecessor of this!");

    // Current node is not the successor of pred node too.
    deleted = Pred->Succs.erase(this);
    Pred->SuccCosts.erase(this);
    assert(deleted && "this is not the successor of Pred!");
    (void) deleted;
  }

  void unlink() {
    while (!succ_empty())
      unlinkSucc(*succ_begin());

    while (!pred_empty())
      unlinkPred(*pred_begin());
  }
};

template<> struct GraphTraits<CompGraphNodeBase*> {
  typedef CompGraphNodeBase NodeType;
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
  typedef CompGraphNodeBase NodeTy;
  typedef std::map<CompGraphNodeBase*, unsigned> BindingMapTy;

protected:
  typedef ilist<NodeTy> NodeVecTy;
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

  virtual float computeCost(NodeTy *Src, unsigned SrcBinding,
                            NodeTy *Dst, unsigned DstBinding) const {
    return 0.0f;
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

  void computeCompatibility();
  void compuateEdgeCosts();
  void fixTransitive();

  unsigned performBinding();

  typedef BindingMapTy::const_iterator binding_iterator;
  binding_iterator binding_begin() const {
    return BindingMap.begin();
  }

  binding_iterator binding_end() const {
    return BindingMap.end();
  }

  unsigned getBinding(CompGraphNodeBase *N) const {
    binding_iterator I = BindingMap.find(N);
    return I == BindingMap.end() ? 0 : I->second;
  }

  bool hasbinding() const { return !BindingMap.empty(); }

  void viewGraph();
  

  bool isBefore(CompGraphNodeBase *Src, CompGraphNodeBase *Dst);

  // Make the edge with default weight, we will udate the weight later.
  void makeEdge(CompGraphNodeBase *Src, CompGraphNodeBase *Dst) {
    // Make sure source is earlier than destination.
    if (!Src->IsTrivial && !Dst->IsTrivial && !isBefore(Src, Dst))
      std::swap(Dst, Src);

    Src->Succs.insert(Dst);
    Src->SuccCosts.insert(std::make_pair(Dst, 0.0f));
    Dst->Preds.insert(Src);
  }
};

template <> struct GraphTraits<CompGraphBase*>
  : public GraphTraits<CompGraphNodeBase*> {

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
