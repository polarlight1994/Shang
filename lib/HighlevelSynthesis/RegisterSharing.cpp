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

#include "shang/Strash.h"
#include "shang/VASTMemoryPort.h"
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

namespace {
class VASTCompGraph : public CompGraphBase {
  typedef CompGraphNode NodeTy;

public:
  const float fanin_factor, fanout_factor, area_factor, consistent_factor;

  VASTCompGraph(DominatorTree &DT, CachedStrashTable &CST)
    : CompGraphBase(DT, CST), fanin_factor(1.0f), fanout_factor(0.1f),
      area_factor(0.6f), consistent_factor(0.8f) {}

  virtual bool isCompatible(NodeTy *Src, NodeTy *Dst) const {
    if (!VFUs::isFUCompatible(Src->FUType, Dst->FUType))
      return false;

    return true;
  }

  virtual float compuateCommonFIBenefit(VASTSelector *Sel) const {
    float Benefit
      = Sel->getBitWidth() * Sel->numNonTrivialFanins() * fanout_factor;

    return Benefit;
  }

  float getEdgeConsistencyBenefit(EdgeType Edge, EdgeType FIEdge) const {
    CompGraphNode *FISrc = FIEdge.first, *FIDst = FIEdge.second;

    if (VFUs::isFUCompatible(FISrc->FUType, FIDst->FUType)) {
      float cost = std::min(FISrc->FUCost, FIDst->FUCost) * consistent_factor;
      // Add interconnection benefit for trivial FU.
      cost = std::max<float>(cost, fanin_factor);
      return cost;
    }

    return 0.0f;
  }

  float computeCost(CompGraphNode *Src, unsigned SrcBinding,
                    CompGraphNode *Dst, unsigned DstBinding) const {
    float Cost = 0.0f;
    // 1. Calculate the saved resource by binding src and dst to the same FU/Reg.
    Cost -= area_factor * compuateSavedResource(Src, Dst);

    // 2. Calculate the interconnection cost.
    Cost += fanin_factor * computeIncreasedMuxPorts(Src, Dst);

    // 3. Timing penalty introduced by MUX
    //
    return Cost;
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

    AU.addRequired<CachedStrashTable>();
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

  bool runOnVASTModule(VASTModule &VM);

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
  INITIALIZE_PASS_DEPENDENCY(CachedStrashTable)
INITIALIZE_PASS_END(RegisterSharing, "shang-register-sharing",
                    "Share the registers in the design", false, true)

Pass *llvm::createRegisterSharingPass() {
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

bool RegisterSharing::runOnVASTModule(VASTModule &VM) {
  LVS = &getAnalysis<SeqLiveVariables>();

  VASTCompGraph G(getAnalysis<DominatorTree>(), getAnalysis<CachedStrashTable>());

  initializeOverlappedSlots(VM);

  typedef VASTModule::seqop_iterator iterator;

  // Due to CFG folding, there maybe more than one operation correspond to
  // the same LLVM Instruction. These operations operate on the same set of
  // registers, we need to avoid adding them more than once to the compatibility
  // graph.
  // TODO: Use multimap.
  std::map<DataflowInst, CompGraphNode*> VisitedInst;

  for (iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I) {
    VASTSeqOp *Op = I;

    VASTSeqInst *Inst = dyn_cast<VASTSeqInst>(Op);

    if (Inst == 0 || !Inst->isBindingCandidate()) {
      for (unsigned i = 0; i < Op->num_srcs(); ++i)
        G.addBoundSels(Op->getSrc(i).getSelector());
      continue;
    }

    CompGraphNode *&N = VisitedInst[Op];

    if (!N) {
      N = G.addNewNode(Inst);
      setUpInterval(N);
    }

    N->updateOrder(Inst->getSlot()->SlotNum);
  }

  G.decomposeTrivialNodes();
  G.computeCompatibility();
  G.fixTransitive();

  G.compuateEdgeCosts();
  G.setCommonFIBenefit();

  unsigned NumFU = G.performBinding();

  if (NumFU == 0)
    return false;

  std::vector<CompGraphNode*> FUMap(NumFU);

  typedef VASTCompGraph::binding_iterator binding_iterator;
  for (binding_iterator I = G.binding_begin(), E = G.binding_end(); I != E; ++I)
  {
    CompGraphNode *N = static_cast<CompGraphNode*>(I->first);
    unsigned FUIdx = I->second - 1;

    CompGraphNode *&FU = FUMap[FUIdx];
    if (!FU) {
      FU = N;
      continue;
    }

    DEBUG(dbgs() << "FU " << FUIdx << "\n";
    dbgs() << "Merge: \n";
    N->dump();
    dbgs() << "\n To: \n";
    FU->dump();
    dbgs() << "\n\n");

    mergeLI(N, FU, VM);

    // Remove it from the compatibility graph since it is already merged into others.
    G.merge(N, FU);
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
    VM.eraseSeqOp(Op);
  }

  VM.eraseSelector(From);
}
