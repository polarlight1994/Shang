//===------ ScheduleEmitter.cpp - Emit the Schedule -------------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Schedule emitter, which reimplement the
// state-transition graph according to the scheduling results. It also re-time
// the data-path if necessary.
//
//===----------------------------------------------------------------------===//
//
#include "VASTScheduling.h"

#include "shang/VASTModulePass.h"
#include "shang/VASTExprBuilder.h"
#include "shang/VASTModule.h"

#include "llvm/IR/Function.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-schedule-emitter"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumBBByPassed, "Number of BasicBlock Bypassed by the CFG folding");
STATISTIC(NumRetimed, "Number of Retimed Value during Schedule Emitting");
STATISTIC(NumRejectedRetiming,
          "Number of Reject Retiming because the predicates are not compatible");
STATISTIC(NumFalsePathSkip,
          "Number of False Paths skipped during the CFG folding");
STATISTIC(NumSlots, "Number of States created in the State-transition Graph");

namespace {
/// VASTCFGPred - Represent the predicate by the path in the CFG.
struct CFGPred : public FoldingSetNode {
  BasicBlock *const*BBList;
  unsigned Size;
public:
  CFGPred(BasicBlock *const*BBList, unsigned Size) : BBList(BBList), Size(Size) {}

  /// Profile - Used to insert VASTCFGPred objects, or objects that contain
  /// VASTExpr objects, into FoldingSets.
  void Profile(FoldingSetNodeID& ID) const {
    for (unsigned i = 0; i < size(); ++i)
      ID.AddPointer(BBList[i]);
  }
  unsigned size() const { return Size; }

  bool isCompatible(ArrayRef<BasicBlock*> RetimingPath) const {
    // This Pred is more restricted and hence not compatible with RetimingPath.
    if (size() > RetimingPath.size()) return false;

    // Now we have size() < RetimingPath.size()
    for (unsigned i = 0; i < size(); ++i)
      // Not compatible because RetimingPath following a different path.
      if (BBList[i] != RetimingPath[i]) return false;

    return true;
  }

  void dump() const {
    dbgs() << "Current Pred Path: ";
    for (unsigned i = 0; i < size(); ++i)
      if (BasicBlock *BB = BBList[i]) dbgs() << BB->getName() << ", ";
      else                            dbgs() << "<entry>, ";
    dbgs()<< '\n';
  }
};

class ScheduleEmitter : public MinimalExprBuilderContext {
  VASTExprBuilder Builder;
  VASTModule &VM;
  ilist<VASTSlot> OldSlots;
  VASTSchedGraph &G;
  std::map<BasicBlock*, VASTSlot*> LandingSlots;
  std::map<BasicBlock*, unsigned> LandingSlotNum;

  std::map<VASTSeqInst*, CFGPred*> PredMap;
  BumpPtrAllocator PredAllocator;
  FoldingSet<CFGPred> UniqueCFGPreds;

  SUBBMap BBMap;

  void takeOldSlots();

  void clearUp();
  void clearUp(VASTSlot *S);

  CFGPred *getCFGPred(ArrayRef<BasicBlock*> RetimingPath) {
    unsigned NumBBs = RetimingPath.size();
    FoldingSetNodeID ID;
    for (unsigned i = 0; i < NumBBs; ++i)
      ID.AddPointer(RetimingPath[i]);

    void *IP = 0;
    if (CFGPred *P = UniqueCFGPreds.FindNodeOrInsertPos(ID, IP))
      return P;

    // Allocate the BB list.
    BasicBlock **BBList = PredAllocator.Allocate<BasicBlock*>(NumBBs);
    std::uninitialized_copy(RetimingPath.begin(), RetimingPath.end(), BBList);
    CFGPred *P = new (PredAllocator) CFGPred(BBList, NumBBs);
    UniqueCFGPreds.InsertNode(P, IP);

    return P;
  }

  VASTValPtr retimeValToSlot(VASTValue *V, VASTSlot *ToSlot,
                             ArrayRef<BasicBlock*> RetimingPath);

  VASTValPtr retimeDatapath(VASTValue *V, VASTSlot *ToSlot,
                            ArrayRef<BasicBlock*> RetimingPath);

  VASTValPtr retimeDatapath(VASTValPtr V, VASTSlot *ToSlot,
                            ArrayRef<BasicBlock*> RetimingPath) {
    VASTValue *Val = V.get();
    VASTValPtr RetimedV = retimeDatapath(Val, ToSlot, RetimingPath);
    if (V.isInverted()) RetimedV = Builder.buildNotExpr(RetimedV);
    return RetimedV;
  }

  VASTSlotCtrl *addSuccSlot(VASTSlot *S, VASTSlot *NextSlot, VASTValPtr Cnd,
                            Value *V = 0);

  VASTSeqInst *cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot, VASTValPtr Pred,
                            SmallVectorImpl<BasicBlock*> &RetimingPath);
  VASTSlotCtrl *cloneSlotCtrl(VASTSlotCtrl *Op, VASTSlot *ToSlot,
                              VASTValPtr Pred,
                              SmallVectorImpl<BasicBlock*> &RetimingPath);

  /// Emit the scheduling units in the same BB.
  ///
  void emitScheduleInBB(MutableArrayRef<VASTSchedUnit*> SUs);

  bool emitToFirstSlot(VASTValPtr Pred, VASTSlot *ToSlot,
                       MutableArrayRef<VASTSchedUnit*> SUs,
                       SmallVectorImpl<BasicBlock*> &RetimingPath);

  void emitToSlot(VASTSeqOp *Op, VASTValPtr Pred, VASTSlot *ToSlot,
                  SmallVectorImpl<BasicBlock*> &RetimingPath);

  void handleNewSeqOp(VASTSeqInst *SeqOp);
  void handleNewSeqOp(VASTSlotCtrl *SeqOp);
public:
  ScheduleEmitter(VASTModule &VM, VASTSchedGraph &G);
  ~ScheduleEmitter() { clearUp(); }

  void initialize();

  void emitSchedule();
};
}

ScheduleEmitter::ScheduleEmitter(VASTModule &VM, VASTSchedGraph &G)
  : MinimalExprBuilderContext(VM), Builder(*this), VM(VM), G(G) {}

//===----------------------------------------------------------------------===//
void ScheduleEmitter::clearUp(VASTSlot *S) {
  typedef VASTSlot::op_iterator op_iterator;
  for (op_iterator I = S->op_begin(); I != S->op_end(); ++I) {
    VASTSeqOp *SeqOp = *I;

    // Delete the dead Exprs used by this SeqOp.
    for (unsigned i = 0, e = SeqOp->size(); i != e; ++i) {
      VASTValue *V = SeqOp->getOperand(i).unwrap().get();
      SeqOp->getOperand(i).unlinkUseFromUser();

      if (!V->use_empty()) continue;

      if (VASTExpr *Child = dyn_cast<VASTExpr>(V))
        VM->recursivelyDeleteTriviallyDeadExprs(Child);
    }

    SeqOp->clearParent();
    VM.eraseSeqOp(*I);
  }
}

void ScheduleEmitter::clearUp() {
  // Clear up the VASTSeqOp in the old list.
  while (!OldSlots.empty()) {
    VASTSlot *CurSlot = &OldSlots.back();

    clearUp(CurSlot);

    OldSlots.pop_back();
  }

  // Release the dead objects now.
  VM.gc();
}

//===----------------------------------------------------------------------===//
void ScheduleEmitter::takeOldSlots() {
  OldSlots.splice(OldSlots.begin(), VM.getSLotList(),
                  VM.slot_begin(), VM.slot_end());
  // Remove the successors of the start slot, we will reconstruct them.
  VM.createStartSlot();
  VASTSlot *StartSlot = VM.getStartSlot();
  VASTSlot *OldStart = OldSlots.begin();
  StartSlot->unlinkSuccs();

  // Build a path from the entry slot.
  SmallVector<BasicBlock*, 4> RetimingPath;
  RetimingPath.push_back(0);
  typedef VASTSlot::op_iterator op_iterator;
  for (op_iterator I = OldStart->op_begin(); I != OldStart->op_end(); ++I)
    if (VASTSeqInst *SeqOp = dyn_cast<VASTSeqInst>(*I))
      if (isa<Argument>(SeqOp->getValue()))
        cloneSeqInst(SeqOp, StartSlot, SeqOp->getPred(), RetimingPath);

  VASTValPtr StartPort = VM.getPort(VASTModule::Start).getValue();
  addSuccSlot(StartSlot, StartSlot, Builder.buildNotExpr(StartPort));

#ifndef NDEBUG
  // Mark the old slots.
  for (ilist<VASTSlot>::iterator I = OldSlots.begin(), E = OldSlots.end();
       I != E; ++I)
    I->setDead();
#endif
}

static
int top_sort_schedule(const VASTSchedUnit *LHS, const VASTSchedUnit *RHS) {
  if (LHS->getSchedule() != RHS->getSchedule())
    return LHS->getSchedule() < RHS->getSchedule() ? -1 : 1;

  VASTSeqOp *LHSOp = LHS->getSeqOp(), *RHSOp = RHS->getSeqOp();

  /// Put the pseudo Scheduling Unit before the normal Scheduling Unit.
  if (LHSOp == 0 && RHSOp != 0) return -1;
  if (LHSOp != 0 && RHSOp == 0) return 1;

  // Make sure we emit all VASTSeqInsts before emitting the VASTSlotCtrls.
  // Because we will emit the SUs in the first slot of the BB that pointed by
  // the VASTCtrls, and we may need to perform retiming based on the newly
  // emitted VASTSeqInsts.
  if (LHSOp && RHSOp && LHSOp->getASTType() != RHSOp->getASTType()) {
    if (LHSOp->getASTType() == VASTNode::vastSeqInst) return -1;
    if (RHSOp->getASTType() == VASTNode::vastSeqInst) return 1;
  }

  // Now LHSOp and RHSOp have the same ASTType.
  // Put the latch before lanch.
  if (LHS->isLatch() && !RHS->isLatch()) return -1;
  if (!LHS->isLatch() && RHS->isLatch()) return 1;

  if (LHS->getIdx() < RHS->getIdx()) return -1;
  if (LHS->getIdx() > RHS->getIdx()) return 1;

  return 0;
}

static int top_sort_schedule_wrapper(const void *LHS, const void *RHS) {
  return top_sort_schedule(*reinterpret_cast<const VASTSchedUnit* const *>(LHS),
                           *reinterpret_cast<const VASTSchedUnit* const *>(RHS));
}

void ScheduleEmitter::initialize() {
  BBMap.buildMap(G);
  BBMap.sortSUs(top_sort_schedule_wrapper);

  takeOldSlots();

  // Assign the landing slot number for each BB.
  Function &F = VM;
  typedef Function::iterator iterator;

  // Allocate the landing slots.
  unsigned CurLandingSlot = 1;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    ArrayRef<VASTSchedUnit*> SUs(BBMap.getSUInBB(BB));
    unsigned SlotIncr = SUs.back()->getSchedule() - SUs.front()->getSchedule();
    LandingSlotNum[BB] = SlotIncr == 0 ? 0 : CurLandingSlot;
    CurLandingSlot += SlotIncr;
  }
}

//===----------------------------------------------------------------------===//
VASTSlotCtrl *ScheduleEmitter::addSuccSlot(VASTSlot *S, VASTSlot *NextSlot,
                                           VASTValPtr Cnd, Value *V) {
  // If the Br is already exist, simply or the conditions together.
  if (VASTSlotCtrl *SlotBr = S->getBrToSucc(NextSlot)) {
    VASTValPtr Pred = SlotBr->getPred();
    SlotBr->getPred().replaceUseBy(Builder.buildOrExpr(Pred, Cnd, 1));
    //SlotBr->annotateValue(V);
    return 0;
  }

  S->addSuccSlot(NextSlot);
  VASTSlotCtrl *SlotBr = VM.createSlotCtrl(NextSlot, S, Cnd);
  if (V) SlotBr->annotateValue(V);

  return SlotBr;
}

VASTSlotCtrl *ScheduleEmitter::cloneSlotCtrl(VASTSlotCtrl *Op, VASTSlot *ToSlot,
                                             VASTValPtr Pred,
                                             SmallVectorImpl<BasicBlock*> &
                                             RetimingPath) {
  // Retime the predicate operand.
  Pred = Builder.buildAndExpr(retimeDatapath(Op->getPred(), ToSlot, RetimingPath),
                              Pred, 1);

  // Some times we may even try to fold the BB through a 'false path' ... such
  // folding can be safely skipped.
  if (Pred == VASTImmediate::False) {
    ++NumFalsePathSkip;
    return 0;
  }

  Value *V = Op->getValue();

  // Handle the trivial case
  if (!Op->isBranch()) {
    VASTSlotCtrl *NewSlotCtrl
      = VM.createSlotCtrl(Op->getNode(), ToSlot, Pred);
    NewSlotCtrl->annotateValue(V);
    return NewSlotCtrl;
  }

  if (isa<ReturnInst>(V) || isa<UnreachableInst>(V))
    return addSuccSlot(ToSlot, VM.getFinishSlot(), Pred, V);

  BasicBlock *TargetBB = Op->getTargetSlot()->getParent();

  // Emit the the SUs in the first slot in the target BB.
  // Connect to the landing slot if not all SU in the target BB emitted to
  // current slot.
  if (emitToFirstSlot(Pred, ToSlot, BBMap.getSUInBB(TargetBB), RetimingPath)) {
    // There is some SeqOp need to be emitted to TargetBB, build the control
    // flow.
    VASTSlot *&LandingSlot = LandingSlots[TargetBB];
    // There maybe more than one branch instruction targeting the landing
    // slot. Only create the slot once.
    if (LandingSlot == 0) {
      LandingSlot = VM.createSlot(LandingSlotNum[TargetBB], TargetBB);
      ++NumSlots;
    }

    return addSuccSlot(ToSlot, LandingSlot, Pred, V);
  }

  // Else all scheduling unit of target block are emitted to current slot
  // do not emit the SlotCtrl because it is not needed.
  ++NumBBByPassed;
  return 0;
}

//===----------------------------------------------------------------------===//
VASTSeqInst *ScheduleEmitter::cloneSeqInst(VASTSeqInst *Op, VASTSlot *ToSlot,
                                           VASTValPtr Pred,
                                           SmallVectorImpl<BasicBlock*> &
                                           RetimingPath) {
  SmallVector<VASTValPtr, 4> RetimedOperands;
  // Retime the predicate operand.
  Pred = Builder.buildAndExpr(retimeDatapath(Op->getPred(), ToSlot, RetimingPath),
                              Pred, 1);

  // Some times we may even try to fold the BB through a 'false path' ... such
  // folding can be safely skipped.
  if (Pred == VASTImmediate::False) {
    ++NumFalsePathSkip;
    return 0;
  }

  // Retime all the operand to the specificed slot.
  typedef VASTOperandList::op_iterator iterator;
  for (iterator I = Op->src_begin(), E = Op->src_end(); I != E; ++I)
    RetimedOperands.push_back(retimeDatapath(*I, ToSlot, RetimingPath));

  VASTSeqInst *NewInst = VM.lauchInst(ToSlot, Pred, Op->getNumSrcs(),
                                      Op->getValue(), Op->getSeqOpType());
  typedef VASTSeqOp::op_iterator iterator;

  for (unsigned i = 0, e = Op->getNumSrcs(); i < e; ++i) {
    VASTSeqValue *Dst = Op->getSrc(i).getDst();
    VASTValPtr Src = RetimedOperands[i];
    // Create a wrapper wire to break the cycle.
    if (Src == Dst) {
      unsigned BitWidth = Src->getBitWidth();
      Twine WrapperName = Twine(Dst->getName()) + "_Wrapper";
      Src = VM.createWrapperWire(WrapperName, BitWidth, Src);
    }

    NewInst->addSrc(Src, i, i < Op->getNumDefs(), Dst);
  }

  // PHINode have extra predicate.
  if (PHINode *PN = dyn_cast<PHINode>(NewInst->getValue())) {
    assert(PN->getBasicBlockIndex(RetimingPath.back()) >= 0
           && "Not from incoming block?");
    RetimingPath.push_back(PN->getParent());
  }

  bool inserted
    = PredMap.insert(std::make_pair(NewInst, getCFGPred(RetimingPath))).second;
  assert(inserted && "Cannot insert Pred!");
  (void) inserted;

  // Pop the extra predicate.
  if (isa<PHINode>(NewInst->getValue())) RetimingPath.pop_back();

#ifdef XDEBUG
  // Verify if we have the CFGPred conflict.
  std::set<std::pair<CFGPred*, VASTSlot*> > UniqueCFGPreds;
  if (NewInst->getNumDefs()) {
    assert(NewInst->getNumDefs() == 1 && "Unexpected multi-define SeqOp!");
    VASTSeqValue *S = NewInst->getDef(0);

    for (VASTSeqValue::iterator I = S->begin(), E = S->end(); I != E; ++I) {
      VASTLatch U = *I;
      std::map<VASTSeqInst*, CFGPred*>::iterator at
        = PredMap.find(cast<VASTSeqInst>(U.Op));
      if (at == PredMap.end()) continue;
      CFGPred *Pred = at->second;
      Pred->dump();
      assert(UniqueCFGPreds.insert(std::make_pair(at->second, U.getSlot())).second
             && "CFGPred conflict detected!");
    }
  }
#endif

  return NewInst;
}

VASTValPtr ScheduleEmitter::retimeValToSlot(VASTValue *V, VASTSlot *ToSlot,
                                            ArrayRef<BasicBlock*> RetimingPath) {
  // TODO: Check the predicate of the assignment.
  VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(V);

  if (SeqVal == 0) return V;

  DEBUG(dbgs() << "Current Retiming Path: ";
  for (unsigned i = 0; i < RetimingPath.size(); ++i)
    if (BasicBlock *BB = RetimingPath[i]) dbgs() << BB->getName() << ", ";
    else                                  dbgs() << "<entry>, ";
  dbgs()<< '\n';);

  // Try to forward the value which is assigned to SeqVal at the same slot.
  VASTValPtr ForwardedValue = SeqVal;

  typedef VASTSeqValue::iterator iterator;
  for (iterator I = SeqVal->begin(), E = SeqVal->end(); I != E; ++I) {
    VASTLatch U = *I;

    // Only retime across the latch operation.
    if (cast<VASTSeqInst>(U.Op)->getSeqOpType() != VASTSeqInst::Latch)
      continue;

    // Wrong slot to retime?
    if (U.getSlot() != ToSlot) continue;

    Value *Val = U.Op->getValue();

    CFGPred *Pred = PredMap[cast<VASTSeqInst>(U.Op)];
    assert(Pred && "CFGPred not found!");

    // Do not perform the retiming if the path is not matched.
    if (!Pred->isCompatible(RetimingPath)) {
      DEBUG(dbgs() << "Reject retiming:\n";
      CFGPred(RetimingPath.data(), RetimingPath.size()).dump();
      Pred->dump(););

      ++NumRejectedRetiming;
      continue;
    }

    DEBUG(dbgs() << "Goning to forward " /*<< VASTValPtr(U) << ", "*/ << *Val
                 << "\n Retiming path and pred path:\n";
          CFGPred(RetimingPath.data(), RetimingPath.size()).dump();
          Pred->dump(););

    assert (ForwardedValue == SeqVal && "Unexpected multiple compatible source!");
    ForwardedValue = VASTValPtr(U);

    assert(ForwardedValue->getBitWidth() == V->getBitWidth()
           && "Bitwidth implicitly changed!");
    ++NumRetimed;
  }

#ifndef NDEBUG
  if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(ForwardedValue.get())) {
    bool AnySrcEmitted = false;

    for (iterator I = SV->begin(), E = SV->end(); I != E; ++I) {
      AnySrcEmitted |= !(*I).getSlot()->isDead();
    }

    if (!AnySrcEmitted) {
      SV->dumpFanins();
      dbgs() << "\nRetiming performed before source value emitted!\n";
    }
  }
#endif

  return ForwardedValue;
}

VASTValPtr ScheduleEmitter::retimeDatapath(VASTValue *Root, VASTSlot *ToSlot,
                                           ArrayRef<BasicBlock*> RetimingPath) {
  std::map<VASTValue*, VASTValPtr> RetimedMap;
  std::set<VASTValue*> Visited;

  VASTExpr *RootExpr = dyn_cast<VASTExpr>(Root);

  // The Root is already the leaf of the expr tree.
  if (RootExpr == 0) return retimeValToSlot(Root, ToSlot, RetimingPath);

  typedef VASTOperandList::op_iterator ChildIt;
  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(RootExpr, RootExpr->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == Node->op_end()) {
      VisitStack.pop_back();

      bool AnyOperandRetimed = false;
      SmallVector<VASTValPtr, 8> RetimedOperands;

      // Collect the possible retimed operands.
      for (ChildIt I = Node->op_begin(), E = Node->op_end(); I != E; ++I) {
        VASTValPtr Operand = *I;
        VASTValPtr RetimedOperand = RetimedMap[Operand.get()];
        if (Operand.isInverted())
          RetimedOperand = Builder.buildNotExpr(RetimedOperand);
        AnyOperandRetimed |= RetimedOperand != Operand;

        RetimedOperands.push_back(RetimedOperand);
      }

      // Rebuild the expression if any of its operand retimed.
      VASTValPtr RetimedExpr = Node;
      if (AnyOperandRetimed)
        RetimedExpr = Builder.buildExpr(Node->getOpcode(), RetimedOperands,
                                        Node->UB, Node->LB);

      bool inserted
        = RetimedMap.insert(std::make_pair(Node, RetimedExpr)).second;
      assert(inserted && "Expr had already retimed?");
      (void) inserted;

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTExpr *ChildExpr = dyn_cast<VASTExpr>(ChildNode)) {
      // ChildNode has a name means we had already visited it.
      if (!Visited.insert(ChildExpr).second) continue;

      VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));
      continue;
    }

    // Retime the leaf if it is not retimed yet.
    VASTValPtr &Retimed = RetimedMap[ChildNode];
    if (!Retimed) Retimed = retimeValToSlot(ChildNode, ToSlot, RetimingPath);
  }

  VASTValPtr RetimedRoot = RetimedMap[RootExpr];
  assert(RetimedRoot && "RootExpr not visited?");
  return RetimedRoot;
}

//===----------------------------------------------------------------------===//
void ScheduleEmitter::handleNewSeqOp(VASTSeqInst *SeqOp) {

}

void ScheduleEmitter::handleNewSeqOp(VASTSlotCtrl *SeqOp) {}

void ScheduleEmitter::emitToSlot(VASTSeqOp *Op, VASTValPtr Pred,
                                 VASTSlot *ToSlot,
                                 SmallVectorImpl<BasicBlock*> &RetimingPath) {
  // Create the new SeqOp.
  switch (Op->getASTType()) {
  case VASTNode::vastSeqInst:
    cloneSeqInst(cast<VASTSeqInst>(Op), ToSlot, Pred, RetimingPath);
    break;
  case VASTNode::vastSlotCtrl:
    cloneSlotCtrl(cast<VASTSlotCtrl>(Op), ToSlot, Pred, RetimingPath);
    break;
  default: llvm_unreachable("Unexpected SeqOp type!");
  }
}

bool ScheduleEmitter::emitToFirstSlot(VASTValPtr Pred, VASTSlot *ToSlot,
                                      MutableArrayRef<VASTSchedUnit*> SUs,
                                      SmallVectorImpl<BasicBlock*> &RetimingPath) {
  assert(SUs[0]->isBBEntry() && "BBEntry not placed at the beginning!");
  unsigned EntrySlot = SUs[0]->getSchedule();
  RetimingPath.push_back(SUs[0]->getParent());

  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *SU = SUs[i];

    // Only emit the SUs in the same slot with the entry.
    if (SU->getSchedule() != EntrySlot) {
      RetimingPath.pop_back();
      return true;
    }

    // Ignore the pseudo scheduling units.
    if (SU->isPHI()) continue;

    emitToSlot(SU->getSeqOp(), Pred, ToSlot, RetimingPath);
  }

  RetimingPath.pop_back();
  return false;
}

void ScheduleEmitter::emitScheduleInBB(MutableArrayRef<VASTSchedUnit*> SUs) {
  assert(SUs[0]->isBBEntry() && "BBEntry not placed at the beginning!");
  unsigned EntrySlot = SUs[0]->getSchedule();
  // All SUs are scheduled to the same slot with the entry, hence they are all
  // folded to the predecessor of this BB.
  if (SUs.back()->getSchedule() == EntrySlot) return;

  BasicBlock *BB = SUs[0]->getParent();
  SmallVector<BasicBlock*, 4> RetimingPath(1, BB);

  unsigned LatestSlot = EntrySlot;
  VASTSlot *CurSlot = LandingSlots[BB];
  assert(CurSlot && "Landing Slot not created?");
  unsigned EntrySlotNum = CurSlot->SlotNum;

  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *CurSU = SUs[i];
    LatestSlot = CurSU->getSchedule();

    // Do not emit the scheduling units at the first slot of the BB. They had
    // already folded in the the last slot of its predecessors.
    if (LatestSlot == EntrySlot) continue;

    // Please note that the EntrySlot is actually folded into its predecessor's
    // last slot, hence we need to minus EntrySlot by 1
    unsigned CurSlotNum = EntrySlotNum + LatestSlot - EntrySlot - 1;
    // Create the slot if it is not created.
    while (CurSlotNum != CurSlot->SlotNum) {
      VASTSlot *NextSlot = VM.createSlot(CurSlot->SlotNum + 1, BB);
      ++NumSlots;
      addSuccSlot(CurSlot, NextSlot, VASTImmediate::True);
      CurSlot = NextSlot;
    }

    emitToSlot(CurSU->getSeqOp(), VASTImmediate::True, CurSlot, RetimingPath);
  }

  assert(RetimingPath.size() == 1 && "Path stack corrupt!");
}


void ScheduleEmitter::emitSchedule() {
  Function &F = VM;

  BasicBlock &Entry = F.getEntryBlock();

  SmallVector<BasicBlock*, 4> RetimingPath;
  RetimingPath.push_back(0);

  VASTValPtr StartPort = VM.getPort(VASTModule::Start).getValue();
  VASTSlot *StartSlot = VM.getStartSlot();
  MutableArrayRef<VASTSchedUnit*> EntrySUs = BBMap.getSUInBB(&Entry);

  if (emitToFirstSlot(StartPort, StartSlot, EntrySUs, RetimingPath)) {
    // Create the landing slot of entry BB if not all SUs in the Entry BB
    // emitted to the idle slot.
    ++NumSlots;
    VASTSlot *S = VM.createSlot(LandingSlotNum[&Entry], &Entry);
    LandingSlots[&Entry] = S;
    // Go to the new slot if the start port is true.
    addSuccSlot(VM.getStartSlot(), S, StartPort);
  }

  assert(RetimingPath.size() == 1 && "Path stack corrupt!");

  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    MutableArrayRef<VASTSchedUnit*> SUs(BBMap.getSUInBB(BB));
    emitScheduleInBB(SUs);
  }
}

//===----------------------------------------------------------------------===//

void VASTSchedGraph::emitSchedule(VASTModule &VM) {
  ScheduleEmitter Emitter(VM, *this);

  Emitter.initialize();

  Emitter.emitSchedule();
}
