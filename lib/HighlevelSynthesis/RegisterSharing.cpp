//===- RegisterSharing.cpp - Share the Registers in the Design --*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the register sharing based on the live variable analysis.
// The sharing algorithm is based on clique partitioning.
// You can find the original description of the clique partitioning algorithm
// in paper:
//   New Efficient Clique Partitioning Algorithms for Register-Transfer Synthesis
//   of Data Paths
//   Jong Tae Kim and Dong Ryeol Shin, 2001
//
//===----------------------------------------------------------------------===//
#include "SeqLiveVariables.h"

#include "shang/VASTExprBuilder.h"
#include "shang/VASTModule.h"
#include "shang/VASTModulePass.h"
#include "shang/Passes.h"

#include "llvm/Analysis/Dominators.h"

#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#define  DEBUG_TYPE "shang-register-sharing"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumRegMerge, "Number of register pairs merged");

typedef std::map<unsigned, SparseBitVector<> > OverlappedMapTy;

namespace llvm {
class SeqLiveInterval : public ilist_node<SeqLiveInterval> {
  BasicBlock *DomBlock;
  // The underlying data.
  SmallVector<VASTSelector*, 3> Sels;
  SparseBitVector<> Defs;
  SparseBitVector<> Alives;
  SparseBitVector<> Kills;

  typedef SmallPtrSet<SeqLiveInterval*, 8> NodeVecTy;
  // Predecessors and Successors.
  NodeVecTy Preds, Succs;

  typedef std::map<const SeqLiveInterval*, float> WeightVecTy;
  WeightVecTy SuccWeights;

  static bool intersects(const SparseBitVector<> &LHSBits,
                         const SparseBitVector<> &RHSBits) {
    return LHSBits.intersects(RHSBits);
  }

  static void
  setOverlappedSlots(SparseBitVector<> &LHS, const SparseBitVector<> &RHS,
                     const OverlappedMapTy &OverlappedMap) {
    typedef SparseBitVector<>::iterator iterator;
    for (iterator I = RHS.begin(), E = RHS.end(); I != E; ++I) {
      unsigned Slot = *I;
      OverlappedMapTy::const_iterator At = OverlappedMap.find(Slot);
      assert(At != OverlappedMap.end() && "Overlapped slots not found!");
      LHS |= At->second;
    }
  }

public:
  static const int HUGE_NEG_VAL = -1000000000;
  static const int TINY_VAL = 1;

  SeqLiveInterval() { }

  SeqLiveInterval(ArrayRef<VASTSelector*> Sels, BasicBlock *DomBlock)
    : DomBlock(DomBlock), Sels(Sels.begin(), Sels.end()) {}

  typedef SmallVectorImpl<VASTSelector*>::iterator sel_iterator;
  sel_iterator begin() { return Sels.begin(); }
  sel_iterator end() { return Sels.end(); }

  typedef SmallVectorImpl<VASTSelector*>::const_iterator const_sel_iterator;
  const_sel_iterator begin() const { return Sels.begin(); }
  const_sel_iterator end() const { return Sels.end(); }

  size_t size() const { return Sels.size(); }
  VASTSelector *getSelector(unsigned Idx) const { return Sels[Idx]; }

  void
  setUpInterval(SeqLiveVariables *LVS, const OverlappedMapTy &OverlappedMap) {
    typedef VASTSelector::def_iterator def_iterator;
    for (sel_iterator I = begin(), E = end(); I != E; ++I) {
      VASTSelector *Sel = *I;
      for (def_iterator SI = Sel->def_begin(), SE = Sel->def_end();
           SI != SE; ++SI) {
        const SeqLiveVariables::VarInfo *LV = LVS->getVarInfo(*SI);
        setOverlappedSlots(Defs, LV->Defs, OverlappedMap);
        setOverlappedSlots(Alives,  LV->Alives, OverlappedMap);
        setOverlappedSlots(Kills,  LV->Kills, OverlappedMap);

        setOverlappedSlots(Defs,  LV->DefKills, OverlappedMap);
        setOverlappedSlots(Kills,  LV->DefKills, OverlappedMap);
      }
    }
  }

  bool isTrivial() const { return Sels.empty(); }

  void dropAllEdges() {
    Preds.clear();
    Succs.clear();
    SuccWeights.clear();
  }

  void print(raw_ostream &OS) const {
    ::dump(Alives, OS);
  }

  void dump() const { print(dbgs()); }

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

  void merge(const SeqLiveInterval *RHS, DominatorTree *DT) {
    Defs        |= RHS->Defs;
    Alives      |= RHS->Alives;
    Kills       |= RHS->Kills;
    DomBlock = DT->findNearestCommonDominator(DomBlock, RHS->DomBlock);
  }

  bool isCompatibleWith(const SeqLiveInterval *RHS) const {
    if (Sels.size() != RHS->Sels.size())
      return false;

    // Bitwidth should be the same.
    for (unsigned i = 0, e = size(); i != e; ++i)
      if (getSelector(i)->getBitWidth() != RHS->getSelector(i)->getBitWidth())
        return false;

    // Defines should not intersects.
    if (intersects(Defs, RHS->Defs))
      return false;

    // Defines and alives should not intersects.
    if (intersects(Defs, RHS->Alives))
      return false;

    if (intersects(Alives, RHS->Defs))
      return false;

    // Alives should not intersects.
    if (intersects(Alives, RHS->Alives))
      return false;

    // Kills should not intersects.
    // Not need to check the DefKills because they are a part of Defs.
    if (intersects(Kills, RHS->Kills))
      return false;

    // Kills and Alives should not intersects.
    // TODO: Ignore the dead slots.
    if (intersects(Kills, RHS->Alives))
      return false;

    if (intersects(Alives, RHS->Kills))
      return false;

    return true;
  }

  bool isNeighbor(SeqLiveInterval *RHS) const {
    return Preds.count(RHS) || Succs.count(RHS);
  }

  int getWeightTo(const SeqLiveInterval *To) const {
    return SuccWeights.find(To)->second;
  }

  int computeNeighborWeight(SeqLiveInterval *RHS = 0) const {
    int Weight = 0;

    for (iterator I = pred_begin(), E = pred_end(); I != E; ++I) {
      SeqLiveInterval *NP = *I;
      if (NP->isTrivial()) continue;

      // RHS is null means we want to compute all neighbor weight, otherwise
      // means we want to compute the common neighbor weight only.
      if (RHS == 0 || NP->isNeighbor(RHS)) Weight += NP->getWeightTo(this);
    }

    for (iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
      SeqLiveInterval *NS = *I;
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
  void unlinkSucc(SeqLiveInterval *Succ) {
    bool deleted = Succs.erase(Succ);
    assert(deleted && "Succ is not the successor of this!");
    SuccWeights.erase(Succ);

    // Current node is not the predecessor of succ node too.
    deleted = Succ->Preds.erase(this);
    assert(deleted && "this is not the predecessor of succ!");
    (void) deleted;
  }

  // Unlink the Pred from current node.
  void unlinkPred(SeqLiveInterval *Pred) {
    bool deleted = Preds.erase(Pred);
    assert(deleted && "Pred is not the predecessor of this!");

    // Current node is not the successor of pred node too.
    deleted = Pred->Succs.erase(this);
    Pred->SuccWeights.erase(this);
    assert(deleted && "this is not the successor of Pred!");
    (void) deleted;
  }

  void deleteUncommonEdges(SeqLiveInterval *RHS) {
    // Delete edge from P and Q that are not connected to their common neighbors.
    SmallVector<SeqLiveInterval*, 8> ToUnlink;
    // Unlink preds.
    for (iterator I = pred_begin(), E = pred_end(); I != E; ++I) {
      SeqLiveInterval *N = *I;
      if (!RHS->isNeighbor(N)) ToUnlink.push_back(N);
    }

    while (!ToUnlink.empty())
      unlinkPred(ToUnlink.pop_back_val());

    // Unlink succs.
    for (iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
      SeqLiveInterval *N = *I;
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
    SmallVector<SeqLiveInterval*, 8> SuccToUnlink;
    for (iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
      SeqLiveInterval *Succ = *I;
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

  static bool lt(SeqLiveInterval *Src, SeqLiveInterval *Dst, DominatorTree *DT) {
    assert(!Src->isTrivial() && !Dst->isTrivial() && "Unexpected trivial node!");
    if (DT->properlyDominates(Src->DomBlock, Dst->DomBlock))
      return true;

    if (DT->properlyDominates(Dst->DomBlock, Src->DomBlock))
      return true;

    return Src < Dst;
  }

  // Make the edge with default weight, we will udate the weight later.
  static
  void MakeEdge(SeqLiveInterval *Src, SeqLiveInterval *Dst, DominatorTree *DT) {
    // Make sure source is earlier than destination.
    if (!Src->isTrivial() && !Dst->isTrivial() && !lt(Src, Dst, DT))
      std::swap(Dst, Src);

    Src->Succs.insert(Dst);
    Src->SuccWeights.insert(std::make_pair(Dst, TINY_VAL));
    Dst->Preds.insert(Src);
  }
};

template<> struct GraphTraits<SeqLiveInterval*> {
  typedef SeqLiveInterval NodeType;
  typedef NodeType::iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->succ_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->succ_end();
  }
};

class LICompGraph {
public:
  typedef SeqLiveInterval NodeTy;

private:
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
  LICompGraph(DominatorTree *DT) : Entry(), Exit(), DT(DT) {}

  ~LICompGraph() {}

  const NodeTy *getEntry() const { return &Entry; }
  const NodeTy *getExit() const { return &Exit; }

  typedef NodeVecTy::iterator iterator;

  // All nodes (except exit node) are successors of the entry node.
  iterator begin() { return Nodes.begin(); }
  iterator end()   { return Nodes.end(); }

  bool hasMoreThanOneNode() const {
    return !Nodes.empty() && &Nodes.front() != &Nodes.back();
  }

  NodeTy *
  getOrCreateNode(VASTSeqInst *SeqInst, SeqLiveVariables *LVS,
                  const std::map<unsigned, SparseBitVector<> > &OverlappedMap) {
    assert(SeqInst && "Unexpected null pointer pass to GetOrCreateNode!");

    SmallVector<VASTSelector*, 4> Sels;
    assert(SeqInst->getNumDefs() == SeqInst->num_srcs()
           && "Expected all assignments are definitions!");
    for (unsigned i = 0; i < SeqInst->num_srcs(); ++i)
      Sels.push_back(SeqInst->getSrc(i).getSelector());

    // Create the node if it not exists yet.
    BasicBlock *DomBlock = cast<Instruction>(SeqInst->getValue())->getParent();
    NodeTy *Node = new NodeTy(Sels, DomBlock);
    Nodes.push_back(Node);
    Node->setUpInterval(LVS, OverlappedMap);
    // And insert the node into the graph.
    for (NodeTy::iterator I = Entry.succ_begin(), E = Entry.succ_begin();
         I != E; ++I) {
      NodeTy *Other = *I;

      // Make edge between compatible nodes.
      if (Node->isCompatibleWith(Other))
        NodeTy::MakeEdge(Node, Other, DT);
    }

    // There will be always an edge from entry to a node
    // and an edge from node to exit.
    NodeTy::MakeEdge(&Entry, Node, 0);
    NodeTy::MakeEdge(Node, &Exit, 0);

    return Node;
  }

  void merge(NodeTy *From, NodeTy *To) {
    To->merge(From, DT);
    deleteNode(From);
  }

  void recomputeCompatibility() {
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

  void verifyTransitive() {
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

  template<class CompEdgeWeight>
  void updateEdgeWeight(CompEdgeWeight &C) {
    for (iterator I = begin(), E = end(); I != E; ++I)
      (*I)->updateEdgeWeight(C);
  }

  // TOOD: Add function: Rebuild graph.

  void viewGraph();
};

template <> struct GraphTraits<LICompGraph*>
  : public GraphTraits<SeqLiveInterval*> {
  
  typedef LICompGraph::iterator nodes_iterator;
  static nodes_iterator nodes_begin(LICompGraph *G) {
    return G->begin();
  }

  static nodes_iterator nodes_end(LICompGraph *G) {
    return G->end();
  }
};

template<> struct DOTGraphTraits<LICompGraph*> : public DefaultDOTGraphTraits{
  typedef LICompGraph GraphTy;
  typedef GraphTy::NodeTy NodeTy;
  typedef NodeTy::iterator NodeIterator;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getEdgeSourceLabel(const NodeTy *Node,NodeIterator I){
    return itostr(Node->getWeightTo(*I));
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

void LICompGraph::viewGraph() {
  ViewGraph(this, "CompatibilityGraph");
}

namespace {
struct RegisterSharing : public VASTModulePass {
  static char ID;
  VASTModule *VM;
  VASTExprBuilder *Builder;
  LICompGraph *G;
  SeqLiveVariables *LVS;
  std::map<unsigned, SparseBitVector<> > OverlappedMap;

  RegisterSharing() : VASTModulePass(ID), VM(0), Builder(0), G(0), LVS(0) {
    initializeRegisterSharingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<DominatorTree>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);

    AU.addPreservedID(STGDistancesID);

    AU.addRequired<SeqLiveVariables>();
    //AU.addPreserved<SeqLiveVariables>();
  }

  void initializeOverlappedSlots();

  bool runOnVASTModule(VASTModule &VM);

  bool performRegisterSharing();
  void mergeLI(SeqLiveInterval *From, SeqLiveInterval *To);
  void mergeSelector(VASTSelector *ToV, VASTSelector *FromV);
};
}

INITIALIZE_PASS_BEGIN(RegisterSharing, "shang-register-sharing",
                      "Share the registers in the design", false, true)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
  INITIALIZE_PASS_DEPENDENCY(SeqLiveVariables)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
INITIALIZE_PASS_END(RegisterSharing, "shang-register-sharing",
                    "Share the registers in the design", false, true)

Pass *llvm::createRegisterSharingPass() {
  return new RegisterSharing();
}

char RegisterSharing::ID = 0;

static VASTSeqInst *IsSharingCandidate(VASTSeqOp *Op) {
  VASTSeqInst *Inst = dyn_cast<VASTSeqInst>(Op);

  if (!Inst) return 0;

  if (!isChainingCandidate(Inst->getValue())) return 0;

  // Do not share the enable as well.
  return Inst;
}

// Build the transitive closure of the overlap slots.
void RegisterSharing::initializeOverlappedSlots() {
  typedef VASTModule::slot_iterator slot_iterator;

  SmallPtrSet<VASTSlot*, 8> Visited;
  SmallVector<std::pair<VASTSlot*, VASTSlot::succ_iterator>, 4> WorkStack;

  for (slot_iterator I = VM->slot_begin(), E = VM->slot_end(); I != E; ++I) {
    VASTSlot *S = I;
    SparseBitVector<> &Overlapped = OverlappedMap[S->SlotNum];
    Visited.clear();
    WorkStack.clear();
    WorkStack.push_back(std::make_pair(S, S->succ_begin()));
    while (!WorkStack.empty()) {
      VASTSlot *Cur = WorkStack.back().first;
      VASTSlot::succ_iterator ChildIt = WorkStack.back().second;

      if (ChildIt == Cur->succ_end()) {
        Overlapped.set(Cur->SlotNum);
        WorkStack.pop_back();
        continue;
      }

      VASTSlot::EdgePtr Edge = *ChildIt;
      ++WorkStack.back().second;
      VASTSlot *Child = Edge;

      // Skip the children require 1-distance edges to be reached.
      if (Edge.getDistance()) continue;

      // Do not visit a node twice.
      if (!Visited.insert(Child)) continue;

      WorkStack.push_back(std::make_pair(Child, Child->succ_begin()));
    }
  }
}

bool RegisterSharing::runOnVASTModule(VASTModule &VM) {
  DominatorTree &DT = getAnalysis<DominatorTree>();
  LVS = &getAnalysis<SeqLiveVariables>();
  this->VM = &VM;

  LICompGraph G(&DT);
  this->G = &G;

  MinimalExprBuilderContext C(VM);
  VASTExprBuilder Builder(C);
  this->Builder = &Builder;

  initializeOverlappedSlots();

  typedef VASTModule::seqop_iterator iterator;

  // Due to CFG folding, there maybe more than one operation correspond to
  // the same LLVM Instruction. These operations operate on the same set of
  // registers, we need to avoid adding them more than once to the compatibility
  // graph.
  std::set<Value*> VisitedInst;

  for (iterator I = VM.seqop_begin(), IE = VM.seqop_end(); I != IE; ++I) {
    VASTSeqOp *Op = I;
    if (VASTSeqInst *Inst = IsSharingCandidate(Op)) {
      if (!VisitedInst.insert(Inst->getValue()).second)
        continue;

      G.getOrCreateNode(Inst, LVS, OverlappedMap);
    }
  }
#ifndef NDEBUG
  G.verifyTransitive();
#endif

  while (performRegisterSharing()) {
#ifndef NDEBUG
    G.verifyTransitive();
#endif
  }

  OverlappedMap.clear();
  return true;
}

static void UpdateQ(SeqLiveInterval *N, SeqLiveInterval *P, SeqLiveInterval *&Q,
                    unsigned &MaxCommon) {
  unsigned NCommonNeighbors = N->computeNeighborWeight(P);
  // 2. Pick a neighbor of p, q, such that the number of common neighbor is
  //    maximum
  if (NCommonNeighbors == 0 || NCommonNeighbors < MaxCommon) return;

  // Tie-breaking: Select q such that the node degree of q is minimum.
  if (Q && NCommonNeighbors == MaxCommon && Q->degree() < N->degree())
    return;

  // If they have the same neighbors weight, pick the node with bigger Id.
  if (Q && Q->degree() < P->degree() && Q > N) return;

  MaxCommon = NCommonNeighbors;
  Q = N;
}

static
SeqLiveInterval *GetNeighborToCombine(SeqLiveInterval *P) {
  typedef SeqLiveInterval::iterator neighbor_it;

  unsigned MaxCommonNeighbors = 0;
  SeqLiveInterval *Q = 0;

  for (neighbor_it I = P->pred_begin(), E = P->pred_end(); I != E; ++I) {
    if ((*I)->isTrivial()) continue;

    UpdateQ(*I, P, Q, MaxCommonNeighbors);
  }

  for (neighbor_it I = P->succ_begin(), E = P->succ_end(); I != E; ++I) {
    if ((*I)->isTrivial()) continue;

    UpdateQ(*I, P, Q, MaxCommonNeighbors);
  }

  assert(Q && "Unexpected Q is null!");

  return Q;
}

void RegisterSharing::mergeLI(SeqLiveInterval *From, SeqLiveInterval *To) {
  assert(From->isCompatibleWith(To)
         && "Cannot merge incompatible LiveIntervals!");
  for (unsigned i = 0, e = From->size(); i != e; ++i)
    mergeSelector(To->getSelector(i), From->getSelector(i));

  // Remove From since it is already merged into others.
  G->merge(From, To);

  ++NumRegMerge;
}

void RegisterSharing::mergeSelector(VASTSelector *To, VASTSelector *From) {
  SmallVector<VASTSeqOp*, 8> DeadOps;

  while (!From->def_empty())
    (*From->def_begin())->changeSelector(To);

  // Clone the assignments targeting FromV, and change the target to ToV.
  typedef VASTSelector::iterator iterator;
  for (iterator I = From->begin(), E = From->end(); I != E; ++I) {
    VASTLatch L = *I;
    VASTSeqOp *Op = L.Op;

    VASTSeqInst *NewInst
      = VM->lauchInst(Op->getSlot(), Op->getGuard(), Op->num_srcs(),
                      Op->getValue(), cast<VASTSeqInst>(Op)->isLatch());
    typedef VASTSeqOp::op_iterator iterator;

    for (unsigned i = 0, e = Op->num_srcs(); i < e; ++i) {
      VASTSeqValue *DstVal = Op->getSrc(i).getDst();
      VASTSelector *DstSel = Op->getSrc(i).getSelector();
      // Redirect the target of the assignment.
      if (DstSel == From) DstSel = To;

      VASTValPtr Src = Op->getSrc(i);
      NewInst->addSrc(Src, i, DstSel, DstVal);
    }

    // Rememeber the dead ops.
    DeadOps.push_back(Op);
  }

  // Erase the dead ops.
  while (!DeadOps.empty()) {
    VASTSeqOp *Op = DeadOps.pop_back_val();
    VM->eraseSeqOp(Op);
  }

  VM->eraseSelector(From);
}

bool RegisterSharing::performRegisterSharing() {
  // G.updateEdgeWeight(UseClosure);
  
  // 1. Pick a node with neighbor weight and call it P.
  SeqLiveInterval *P = 0;
  // Initialize MaxNeighborWeight to a nozero value so we can ignore the
  // trivial nodes.
  int MaxNeighborWeight = 1;

  for (LICompGraph::iterator I = G->begin(), E = G->end(); I != E; ++I) {
    SeqLiveInterval *N = I;
    // Ignore the Nodes that has only virtual neighbors.
    if (N->isTrivial()) continue;

    int CurrentNeighborWeight = N->computeNeighborWeight();

    // Do not update P if N has smaller neighbor weight.
    if (CurrentNeighborWeight < MaxNeighborWeight) continue;

    // If N and P have the same neighbor weight, pick the one with bigger Id.
    if (P && CurrentNeighborWeight == MaxNeighborWeight && N < P)
      continue;

    P = N;
    MaxNeighborWeight = CurrentNeighborWeight;
  }

  if (P == 0 || P->degree() <= 2) return false;

  DEBUG(G->viewGraph());

  // If P has any no-virtual neighbor.
  while (P->degree() > 2) {
    typedef SeqLiveInterval::iterator neighbor_it;
    SeqLiveInterval *Q = GetNeighborToCombine(P);

    // Combine P and Q and call it P, Make sure Q is before P.
    if (Q >= P) {
      std::swap(P, Q);
    }

    P->deleteUncommonEdges(Q);

    // Merge QV into PV and delete Q.
    mergeLI(Q, P);
  }

  G->recomputeCompatibility();
  return true;
}
