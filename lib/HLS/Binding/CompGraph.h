//===------- CompGraph.h - Compatibility Graph for Binding ------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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

#ifndef COMPATIBILITY_GRAPH_H
#define COMPATIBILITY_GRAPH_H

#include "vast/Dataflow.h"
#include "vast/FUInfo.h"

#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/ilist.h"

#include <set>
#include <map>

namespace llvm {
class BasicBlock;
class DominatorTree;
}

namespace vast {
using namespace llvm;

class VASTSelector;
class VASTSeqOp;
class VASTSeqInst;
class VASTSeqValue;
class CombPatternTable;
class MinCostFlowSolver;

class CompGraphNode : public ilist_node<CompGraphNode> {
public:
  const unsigned Idx : 31;
  const unsigned IsTrivial : 1;
  const VFUs::FUTypes FUType;
  const unsigned FUCost;
  const DataflowInst Inst;

  struct Cost {
    // Fixed cost including saved resource, and timing criticality.
    float InterconnectCost;
    float SchedulingCost;
    typedef std::pair<unsigned*, unsigned*> NodePair;
    // The leaves differences of patterns rooted on the fanins of the MUX, if
    // the leaves are bind to the same physical units, the pattern become
    // identical, and hence the corresponding fanin (the root of the patterns)
    std::map<NodePair, float> Deltas;
    typedef std::map<std::pair<unsigned*, unsigned*>, float>::const_iterator
            delta_iterator;

    delta_iterator delta_begin() const { return Deltas.begin(); }
    delta_iterator delta_end() const { return Deltas.end(); }

    void accumulateDelta(CompGraphNode *Src, CompGraphNode *Dst, float Weight);

    float getMergedDetaBenefit() const;

    Cost() : InterconnectCost(0.0f), SchedulingCost(0.0f) {}
  };

private:
  unsigned Order;
  SparseBitVector<> Defs;
  SparseBitVector<> Reachables;
  // The underlying data.
  SmallVector<VASTSelector*, 3> Sels;

  typedef std::set<CompGraphNode*> NodeVecTy;
  // Predecessors and Successors.
  NodeVecTy Preds, Succs;

  typedef std::map<CompGraphNode*, Cost> CostVecTy;
  CostVecTy SuccCosts;
  unsigned BindingIdx;

  static bool intersects(const SparseBitVector<> &LHSBits,
                         const SparseBitVector<> &RHSBits) {
    return LHSBits.intersects(RHSBits);
  }

  typedef CostVecTy::iterator cost_iterator;
  cost_iterator cost_begin() { return SuccCosts.begin(); }
  cost_iterator cost_end() { return SuccCosts.end(); }

protected:
  bool isCompatibleWithInterval(const CompGraphNode *RHS) const;
  bool isCompatibleWithStructural(const CompGraphNode *RHS) const;

  friend class CompGraphBase;
public:

  CompGraphNode()
    : Idx(0), IsTrivial(true), FUType(VFUs::Trivial), FUCost(0), Inst(),
      Order(0), BindingIdx(0) { }

  CompGraphNode(VFUs::FUTypes FUType, unsigned FUCost, unsigned Idx,
                DataflowInst Inst, ArrayRef<VASTSelector*> Sels)
    : Idx(Idx), IsTrivial(false), FUType(FUType), FUCost(FUCost), Inst(Inst),
      Order(UINT32_MAX), Sels(Sels.begin(), Sels.end()), BindingIdx(Idx) {}

  virtual ~CompGraphNode() {}

  BasicBlock *getDomBlock() const;

  unsigned getBindingIdx() const { return BindingIdx; }
  bool setBindingIdx(unsigned Idx) {
    bool Changed = BindingIdx != Idx;
    BindingIdx = Idx;
    return Changed;
  }

  typedef SmallVectorImpl<VASTSelector*>::iterator sel_iterator;
  sel_iterator begin() { return Sels.begin(); }
  sel_iterator end() { return Sels.end(); }

  typedef SmallVectorImpl<VASTSelector*>::const_iterator const_sel_iterator;
  const_sel_iterator begin() const { return Sels.begin(); }
  const_sel_iterator end() const { return Sels.end(); }

  size_t size() const { return Sels.size(); }
  VASTSelector *getSelector(unsigned Idx) const { return Sels[Idx]; }

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

  void merge(const CompGraphNode *RHS);

  virtual bool isCompatibleWith(const CompGraphNode *RHS) const {
    return isCompatibleWithStructural(RHS) && isCompatibleWithInterval(RHS);
  }

  SparseBitVector<> &getDefs() { return Defs; }
  SparseBitVector<> &getReachables() { return Reachables; }

  const SparseBitVector<> &getDefs() const { return Defs; }
  const SparseBitVector<> &getReachables() const { return Reachables; }

  bool isIntervalEmpty() const { return Defs.empty() && Reachables.empty(); }

  bool isNeighbor(CompGraphNode *RHS) const {
    return Preds.count(RHS) || Succs.count(RHS);
  }

  bool countSuccessor(CompGraphNode *RHS) const {
    return Succs.count(RHS);
  }

  const Cost &getCostTo(const CompGraphNode *To) const;
  Cost &getCostToInternal(const CompGraphNode *To);

  // Unlink the Succ from current node.
  void unlinkSucc(CompGraphNode *Succ) {
    bool deleted = Succs.erase(Succ);
    assert(deleted && "Succ is not the successor of this!");
    SuccCosts.erase(Succ);

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
} // end namespace vast

namespace llvm {
using namespace vast;

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
} // end namespace llvm

namespace vast {
using namespace llvm;
class CompGraphBase {
public:
  typedef CompGraphNode NodeTy;
  typedef std::pair<NodeTy*, NodeTy*> EdgeType;

  typedef std::vector<NodeTy*> ClusterType;
  typedef std::vector<ClusterType> ClusterVectors;

  typedef std::vector<EdgeType> EdgeVector;
protected:
  typedef ilist<NodeTy> NodeVecTy;
  // The dummy entry node of the graph.
  NodeTy Entry, Exit;
  // Nodes vector.
  NodeVecTy Nodes;
  std::map<VASTSelector*, CompGraphNode*> SelectorMap;

  // Due to CFG folding, there maybe more than one operation correspond to
  // the same LLVM Instruction. These operations operate on the same set of
  // registers, we need to avoid adding them more than once to the compatibility
  // graph.
  std::map<DataflowInst, CompGraphNode*> InstMap;

  // Pre-calculate the possible merged edges to speed up the cost update.
  std::map<EdgeType, EdgeVector> FaninCompatibles, FanoutCompatibles;

  DominatorTree &DT;
  ClusterVectors Clusters;

  void deleteNode(NodeTy *N) {
    N->unlink();
    Nodes.erase(N);
  }

  std::map<BasicBlock*, unsigned> DomTreeLevels;
  void initalizeDomTreeLevel();

  unsigned getDomTreeLevel(BasicBlock *BB) const {
    std::map<BasicBlock*, unsigned>::const_iterator I = DomTreeLevels.find(BB);
    assert(I != DomTreeLevels.end() && "DFS order not defined?");
    return I->second;
  }

  virtual CompGraphNode *createNode(VFUs::FUTypes FUType, unsigned FUCost,
                                    unsigned Idx, DataflowInst Inst,
                                    ArrayRef<VASTSelector*> Sels) const {
    return new CompGraphNode(FUType, FUCost, Idx, Inst, Sels);
  }

  unsigned computeReqiredResource(const CompGraphNode *Src) const;

private:
  MinCostFlowSolver *MCF[5];
public:
  explicit CompGraphBase(DominatorTree &DT)
    : Entry(), Exit(), DT(DT) {
    initalizeDomTreeLevel();
    for (unsigned i = 0; i < 5; ++i)
      MCF[i] = 0;
  }

  virtual ~CompGraphBase();

  CompGraphNode *lookupNode(VASTSelector *Sel) const {
    std::map<VASTSelector*, NodeTy*>::const_iterator I = SelectorMap.find(Sel);
    return I != SelectorMap.end() ? I->second : 0;
  }

  NodeTy *addNewNode(VASTSeqInst *SeqInst);
  void addBoundNode(VASTSeqOp *SeqOp);

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
    To->merge(From);
    deleteNode(From);
  }

  virtual float computeCost(const NodeTy *Src, const NodeTy *Dst) const {
    return 0.0f;
  }

  void decomposeTrivialNodes();
  void computeCompatibility();
  void initializeCosts(CombPatternTable &CPT);
  void fixTransitive();

  unsigned performBinding();

  typedef ClusterVectors::const_iterator cluster_iterator;
  cluster_iterator cluster_begin() const { return Clusters.begin(); }
  cluster_iterator cluster_end() const { return Clusters.end(); }

  CompGraphNode *getNode(DataflowInst Inst) const {
    std::map<DataflowInst, CompGraphNode*>::const_iterator I = InstMap.find(Inst);
    return I == InstMap.end() ? 0 : I->second;
  }

  bool hasbinding() const { return !Clusters.empty(); }

  void viewGraph();

  bool isBefore(CompGraphNode *Src, CompGraphNode *Dst);

  // Make the edge with default weight, we will udate the weight later.
  void makeEdge(CompGraphNode *Src, CompGraphNode *Dst) {
    // Make sure source is earlier than destination.
    if (!Src->IsTrivial && !Dst->IsTrivial && !isBefore(Src, Dst))
      std::swap(Dst, Src);

    Src->Succs.insert(Dst);
    Src->SuccCosts.insert(std::make_pair(Dst, CompGraphNode::Cost()));
    Dst->Preds.insert(Src);
  }
};
} // end namespace vast

namespace llvm {
using namespace vast;

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
