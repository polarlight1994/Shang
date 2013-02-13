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

#include "llvm/Support/CFG.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/PostOrderIterator.h"
#define DEBUG_TYPE "vast-scheduling-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;
//===----------------------------------------------------------------------===//
VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, PHINode *PN)
  : Schedule(0),  InstIdx(InstIdx), Ptr(PN) {}

VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, BasicBlock *BB)
  : Schedule(0),  InstIdx(InstIdx), Ptr(BB) {}

VASTSchedUnit::VASTSchedUnit(unsigned InstIdx, VASTSeqOp *Op)
  : Schedule(0),  InstIdx(InstIdx), Ptr(Op) {}

VASTSchedUnit::VASTSchedUnit()
  : Schedule(0), InstIdx(0), Ptr(reinterpret_cast<VASTSeqOp*>(0))
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

BasicBlock *VASTSchedUnit::getParent() const {
  assert(!isEntry() && !isExit() && "Call getParent on the wrong SUnit type!");

  if (VASTSeqOp *Op = Ptr.dyn_cast<VASTSeqOp*>())
    return Op->getSlot()->getParent();

  if (BasicBlock *BB = Ptr.dyn_cast<BasicBlock*>())
    return BB;

  return Ptr.get<PHINode*>()->getParent();
}

void VASTSchedUnit::scheduleTo(unsigned Step) {
  assert(Step && "Bad schedule!");
  Schedule = Step;
}

void VASTSchedUnit::resetSchedule() {
  Schedule = 0;
}

void VASTSchedUnit::print(raw_ostream &OS) const {
  if (isEntry()) {
    OS << "Entry Node";
    return;
  }

  if (isExit()) {
    OS << "Exit Node";
    return;
  }

  OS << '#' << InstIdx << ' ';
  if (VASTSeqOp *Op = Ptr.dyn_cast<VASTSeqOp*>())
    Op->print(OS);
  else if (BasicBlock *BB = Ptr.dyn_cast<BasicBlock*>())
    OS << "BB: " << BB->getName();
  else
    OS << "PHI: " << *Ptr.get<PHINode*>();

  OS << " Scheduled to " << Schedule;
}

void VASTSchedUnit::dump() const {
  print(dbgs());
}

//===----------------------------------------------------------------------===//
VASTSchedGraph::VASTSchedGraph() {
  // Create the entry SU.
  SUnits.push_back(new VASTSchedUnit(0, reinterpret_cast<BasicBlock*>(0)));

  // Create the exit SU.
  SUnits.push_back(new VASTSchedUnit(0, reinterpret_cast<PHINode*>(0)));
}

VASTSchedGraph::~VASTSchedGraph() {}

void VASTSchedGraph::schedule() {
  scheduleSDC();
}

void VASTSchedGraph::resetSchedule() {
  for (iterator I = begin(), E = end(); I != E; ++I)
    I->resetSchedule();

  getEntry()->scheduleTo(1);
}

void VASTSchedGraph::viewGraph() {
  ViewGraph(this, "SchedulingGraph");
}

void VASTSchedGraph::verify() const {
  if (getEntry()->use_empty() && !getEntry()->dep_empty())
    llvm_unreachable("Broken dependencies on Entry!");

  if (getExit()->dep_empty() && !getExit()->use_empty())
    llvm_unreachable("Broken dependencies on Exit!");

  for (const_iterator I = llvm::next(SUnits.begin()), E = SUnits.back();
       I != E; ++I)
    if (I->dep_empty() || I->use_empty())
      llvm_unreachable("Broken dependencies!");
}

void VASTSchedGraph::prepareForScheduling() {
  getEntry()->scheduleTo(1);

  getExit()->InstIdx = size();
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
  bool addFlowDepandency(Value *V, VASTSchedUnit *U);

  void addConditionalDependencies(BasicBlock *BB, VASTSchedUnit *BBEntry);

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

bool VASTScheduling::addFlowDepandency(Value *V, VASTSchedUnit *U) {
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
    if (addFlowDepandency(ChildNode, U))
      continue;

    if (Instruction *ChildInst = dyn_cast<Instruction>(ChildNode))
      VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
  }
}

void VASTScheduling::buildFlowDependencies(Value *V, VASTSchedUnit *U) {
  VASTSeqOp *Op = U->getSeqOp();

  if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(Op)) {
    VASTSeqInst::Type T = SeqInst->getSeqOpType();
    if (T == VASTSeqInst::Launch) {
      buildFlowDependencies(cast<Instruction>(V), U);
      return;
    }

    if (ReturnInst *Ret = dyn_cast<ReturnInst>(V)) {
      buildFlowDependencies(Ret, U);
      // Also add the dependencies form the return instruction to the exit of
      // the scheduling graph.
      G->getExit()->addDep(U, VASTDep::CreateCndDep());
      return;
    }

    if (PHINode *PN = dyn_cast<PHINode>(V)) {
      BasicBlock *IncomingBB = U->getParent();
      BasicBlock *PNParent = PN->getParent();
      Value *V = PN->DoPHITranslation(PNParent, IncomingBB);
      if (!addFlowDepandency(V, U))
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
      if (BasicBlock *BB = U->getParent())
        buildFlowDependencies(BB->getTerminator(), U);

      return;
    }

    Instruction *Inst = cast<Instruction>(V);
    // Get the launching SeqOp and build the dependencies.

    return;
  }
}

void VASTScheduling::addConditionalDependencies(BasicBlock *BB,
                                                VASTSchedUnit *BBEntry) {
  bool PredEmpty = true;
  // Add the dependencies from other BB.
  for (pred_iterator I = pred_begin(BB), E = pred_end(BB); I != E; ++I) {
    BasicBlock *PredBB = *I;
    PredEmpty = false;

    IR2SUMapTy::const_iterator at = IR2SUMap.find(BB);

    if (at == IR2SUMap.end()) continue;

    // Get the corresponding br SeqOp and add the conditional dependencies.
    ArrayRef<VASTSchedUnit*> SUs(at->second);
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSeqOp *Op = SUs[i]->getSeqOp();

      if (Op->getValue() == BB) {
        assert(isa<VASTSeqSlotCtrl>(Op)
               && cast<VASTSeqSlotCtrl>(Op)->getCtrlType()
                  == VASTSeqSlotCtrl::SlotBr
               && "Bad SeqOp type!");

        BBEntry->addDep(SUs[i], VASTDep::CreateCndDep());
        break;
      }
    }
  }

  // If the BB do not have any predecessor, it is the entry block of the
  // function. Add a flow dependencies form it.
  if (PredEmpty) BBEntry->addDep(G->getEntry(), VASTDep::CreateFlowDep(0));
}

VASTSchedUnit *VASTScheduling::getOrCreateBBEntry(BasicBlock *BB) {
  SmallVectorImpl<VASTSchedUnit*> &SUs = IR2SUMap[BB];

  // Simply return the BBEntry if it had already existed.
  if (!SUs.empty()) {
    assert(SUs.back()->isBBEntry() && "Unexpected SU type!");
    return SUs.back();
  }

  VASTSchedUnit *Entry = G->createSUnit(BB);

  addConditionalDependencies(BB, Entry);

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

  assert(V && "Unexpected null llvm Value!");
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

  // Connect the conditional dependencies.


  // Build the dependencies edges.
  // When the BranchInst looping back, we need to wait for the last instruction?
  // Or just build another conditional dependencies?

#ifndef NDEBUG
  G->verify();
#endif

  DEBUG(G->viewGraph());
}

bool VASTScheduling::runOnVASTModule(VASTModule &VM) {
  OwningPtr<VASTSchedGraph> GPtr(new VASTSchedGraph());
  G = GPtr.get();

  TimingNetlist &TNL = getAnalysis<TimingNetlist>();

  // Create the llvm Value to VASTSeqOp mapping.
  buildSchedulingGraph(VM);

  G->schedule();

  return true;
}
