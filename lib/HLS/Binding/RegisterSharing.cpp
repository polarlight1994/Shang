//===- RegisterSharing.cpp - Share the Registers in the Design --*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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
#include "PreSchedBinding.h"

#include "vast/Strash.h"
#include "vast/VASTMemoryBank.h"
#include "vast/VASTExprBuilder.h"
#include "vast/VASTModule.h"
#include "vast/VASTModulePass.h"
#include "vast/Passes.h"

#include "llvm/Analysis/Dominators.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/PostOrderIterator.h"
#define  DEBUG_TYPE "shang-register-sharing"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumRegMerge, "Number of register pairs merged");
STATISTIC(NumInconsistent, "Number of operation/variable pairs that do not have "
                           "the same compatibility indicated by the schedule "
                           "independent binding");

typedef std::map<unsigned, SparseBitVector<> > OverlappedMapTy;

namespace {
class VASTCompGraph : public CompGraphBase {
  typedef CompGraphNode NodeTy;

public:
  VASTCompGraph(DominatorTree &DT)
    : CompGraphBase(DT) {}

  float computeCost(const CompGraphNode *Src, const CompGraphNode *Dst) const {
    const NodeTy::Cost &Cost = Src->getCostTo(Dst);
    float MergedDeltas = Cost.getMergedDetaBenefit();
    float CurrentCost = Cost.InterconnectCost - MergedDeltas;

    return CurrentCost;
  }
};
}

namespace {
struct RegisterSharing : public VASTModulePass {
  static char ID;
  SeqLiveVariables *LVS;
  std::map<unsigned, SparseBitVector<> > OverlappedMap;

  RegisterSharing() : VASTModulePass(ID), LVS(0) {
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

    AU.addRequired<CombPatternTable>();
    // AU.addRequired<PreSchedBinding>();
  }

  void initializeOverlappedSlots(VASTModule &VM);

  void setOverlappedSlots(SparseBitVector<> &LHS,
                          const SparseBitVector<> &RHS) const {
    typedef SparseBitVector<>::iterator iterator;
    for (iterator I = RHS.begin(), E = RHS.end(); I != E; ++I) {
      unsigned Slot = *I;
      OverlappedMapTy::const_iterator At = OverlappedMap.find(Slot);
      assert(At != OverlappedMap.end() && "Overlapped slots not found!");
      LHS |= At->second;
    }
  }

  void setUpInterval(CompGraphNode *LI) {
    typedef VASTSelector::def_iterator def_iterator;
    typedef CompGraphNode::sel_iterator sel_iterator;
    for (sel_iterator I = LI->begin(), E = LI->end(); I != E; ++I) {
      VASTSelector *Sel = *I;
      for (def_iterator SI = Sel->def_begin(), SE = Sel->def_end();
           SI != SE; ++SI) {
        const SeqLiveVariables::VarInfo *LV = LVS->getVarInfo(*SI);
        setOverlappedSlots(LI->getDefs(), LV->Defs);
        setOverlappedSlots(LI->getDefs(),  LV->DefKills);

        LI->getReachables() |=  LV->Alives;
        LI->getReachables() |=  LV->Kills;
        LI->getReachables() |=  LV->DefKills;
      }
    }
  }

  typedef CompGraphBase::ClusterType ClusterType;
  typedef CompGraphBase::ClusterVectors ClusterVectors;

  bool runOnVASTModule(VASTModule &VM);

  void checkConsistencyAgainstPSB(VASTCompGraph &G);
  void checkConsistencyAgainstPSB(const ClusterType &Cluster, VASTCompGraph &G);

  bool performRegisterSharing();
  void mergeLI(CompGraphNode *From, CompGraphNode *To, VASTModule &VM);
  void mergeSelector(VASTSelector *ToV, VASTSelector *FromV,
                     VASTModule &VM);
};
}

INITIALIZE_PASS_BEGIN(RegisterSharing, "shang-register-sharing",
                      "Share the registers in the design", false, true)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
  INITIALIZE_PASS_DEPENDENCY(SeqLiveVariables)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(CombPatternTable)
  INITIALIZE_PASS_DEPENDENCY(PreSchedBinding)
INITIALIZE_PASS_END(RegisterSharing, "shang-register-sharing",
                    "Share the registers in the design", false, true)

Pass *vast::createRegisterSharingPass() {
  return new RegisterSharing();
}

char RegisterSharing::ID = 0;

// Build the transitive closure of the overlap slots.
void RegisterSharing::initializeOverlappedSlots(VASTModule &VM) {
  typedef VASTModule::slot_iterator slot_iterator;

  SmallPtrSet<VASTSlot*, 8> Visited;
  SmallVector<std::pair<VASTSlot*, VASTSlot::succ_iterator>, 4> WorkStack;

  for (slot_iterator I = VM.slot_begin(), E = VM.slot_end(); I != E; ++I) {
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

void RegisterSharing::checkConsistencyAgainstPSB(const ClusterType &Cluster,
                                                 VASTCompGraph &G) {
  unsigned Id = 0;
  for (unsigned i = 0; i < Cluster.size(); ++i) {
    CompGraphNode *Src = G.getNode(Cluster[i]->Inst);
    // A node maybe eliminated by CFG folding or chaining.
    if (Src == 0)
      continue;

    if (Id == 0)
      Id = Src->getBindingIdx();

    for (unsigned j = i + 1; j < Cluster.size(); ++j) {
      CompGraphNode *Dst = G.getNode(Cluster[j]->Inst);
      if (Dst == 0)
        continue;

      // TODO: Add reduce the cost between src and dst to produce a consistent
      // binding?
      if (Src->isNeighbor(Dst)) {
        Dst->setBindingIdx(Id);
        continue;
      }

      ++NumInconsistent;
    }
  }
}

void RegisterSharing::checkConsistencyAgainstPSB(VASTCompGraph &G) {
  PreSchedBinding &PSB = getAnalysis<PreSchedBinding>();
  typedef CompGraphBase::cluster_iterator cluster_iterator;
  for (cluster_iterator I = PSB->cluster_begin(), E = PSB->cluster_end();
       I != E; ++I) {
    checkConsistencyAgainstPSB(*I, G);
  }
}

bool RegisterSharing::runOnVASTModule(VASTModule &VM) {
  typedef VASTModule::selector_iterator selector_iterator;
  // Clear up all MUX before we perform sharing.
  for (selector_iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
    I->dropMux();

  LVS = &getAnalysis<SeqLiveVariables>();

  VASTCompGraph G(getAnalysis<DominatorTree>());

  initializeOverlappedSlots(VM);

  typedef VASTModule::seqop_iterator iterator;

  // TODO: Use multimap.

  for (iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I) {
    VASTSeqOp *Op = I;

    VASTSeqInst *Inst = dyn_cast<VASTSeqInst>(Op);

    if (Inst == 0 || !Inst->isBindingCandidate()) {
      G.addBoundNode(Op);
      continue;
    }

    CompGraphNode *N = G.addNewNode(Inst);
    if (N->isIntervalEmpty())
      setUpInterval(N);

    N->updateOrder(Inst->getSlot()->SlotNum);
  }

  G.decomposeTrivialNodes();
  G.computeCompatibility();
  G.fixTransitive();

  G.initializeCosts(getAnalysis<CombPatternTable>());

  //checkConsistencyAgainstPSB(G);

  unsigned NumFU = G.performBinding();

  if (NumFU == 0)
    return false;

  typedef VASTCompGraph::cluster_iterator cluster_iterator;
  for (cluster_iterator I = G.cluster_begin(), E = G.cluster_end(); I != E; ++I)
  {
    const ClusterType &Cluster = *I;
    CompGraphNode *Head = I->front();

    typedef ClusterType::const_iterator iterator;
    for (iterator CI = Cluster.begin() + 1, CE = Cluster.end(); CI != CE; ++CI) {
      CompGraphNode *Node = *CI;
      mergeLI(Node, Head, VM);

      // Remove it from the compatibility graph since it is already merged into
      // others.
      G.merge(Node, Head);
    }
  }

  OverlappedMap.clear();
  return true;
}

void
RegisterSharing::mergeLI(CompGraphNode *From, CompGraphNode *To, VASTModule &VM) {
  assert(From->isCompatibleWith(To)
         && "Cannot merge incompatible LiveIntervals!");
  for (unsigned i = 0, e = From->size(); i != e; ++i)
    mergeSelector(To->getSelector(i), From->getSelector(i), VM);

  ++NumRegMerge;
}

void RegisterSharing::mergeSelector(VASTSelector *To, VASTSelector *From,
                                    VASTModule &VM) {
  SmallVector<VASTSeqOp*, 8> DeadOps;

  while (!From->def_empty())
    (*From->def_begin())->changeSelector(To);

  // Clone the assignments targeting FromV, and change the target to ToV.
  typedef VASTSelector::iterator iterator;
  for (iterator I = From->begin(), E = From->end(); I != E; ++I) {
    VASTLatch L = *I;
    VASTSeqOp *Op = L.Op;

    VASTSeqInst *NewInst
      = VM.lauchInst(Op->getSlot(), Op->getGuard(), Op->num_srcs(),
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
    Op->eraseFromParent();
  }

  VM.eraseSelector(From);
}
