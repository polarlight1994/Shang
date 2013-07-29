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
#include "CompGraph.h"

#include "shang/VASTExprBuilder.h"
#include "shang/VASTModule.h"
#include "shang/VASTModulePass.h"
#include "shang/Passes.h"

#include "llvm/Analysis/Dominators.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/PostOrderIterator.h"
#define  DEBUG_TYPE "shang-register-sharing"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumRegMerge, "Number of register pairs merged");

typedef std::map<unsigned, SparseBitVector<> > OverlappedMapTy;

namespace llvm {

class LICompGraph : public CompGraphBase {
public:
  explicit LICompGraph(DominatorTree *DT) : CompGraphBase(DT) {}

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
  
  static void setUpInterval(CompGraphNode *LI, SeqLiveVariables *LVS,
                            const OverlappedMapTy &OverlappedMap) {
    typedef VASTSelector::def_iterator def_iterator;
    for (CompGraphNode::sel_iterator I = LI->begin(), E = LI->end(); I != E; ++I) {
      VASTSelector *Sel = *I;
      for (def_iterator SI = Sel->def_begin(), SE = Sel->def_end();
           SI != SE; ++SI) {
        const SeqLiveVariables::VarInfo *LV = LVS->getVarInfo(*SI);
        setOverlappedSlots(LI->getDefs(), LV->Defs, OverlappedMap);
        setOverlappedSlots(LI->getDefs(),  LV->DefKills, OverlappedMap);

        LI->getReachables() |=  LV->Alives;
        LI->getReachables() |=  LV->Kills;
        LI->getReachables() |=  LV->DefKills;
      }
    }
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
    NodeTy *Node = new NodeTy(DomBlock, Sels);
    Nodes.push_back(Node);
    setUpInterval(Node, LVS, OverlappedMap);
    // And insert the node into the graph.
    for (NodeTy::iterator I = Entry.succ_begin(), E = Entry.succ_end();
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

  // TOOD: Add function: Rebuild graph.

};
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
  void mergeLI(CompGraphNode *From, CompGraphNode *To);
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

static void UpdateQ(CompGraphNode *N, CompGraphNode *P, CompGraphNode *&Q,
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
CompGraphNode *GetNeighborToCombine(CompGraphNode *P) {
  typedef CompGraphNode::iterator neighbor_it;

  unsigned MaxCommonNeighbors = 0;
  CompGraphNode *Q = 0;

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

void RegisterSharing::mergeLI(CompGraphNode *From, CompGraphNode *To) {
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
  CompGraphNode *P = 0;
  // Initialize MaxNeighborWeight to a nozero value so we can ignore the
  // trivial nodes.
  int MaxNeighborWeight = 1;

  for (LICompGraph::iterator I = G->begin(), E = G->end(); I != E; ++I) {
    CompGraphNode *N = I;
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
    typedef CompGraphNode::iterator neighbor_it;
    CompGraphNode *Q = GetNeighborToCombine(P);

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
