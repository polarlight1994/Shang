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

#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#define  DEBUG_TYPE "shang-register-sharing"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumRegMerge, "Number of register pairs merged");

namespace llvm {
class SeqLiveInterval {
  // The underlying data.
  VASTSeqValue *SV;
  SparseBitVector<> Alives;

  typedef SmallPtrSet<SeqLiveInterval*, 8> NodeVecTy;
  // Predecessors and Successors.
  NodeVecTy Preds, Succs;

  typedef std::map<const SeqLiveInterval*, int> WeightVecTy;
  WeightVecTy SuccWeights;

public:
  static const int HUGE_NEG_VAL = -1000000000;
  static const int TINY_VAL = 1;

  SeqLiveInterval(VASTSeqValue *SV, SeqLiveVariables *LVS) : SV(SV) {
    if (LVS == 0) return;

    typedef VASTSeqValue::const_iterator iterator;
    for (iterator DI = SV->begin(), DE = SV->end(); DI != DE; ++DI) {
      const SeqLiveVariables::VarInfo *LV = LVS->getVarInfo(*DI);
      Alives |= LV->Alives;
      Alives |= LV->Kills;
      Alives |= LV->DefKills;
    }
  }

  bool isTrivial() const { return SV == 0; }

  void dropAllEdges() {
    Preds.clear();
    Succs.clear();
    SuccWeights.clear();
  }

  VASTSeqValue *get() const { return SV; }
  VASTSeqValue *operator*() const { return get(); }
  VASTSeqValue *operator->() const { return get(); }

  //void print(raw_ostream &OS) const;
  //void dump() const;

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

  void merge(const SeqLiveInterval *RHS) {
    Alives |= RHS->Alives;
  }

  bool compatibleWith(const SeqLiveInterval *RHS) const {
    return SV->getBitWidth() == RHS->SV->getBitWidth()
           && !Alives.intersects(RHS->Alives);
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
      if (Succ->get()) {
        int Weigth = C(this->get(), Succ->get());
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

  // Make the edge with default weight, we will udate the weight later.
  static void MakeEdge(SeqLiveInterval *Src, SeqLiveInterval *Dst) {
    // Make sure source is earlier than destination.
    if (!Src->isTrivial() && !Dst->isTrivial() && Src->get() > Dst->get())
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
  typedef DenseMap<VASTSeqValue*, NodeTy*> NodeMapTy;
  // The dummy entry node of the graph.
  NodeTy Entry, Exit;
  // Nodes vector.
  NodeMapTy Nodes;

public:
  LICompGraph() : Entry(0, 0), Exit(0, 0) {}

  ~LICompGraph() {
    DeleteContainerSeconds(Nodes);
  }

  const NodeTy *getEntry() const { return &Entry; }
  const NodeTy *getExit() const { return &Exit; }

  typedef NodeTy::iterator iterator;

  // All nodes (except exit node) are successors of the entry node.
  iterator begin() { return Entry.succ_begin(); }
  iterator end()   { return Entry.succ_end(); }

  bool empty() const { return Entry.succ_empty(); }
  bool hasMoreThanOneNode() const { return Entry.num_succ() > 1; }

  NodeTy *operator[](VASTSeqValue *N) const { return Nodes.lookup(N); }

  NodeTy *GetOrCreateNode(VASTSeqValue *V, SeqLiveVariables *LVS) {
    assert(V && "Unexpected null pointer pass to GetOrCreateNode!");
    NodeTy *&Node = Nodes[V];
    // Create the node if it not exists yet.
    if (Node == 0) {
      Node = new NodeTy(V, LVS);
      // And insert the node into the graph.
      for (iterator I = begin(), E = end(); I != E; ++I) {
        NodeTy *Other = *I;

        // Make edge between compatible nodes.
        if (Node->compatibleWith(Other))
          NodeTy::MakeEdge(Node, Other);
      }

      // There will be always an edge from entry to a node
      // and an edge from node to exit.
      NodeTy::MakeEdge(&Entry, Node);
      NodeTy::MakeEdge(Node, &Exit);
    }

    return Node;
  }

  void deleteNode(NodeTy *N) {
    Nodes.erase(N->get());
    N->unlink();
    delete N;
  }

  void recomputeCompatibility() {
    Entry.dropAllEdges();
    Exit.dropAllEdges();

    typedef NodeMapTy::iterator node_iterator;
    for (node_iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I)
      I->second->dropAllEdges();

    for (node_iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
      NodeTy *Node = I->second;

      // And insert the node into the graph.
      for (iterator I = begin(), E = end(); I != E; ++I) {
        NodeTy *Other = *I;

        // Make edge between compatible nodes.
        if (Node->compatibleWith(Other))
          NodeTy::MakeEdge(Node, Other);
      }

      // There will always edge from entry to a node and from node to exit.
      NodeTy::MakeEdge(&Entry, Node);
      NodeTy::MakeEdge(Node, &Exit);
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
    return Node->isTrivial() ? "<null>" : std::string(Node->get()->getName());
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

  RegisterSharing() : VASTModulePass(ID), VM(0), Builder(0), G(0), LVS(0) {
    initializeRegisterSharingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);

    AU.addRequired<SeqLiveVariables>();
    //AU.addPreserved<SeqLiveVariables>();
  }

  bool runOnVASTModule(VASTModule &VM);

  bool performRegisterSharing();
  void mergeLI(SeqLiveInterval *From, SeqLiveInterval *To);
  void mergeSeqVal(VASTSeqValue *ToV, VASTSeqValue *FromV);
};
}

INITIALIZE_PASS_BEGIN(RegisterSharing, "shang-register-sharing",
                      "Share the registers in the design", false, true)
  INITIALIZE_PASS_DEPENDENCY(SeqLiveVariables)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
INITIALIZE_PASS_END(RegisterSharing, "shang-register-sharing",
                    "Share the registers in the design", false, true)

Pass *llvm::createRegisterSharingPass() {
  return new RegisterSharing();
}

char RegisterSharing::ID = 0;

bool RegisterSharing::runOnVASTModule(VASTModule &VM) {
  LVS = &getAnalysis<SeqLiveVariables>();
  this->VM = &VM;

  LICompGraph G;
  this->G = &G;

  MinimalExprBuilderContext C(VM);
  VASTExprBuilder Builder(C);
  this->Builder = &Builder;

  typedef VASTModule::seqval_iterator iterator;

  for (iterator I = VM.seqval_begin(), IE = VM.seqval_end(); I != IE; ++I) {
    if (I->getValType() != VASTSeqValue::Data) continue;
    
    G.GetOrCreateNode(I, LVS);
  }

  while (performRegisterSharing())
    ;

  return false;
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
  if (Q && Q->degree() < P->degree() && Q->get() > N->get()) return;

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

  assert(Q && Q->get() && "Unexpected Q is null!");

  return Q;
}

void RegisterSharing::mergeLI(SeqLiveInterval *From, SeqLiveInterval *To) {

  assert(From->compatibleWith(To) && "Cannot merge incompatible LiveIntervals!");
  VASTSeqValue *FromV = From->get(), *ToV = To->get();
  DEBUG(dbgs() << "Merge " << FromV->getName() << " to " << ToV->getName()
               << '\n');

  mergeSeqVal(ToV, FromV);

  // Merge the interval.
  To->merge(From);

  // Remove From since it is already merged into others.
  G->deleteNode(From);

  ++NumRegMerge;
}

void RegisterSharing::mergeSeqVal(VASTSeqValue *ToV, VASTSeqValue *FromV) {
  typedef VASTSeqValue::iterator iterator;
  // Create a wrapper wire to break the cycle which will generated by the later
  // replacement.
  for (iterator I = ToV->begin(), E = ToV->end(); I != E; ++I) {
    VASTLatch L = *I;
    VASTValPtr Src = L;

    if (Src != FromV) continue;;

    unsigned BitWidth = Src->getBitWidth();
    Src = VM->createWrapperWire(ToV->getName(), BitWidth, ToV);
    L.replaceUsedBy(Src);
  }

  Builder->replaceAllUseWith(FromV, ToV);

  SmallVector<VASTSeqOp*, 8> DeadOps;

  // Clone the assignments targeting FromV, and change the target to ToV.
  for (iterator I = FromV->begin(), E = FromV->end(); I != E; ++I) {
    VASTLatch L = *I;
    VASTSeqOp *Op = L.Op;

    VASTSeqInst *NewInst
      = VM->lauchInst(Op->getSlot(), Op->getPred(), Op->getNumSrcs(),
      Op->getValue(), cast<VASTSeqInst>(Op)->getSeqOpType());
    typedef VASTSeqOp::op_iterator iterator;

    for (unsigned i = 0, e = Op->getNumSrcs(); i < e; ++i) {
      VASTSeqValue *Dst = Op->getSrc(i).getDst();
      // Redirect the target of the assignment.
      if (Dst == FromV) Dst = ToV;

      VASTValPtr Src = Op->getSrc(i);
      // Create a wrapper wire to break the cycle.
      if (Src == Dst) {
        unsigned BitWidth = Src->getBitWidth();
        Twine WrapperName = Twine(Dst->getName()) + "_Wrapper";
        Src = VM->createWrapperWire(WrapperName, BitWidth, Src);
      }

      NewInst->addSrc(Src, i, i < Op->getNumDefs(), Dst);
    }

    // Also transfer the live variable information.
    VASTLatch NewL = NewInst->getSrc(L.No);
    //LVS->transferVarInfo(L, NewL);

    // Rememeber the dead ops.
    DeadOps.push_back(Op);
  }

  // Erase the dead ops.
  while (!DeadOps.empty()) {
    VASTSeqOp *Op = DeadOps.pop_back_val();
    Op->getSlot()->removeOp(Op);
    VM->eraseSeqOp(Op);
  }
}

bool RegisterSharing::performRegisterSharing() {
  // G.updateEdgeWeight(UseClosure);
  
  // 1. Pick a node with neighbor weight and call it P.
  SeqLiveInterval *P = 0;
  // Initialize MaxNeighborWeight to a nozero value so we can ignore the
  // trivial nodes.
  int MaxNeighborWeight = 1;

  for (LICompGraph::iterator I = G->begin(), E = G->end(); I != E; ++I) {
    SeqLiveInterval *N = *I;
    // Ignore the Nodes that has only virtual neighbors.
    if (N->isTrivial()) continue;

    int CurrentNeighborWeight = N->computeNeighborWeight();

    // Do not update P if N has smaller neighbor weight.
    if (CurrentNeighborWeight < MaxNeighborWeight) continue;

    // If N and P have the same neighbor weight, pick the one with bigger Id.
    if (P && CurrentNeighborWeight == MaxNeighborWeight
        && N->get() < P->get())
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
    if (Q->get() >= P->get()) {
      std::swap(P, Q);
    }

    P->deleteUncommonEdges(Q);

    // Merge QV into PV and delete Q.
    mergeLI(Q, P);
  }

  G->recomputeCompatibility();
  return true;
}
