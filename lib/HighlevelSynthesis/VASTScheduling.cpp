//===----- VASTScheduling.cpp - Scheduling Graph on VAST  -------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VASTSUnit class, which represents the elemental
// scheduling unit in VAST.
//
//===----------------------------------------------------------------------===//
//

#include "TimingNetlist.h"
#include "VASTScheduling.h"
#include "ScheduleDOT.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTModulePass.h"

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/PostOrderIterator.h"
#define DEBUG_TYPE "vast-scheduling-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;
//===----------------------------------------------------------------------===//
VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, PHINode *PN)
  : SchedSlot(0),  InstIdx(InstIdx), Ptr(PN) {}

VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, BasicBlock *BB)
  : SchedSlot(0),  InstIdx(InstIdx), Ptr(BB) {}

VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, VASTSeqOp *Op)
  : SchedSlot(0),  InstIdx(InstIdx), Ptr(Op) {}

VASTSchedUnit::VASTSchedUnit()
  : SchedSlot(0), InstIdx(0), Ptr(reinterpret_cast<VASTSeqOp*>(0))
{}

void VASTSchedUnit::EdgeBundle::addEdge(VASTDep NewEdge) {
 assert(NewEdge.getEdgeType() != VASTDep::FixedTiming
        && "Fixed timing constraint cannot be added!");
  unsigned InsertBefore = 0, Size = Edges.size();
  bool NeedToInsert = true;

  VASTDep::Types NewEdgeType = NewEdge.getEdgeType();
  unsigned NewDistance = NewEdge.getDistance();
  int      NewLatency = NewEdge.getLatency();

  while (InsertBefore < Size) {
    VASTDep &CurEdge = Edges[InsertBefore];
    // Keep the edges in ascending order.
    if (CurEdge.getDistance() > NewEdge.getDistance())
      break;

    VASTDep::Types CurEdgeType = CurEdge.getEdgeType();
    unsigned CurDistance = CurEdge.getDistance();
    int      CurLatency = CurEdge.getLatency();

    // Update the edge with the tighter constraint.
    if (CurDistance == NewDistance && CurLatency < NewLatency) {
      if (NewEdgeType == CurEdgeType) {
        NeedToInsert = false;
        CurEdge = NewEdge;
      }

      break;
    }

    // Now we have CurDistance < NewDistance, NewEdge is masked by CurEdge if
    // NewEdge has a smaller latency than CurEdge.
    if (NewLatency <= CurLatency && NewEdgeType == CurEdgeType) return;

    ++InsertBefore;
  }

  assert(Edges[InsertBefore].getLatency() <= NewEdge.getLatency()
         && Edges[InsertBefore].getDistance() >= NewEdge.getDistance()
         && "Bad insert position!");

  // Insert the new edge right before the edge with bigger iterative distance.
  if (NeedToInsert) Edges.insert(Edges.begin() + InsertBefore, NewEdge);
}


VASTDep &VASTSchedUnit::EdgeBundle::getEdge(unsigned II) {
  assert(Edges.size() && "Unexpected empty edge bundle!");
  VASTDep *CurEdge = &Edges.front();
  int Latency = CurEdge->getLatency(II);

  // Zero II means we should ignore the loop-carried dependencies.
  // if (II == 0) return *CurEdge;

  for (unsigned i = 1, e = Edges.size(); i != e; ++i) {
    VASTDep &Edge = Edges[i];

    // Find the edge with biggest latency.
    int NewLatency = Edge.getLatency(II);
    if (NewLatency > Latency) {
      Latency = NewLatency;
      CurEdge = &Edge;
    }
  }

  return *CurEdge;
}

BasicBlock *VASTSchedUnit::getParentBB() const {
  if (VASTSeqOp *Op = Ptr.dyn_cast<VASTSeqOp*>())
    return Op->getSlot()->getParent();

  if (BasicBlock *BB = Ptr.dyn_cast<BasicBlock*>())
    return BB;

  return Ptr.get<PHINode*>()->getParent();
}

void VASTSchedUnit::print(raw_ostream &OS) const {
  if (InstIdx == 0) {
    OS << "Entry Node";
    return;
  }

  OS << '#' << InstIdx << ' ';
  if (VASTSeqOp *Op = Ptr.dyn_cast<VASTSeqOp*>())
    Op->print(OS);
  else if (BasicBlock *BB = Ptr.dyn_cast<BasicBlock*>())
    OS << "BB: " << BB->getName();
  else
    OS << "PHI: " << *Ptr.get<PHINode*>();

  OS << " Scheduled to " << SchedSlot;
}

void VASTSchedUnit::dump() const {
  print(dbgs());
}

//===----------------------------------------------------------------------===//
VASTSchedGraph::VASTSchedGraph() {
  // Create the entry SU.
  SUnits.push_back(new VASTSchedUnit(0, reinterpret_cast<PHINode*>(0)));
  // Create the exit SU.
  SUnits.push_back(new VASTSchedUnit(0, reinterpret_cast<BasicBlock*>(0)));
}

VASTSchedGraph::~VASTSchedGraph() {}

void VASTSchedGraph::schedule() {}

void VASTSchedGraph::viewGraph() const {
  ViewGraph(const_cast<VASTSchedGraph*>(this), "SchedulingGraph");
}

//===----------------------------------------------------------------------===//
namespace {
struct VASTScheduling : public VASTModulePass {
  static char ID;
  typedef std::map<Value*, SmallVector<VASTSchedUnit*, 4> > IR2SUMapTy;
  IR2SUMapTy IR2SUMap;
  VASTSchedGraph *G;
  TimingNetlist *TNL;

  VASTScheduling() : VASTModulePass(ID), G(0), TNL(0) {
    initializeVASTSchedulingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredID(BasicBlockTopOrderID);
    AU.addRequired<TimingNetlist>();
  }

  VASTSchedUnit *getOrCreateBBEntry(BasicBlock *BB);
  VASTSchedUnit *createSUnit(Value *V, VASTSeqOp *Op, VASTSchedUnit *BBEntry);

  void buildFlowDependencies(Instruction *I, VASTSchedUnit *U);
  void buildFlowDependencies(Value *V, VASTSchedUnit *U);
  bool addFlowDepandencies(Value *V, VASTSchedUnit *U);

  void buildSchedulingGraph(VASTModule &VM);
  void buildSchedulingUnits(VASTSlot *S);

  bool runOnVASTModule(VASTModule &VM);

  void releaseMemory() {
    IR2SUMap.clear();
    G = 0;
    TNL = 0;
  }
};
}

char VASTScheduling::ID = 0;

INITIALIZE_PASS_BEGIN(VASTScheduling,
                      "vast-scheduling", "Perfrom Scheduling on the VAST",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(BasicBlockTopOrder)
INITIALIZE_PASS_END(VASTScheduling,
                    "vast-scheduling", "Perfrom Scheduling on the VAST",
                    false, true)

Pass *llvm::createVASTSchedulingPass() {
  return new VASTScheduling();
}

bool VASTScheduling::addFlowDepandencies(Value *V, VASTSchedUnit *U) {
  IR2SUMapTy::const_iterator at = IR2SUMap.find(V);

  if (at == IR2SUMap.end()) return false;

  // Get the corresponding latch SeqOp.
  ArrayRef<VASTSchedUnit*> SUs(at->second);
  for (unsigned i = 0; i < SUs.size(); ++i)
    if (VASTSeqInst *Inst = dyn_cast<VASTSeqInst>(SUs[i]->getSeqOp()))
      if (Inst->getSeqOpType() == VASTSeqInst::Latch) {
        U->addDep(SUs[i], VASTDep::CreateFlowDep(0));
        return true;
      }

  llvm_unreachable("Source SU not found!");
  return false;
}

void VASTScheduling::buildFlowDependencies(Instruction *I, VASTSchedUnit *U) {
  // Ignore the trivial case.
  if (I == 0) return;

  SmallVector<std::pair<Instruction*, Instruction::op_iterator>, 8> VisitStack;
  std::set<Value*> Visited;

  VisitStack.push_back(std::make_pair(I, I->op_begin()));

  while (!VisitStack.empty()) {
    Instruction *CurInst = VisitStack.back().first;

    if (CurInst->op_end() == VisitStack.back().second) {
      VisitStack.pop_back();
      continue;
    }

    Value *ChildNode = *VisitStack.back().second++;

    // Are we reach the leaf of the dependencies tree?
    if (addFlowDepandencies(ChildNode, U))
      continue;

    if (Instruction *ChildInst = dyn_cast<Instruction>(ChildNode))
      VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
  }
}

void VASTScheduling::buildFlowDependencies(Value *V, VASTSchedUnit *U) {
  VASTSeqOp *Op = U->getSeqOp();

  if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(Op)) {
    VASTSeqInst::Type T = SeqInst->getSeqOpType();
    if (T == VASTSeqInst::Launch || isa<ReturnInst>(V)) {
      buildFlowDependencies(cast<Instruction>(V), U);
      return;
    }

    if (PHINode *PN = dyn_cast<PHINode>(V)) {
      BasicBlock *IncomingBB = U->getParentBB();
      BasicBlock *PNParent = PN->getParent();
      Value *V = PN->DoPHITranslation(PNParent, IncomingBB);
      if (!addFlowDepandencies(V, U))
        buildFlowDependencies(dyn_cast<Instruction>(V), U);
      return;
    }

    if (T == VASTSeqInst::Latch && !isa<Argument>(V)) {
      // Simply build the dependencies from the launch instruction.

      return;
    }

    return;
  }

  if (VASTSeqSlotCtrl *SlotCtrl = dyn_cast<VASTSeqSlotCtrl>(Op)) {
    if (SlotCtrl->getCtrlType() == VASTSeqSlotCtrl::SlotBr) {
      // This SlotCtrl is corresponding to the terminator of the BasicBlock.
      // Please note that the SlotBr of the entry slot do not have a
      // corresponding LLVM BB.
      if (BasicBlock *BB = U->getParentBB())
        buildFlowDependencies(BB->getTerminator(), U);

      return;
    }

    Instruction *Inst = cast<Instruction>(V);
    // Get the launching SeqOp and build the dependencies.

    return;
  }

}

VASTSchedUnit *VASTScheduling::getOrCreateBBEntry(BasicBlock *BB) {
  SmallVectorImpl<VASTSchedUnit*> &SUs = IR2SUMap[BB];

  // Simply return the BBEntry if it had already existed.
  if (!SUs.empty()) return SUs.back();

  VASTSchedUnit *Entry = G->createSUnit(BB);

  // Also create the SUnit for the PHI Nodes.
  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(), E = BB->getFirstNonPHI(); I != E; ++I) {
    PHINode *PN = cast<PHINode>(I);
    VASTSchedUnit *U = G->createSUnit(PN);

    // Add the dependencies between the entry of the BB and the PHINode.
    U->addDep(Entry, VASTDep(VASTDep::Predicate, 0, 0));
    IR2SUMap[PN].push_back(U);
  }

  return Entry;
}

VASTSchedUnit *VASTScheduling::createSUnit(Value *V, VASTSeqOp *Op,
                                           VASTSchedUnit *BBEntry) {
  VASTSchedUnit *U = G->createSUnit(Op);

  assert(U && "The scheduling Unit Not built!");
  U->addDep(BBEntry, VASTDep(VASTDep::Predicate, 0, 0));
  IR2SUMap[V].push_back(U);

  // Build the flow dependencies.
  buildFlowDependencies(V, U);

  return U;
}

void VASTScheduling::buildSchedulingUnits(VASTSlot *S) {
  typedef VASTSlot::op_iterator op_iterator;

  BasicBlock *BB = S->getParent();

  // If the BB is NULL, this slot should be the entry or the exit of the
  // state-transition graph.
  VASTSchedUnit *BBEntry = 0;
  if (BB == 0) BBEntry = G->getEntry();
  else         BBEntry = getOrCreateBBEntry(BB);

  for (op_iterator OI = S->op_begin(), OE = S->op_end(); OI != OE; ++OI) {
    VASTSeqOp *Op = *OI;

    Value *V = Op->getValue();
    // We can safely ignore the SeqOp that does not correspond to any LLVM
    // Value, their will be rebuilt when we emit the scheduling.
    if (V == 0) continue;

    createSUnit(V, Op, BBEntry);
  }
}

void VASTScheduling::buildSchedulingGraph(VASTModule &VM) {
  // Build the scheduling units according to the original scheduling.
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >
    RPO(VM.getStartSlot());

  typedef
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >::rpo_iterator
  slot_top_iterator;

  for (slot_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    buildSchedulingUnits(*I);

  // Build the dependencies edges.
  // When the BranchInst looping back, we need to wait for the last instruction?
  // Or just build another conditional dependencies?

  DEBUG(G->viewGraph());
}

bool VASTScheduling::runOnVASTModule(VASTModule &VM) {
  OwningPtr<VASTSchedGraph> GPtr(new VASTSchedGraph());
  G = GPtr.get();

  TimingNetlist &TNL = getAnalysis<TimingNetlist>();

  // Create the llvm Value to VASTSeqOp mapping.
  buildSchedulingGraph(VM);

  return true;
}
