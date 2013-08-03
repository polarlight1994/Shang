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

  NodeTy *addNode(VASTSeqInst *SeqInst) {
    assert(SeqInst && "Unexpected null pointer pass to GetOrCreateNode!");

    SmallVector<VASTSelector*, 4> Sels;
    assert(SeqInst->getNumDefs() == SeqInst->num_srcs()
           && "Expected all assignments are definitions!");
    for (unsigned i = 0; i < SeqInst->num_srcs(); ++i)
      Sels.push_back(SeqInst->getSrc(i).getSelector());

    // Create the node if it not exists yet.
    BasicBlock *DomBlock = cast<Instruction>(SeqInst->getValue())->getParent();
    NodeTy *Node = new NodeTy(Nodes.size() + 1, DomBlock, Sels);
    Nodes.push_back(Node);

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

  void setUpInterval(CompGraphNodeBase *LI) {
    typedef VASTSelector::def_iterator def_iterator;
    typedef CompGraphNodeBase::sel_iterator sel_iterator;
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
  void mergeLI(CompGraphNodeBase *From, CompGraphNodeBase *To);
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

static VASTSeqInst *DynCastSharingCandidate(VASTSeqOp *Op) {
  VASTSeqInst *Inst = dyn_cast<VASTSeqInst>(Op);

  if (!Inst) return 0;

  if (!isChainingCandidate(Inst->getValue())) return 0;

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
  std::map<Value*, CompGraphNodeBase*> VisitedInst;

  for (iterator I = VM.seqop_begin(), IE = VM.seqop_end(); I != IE; ++I) {
    VASTSeqOp *Op = I;
    VASTSeqInst *Inst = DynCastSharingCandidate(Op);

    if (Inst == 0)
      continue;

    CompGraphNodeBase *&N = VisitedInst[Inst->getValue()];

    if (!N) {
      N = G.addNode(Inst);
      setUpInterval(N);
    }

    N->updateOrder(Inst->getSlot()->SlotNum);
  }

  G.recomputeCompatibility();

  G.fixTransitive();

  unsigned NumFU = G.performBinding();

  std::vector<CompGraphNodeBase*> FUMap(NumFU);

  typedef LICompGraph::binding_iterator binding_iterator;
  for (binding_iterator I = G.binding_begin(), E = G.binding_end(); I != E; ++I)
  {
    CompGraphNodeBase *N = I->first;
    unsigned FUIdx = I->second - 1;

    CompGraphNodeBase *&FU = FUMap[FUIdx];
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

    mergeLI(N, FU);
  }

  OverlappedMap.clear();
  return true;
}

void RegisterSharing::mergeLI(CompGraphNodeBase *From, CompGraphNodeBase *To) {
  if (!From->isCompatibleWith(To)) {
    From->isCompatibleWith(To);
    To->isCompatibleWith(From);
  }

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
