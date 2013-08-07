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

#ifndef COMPATIBILITY_GRAPH_H
#define COMPATIBILITY_GRAPH_H

#include "Dataflow.h"
#include "shang/FUInfo.h"

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
class VASTSelector;
class VASTSeqInst;
class VASTSeqValue;
class DominatorTree;
class CachedStrashTable;
class MinCostFlowSolver;

class CompGraphNode : public ilist_node<CompGraphNode> {
public:
  const unsigned Idx : 31;
  const unsigned IsTrivial : 1;
  const VFUs::FUTypes FUType;
  const unsigned FUCost;
  const DataflowInst Inst;

  struct Cost {
    int16_t ResourceCost;
    int16_t InterconnectCost;
    int16_t TimingPenalty;
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

  typedef std::map<const CompGraphNode*, float> CostVecTy;
  CostVecTy SuccCosts;

  static bool intersects(const SparseBitVector<> &LHSBits,
                         const SparseBitVector<> &RHSBits) {
    return LHSBits.intersects(RHSBits);
  }
protected:
  bool isCompatibleWithInterval(const CompGraphNode *RHS) const;
  bool isCompatibleWithStructural(const CompGraphNode *RHS) const;

  friend class CompGraphBase;
public:

  CompGraphNode()
    : Idx(0), IsTrivial(true), FUType(VFUs::Trivial), FUCost(0), Order(0),
      Inst() { }

  CompGraphNode(VFUs::FUTypes FUType, unsigned FUCost, unsigned Idx,
                DataflowInst Inst, ArrayRef<VASTSelector*> Sels)
    : Idx(Idx), IsTrivial(false), FUType(FUType), FUCost(FUCost),
      Order(UINT32_MAX), Inst(Inst), Sels(Sels.begin(), Sels.end()) {}

  BasicBlock *getDomBlock() const;

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

  bool isIntervalEmpty() const { return Defs.empty() && Reachables.empty(); }

  bool isDead() const { return Defs == Reachables; }

  bool isNeighbor(CompGraphNode *RHS) const {
    return Preds.count(RHS) || Succs.count(RHS);
  }

  bool countSuccessor(CompGraphNode *RHS) const {
    return Succs.count(RHS);
  }

  float getCostTo(const CompGraphNode *To) const;

  void setCost(const CompGraphNode *To, float Cost);

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
  typedef std::pair<NodeTy*, NodeTy*> EdgeType;

  typedef std::vector<EdgeType> ClusterType;
  typedef std::vector<ClusterType> ClusterVectors;

  typedef std::vector<EdgeType> EdgeVector;
protected:
  typedef ilist<NodeTy> NodeVecTy;
  // The dummy entry node of the graph.
  NodeTy Entry, Exit;
  // Nodes vector.
  NodeVecTy Nodes;
  std::map<VASTSelector*, CompGraphNode*> SelectorMap;
  std::set<VASTSelector*> BoundSels;

  // Due to CFG folding, there maybe more than one operation correspond to
  // the same LLVM Instruction. These operations operate on the same set of
  // registers, we need to avoid adding them more than once to the compatibility
  // graph.
  std::map<DataflowInst, CompGraphNode*> InstMap;

  // Try to bind the compatible edges together.
  std::map<EdgeType, EdgeVector> CompatibleEdges;

  DominatorTree &DT;
  CachedStrashTable &CST;
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

  virtual float computeCost(NodeTy *Src, NodeTy *Dst) const {
    return 0.0f;
  }

  virtual float compuateCommonFIBenefit(VASTSelector *Sel) const {
    return 0.0f;
  }

  virtual CompGraphNode *createNode(VFUs::FUTypes FUType, unsigned FUCost,
                                    unsigned Idx, DataflowInst Inst,
                                    ArrayRef<VASTSelector*> Sels) const {
    return new CompGraphNode(FUType, FUCost, Idx, Inst, Sels);
  }

  float computeIncreasedMuxPorts(VASTSelector *Src, VASTSelector *Dst) const;
  float computeIncreasedMuxPorts(CompGraphNode *Src, CompGraphNode *Dst) const;
  float compuateSavedResource(CompGraphNode *Src, CompGraphNode *Dst) const;

  void extractFaninNodes(NodeTy *N, std::set<NodeTy*> &Fanins) const;
  void extractFaninNodes(VASTSelector *Sel,
                         std::set<CompGraphNode*> &Fanins) const;
  void translateToCompNodes(std::set<VASTSeqValue*> &SVSet,
                            std::set<CompGraphNode*> &Fanins) const;
  void
  collectCompatibleEdges(NodeTy *Dst, NodeTy *Src, std::set<NodeTy*> &SrcFIs);
private:
  MinCostFlowSolver *MCF;
public:
  explicit CompGraphBase(DominatorTree &DT, CachedStrashTable &CST)
    : Entry(), Exit(), DT(DT), CST(CST), MCF(0) {
    initalizeDomTreeLevel();
  }

  virtual ~CompGraphBase();

  CompGraphNode *lookupNode(VASTSelector *Sel) const {
    std::map<VASTSelector*, NodeTy*>::const_iterator I = SelectorMap.find(Sel);
    return I != SelectorMap.end() ? I->second : 0;
  }

  NodeTy *addNewNode(VASTSeqInst *SeqInst);

  void addBoundSels(VASTSelector *Sel) {
    BoundSels.insert(Sel);
  }

  void setCommonFIBenefit();
  void setCommonFIBenefit(VASTSelector *Sel);

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

  void decomposeTrivialNodes();
  void computeCompatibility();
  void compuateEdgeCosts();
  void fixTransitive();

  unsigned performBinding();

  virtual float getEdgeConsistencyBenefit(EdgeType Edge, EdgeType FIEdge) const {
    return 0.0f;
  }

  typedef ClusterVectors::const_iterator cluster_iterator;
  cluster_iterator cluster_begin() const { return Clusters.begin(); }
  cluster_iterator cluster_end() const { return Clusters.end(); }

  CompGraphNode *getNode(DataflowInst Inst) const {
    std::map<DataflowInst, CompGraphNode*>::const_iterator I = InstMap.find(Inst);
    return I == InstMap.end() ? 0 : I->second;
  }

  bool hasbinding() const { return !Clusters.empty(); }

  void viewGraph();
  
  typedef std::map<EdgeType, EdgeVector>::const_iterator comp_edge_iterator;
  comp_edge_iterator comp_edge_begin() const { return CompatibleEdges.begin(); }
  comp_edge_iterator comp_edge_end() const { return CompatibleEdges.end(); }

  bool isBefore(CompGraphNode *Src, CompGraphNode *Dst);

  // Make the edge with default weight, we will udate the weight later.
  void makeEdge(CompGraphNode *Src, CompGraphNode *Dst) {
    // Make sure source is earlier than destination.
    if (!Src->IsTrivial && !Dst->IsTrivial && !isBefore(Src, Dst))
      std::swap(Dst, Src);

    Src->Succs.insert(Dst);
    Src->SuccCosts.insert(std::make_pair(Dst, 0.0f));
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
