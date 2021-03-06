//===- SeqLiveVariables.cpp - LiveVariables analysis on the STG -*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the LiveVariable Analysis on the state-transition graph.
//
//===----------------------------------------------------------------------===//
#include "SeqLiveVariables.h"

#include "vast/Passes.h"

#include "vast/VASTSeqOp.h"
#include "vast/VASTSeqValue.h"
#include "vast/VASTModule.h"
#include "vast/VASTModulePass.h"

#include "llvm/Analysis/Dominators.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/Support/ErrorHandling.h"
#define DEBUG_TYPE "shang-live-variables"
#include "llvm/Support/Debug.h"

using namespace llvm;

void SeqLiveVariables::VarInfo::print(raw_ostream &OS) const {
  if (V) V->print(OS);

  OS << "\n  Defined in Slots: ";
  ::dump(Defs, OS);
  OS << "\n  Alive in Slots: ";
  ::dump(Alives, OS);
  OS << "\n  Killed by:";
  ::dump(Kills, OS);
  OS << "\n  Killed-Defined: ";
  ::dump(DefKills, OS);
  OS << "\n";
}

void SeqLiveVariables::VarInfo::dump() const {
  print(dbgs());
}

void SeqLiveVariables::VarInfo::verifyKillAndAlive() const {
  if (Alives.intersects(Kills)) {
    dbgs() << "Bad VarInfo: \n";
    dump();
    SparseBitVector<> OverlapMask = Alives & Kills;
    dbgs() << "Overlap:\n";
    ::dump(OverlapMask, dbgs());
    llvm_unreachable("Kills and Alives should not intersect!");
  }

  if (Kills.empty() && DefKills.empty()) {
    dbgs() << "Bad VarInfo: \n";
    dump();
    llvm_unreachable("There should always be a kill!");
    return;
  }
}

void SeqLiveVariables::VarInfo::verify() const {
  verifyKillAndAlive();
}

char SeqLiveVariables::ID = 0;
char &vast::SeqLiveVariablesID = SeqLiveVariables::ID;

INITIALIZE_PASS_BEGIN(SeqLiveVariables, "shang-seq-live-variables",
                      "Seq Live Variables Analysis", false, true)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
INITIALIZE_PASS_END(SeqLiveVariables, "shang-seq-live-variables",
                    "Seq Live Variables Analysis", false, true)

Pass *vast::createSeqLiveVariablesPass() {
  return new SeqLiveVariables();
}

SeqLiveVariables::SeqLiveVariables() : VASTModulePass(ID) {
  initializeSeqLiveVariablesPass(*PassRegistry::getPassRegistry());
}

void SeqLiveVariables::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequired<DominatorTree>();
  AU.setPreservesAll();
}

void SeqLiveVariables::releaseMemory() {
  VarInfos.clear();
  VarList.clear();
}

void SeqLiveVariables::print(raw_ostream &OS) const {
  OS << "\n\nSeqLiveVariables:\n";
  SmallPtrSet<VarInfo*, 8> VIs;

  typedef VASTModule::const_selector_iterator iterator;
  llvm_unreachable("Not implemented!");
  //for (iterator I = VM->seqval_begin(), E = VM->seqval_end(); I != E; ++I) {
  //  const VASTSelector *V = I;
  //  VIs.clear();

  //  typedef VASTSelector::const_iterator latch_iterator;
  //  for (latch_iterator DI = V->begin(), DE = V->end(); DI != DE; ++DI) {
  //    std::map<VarName, VarInfo*>::const_iterator at = VarInfos.find(*DI);
  //    if (at != VarInfos.end()) VIs.insert(getVarInfo(*DI));
  //  }

  //  if (VIs.empty()) continue;

  //  OS << V->getName() << ":\n";

  //  typedef SmallPtrSet<VarInfo*, 8>::iterator vi_iterator;
  //  for (vi_iterator VI = VIs.begin(), VE = VIs.end(); VI != VE; ++VI) {
  //    VarInfo *VInfo = *VI;
  //    VInfo->print(OS);
  //    OS << '\n';
  //  }
  //}

  OS << "\n\n\n";
}

void SeqLiveVariables::dumpVarInfoSet(SmallPtrSet<VarInfo*, 8> VIs) {
  typedef SmallPtrSet<VarInfo*, 8>::iterator vi_iterator;
  for (vi_iterator VI = VIs.begin(), VE = VIs.end(); VI != VE; ++VI) {
    VarInfo *V = *VI;
    dbgs() << "VarInfo[" << V  << "]:";
    (*VI)->dump();
  }
}

void SeqLiveVariables::verifyAnalysis() const {
  SparseBitVector<> UnionMask, OverlappedMask;
  SmallPtrSet<VarInfo*, 8> VIs;

  // Verify the VarInfo itself first.
  for (const_var_iterator I = VarList.begin(), E = VarList.end(); I != E; ++I)
    I->verify();

  // The liveness of the variable information derived from the same SeqVal should
  // not overlap.
  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM->selector_begin(), E = VM->selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;

    // Ignore the Selectors that is never assigned.
    if (Sel->empty()) continue;

    // Reset the context.
    VIs.clear();
    UnionMask.clear();

    typedef VASTSelector::def_iterator def_iterator;
    for (def_iterator DI = Sel->def_begin(), DE = Sel->def_end(); DI != DE; ++DI) {
      VASTSeqValue *V = *DI;
      VIs.insert(getVarInfo(V));
    }

    typedef SmallPtrSet<VarInfo*, 8>::iterator vi_iterator;
    for (vi_iterator VI = VIs.begin(), VE = VIs.end(); VI != VE; ++VI) {
      VarInfo *VInfo = *VI;

      // FIXME: Check the overlapped slots more carefully.
      if (UnionMask.intersects(VInfo->Alives)) {
        dbgs() << "Current VASTSeqVal: " << Sel->getName() << '\n';
        dumpVarInfoSet(VIs);
        dbgs() << "Overlap slots:\n";

        SparseBitVector<> Overlap = UnionMask & VInfo->Alives;
        ::dump(Overlap, dbgs());

        dbgs() << "All slots:\n";
        ::dump(UnionMask, dbgs());

        llvm_unreachable("VarInfo of the same SeqVal alive slot overlap!");
      }

      // Construct the union.
      UnionMask |= VInfo->Alives;
      UnionMask |= VInfo->Kills;
      UnionMask |= VInfo->DefKills;
    }
  }
}

bool SeqLiveVariables::runOnVASTModule(VASTModule &M) {
  VM = &M;
  DT = &getAnalysis<DominatorTree>();
  // Compute the PHI joins.
  createInstVarInfo(VM);

  // Calculate live variable information in depth first order on the CFG of the
  // function.  This guarantees that we will see the definition of a virtual
  // register before its uses due to dominance properties of SSA (except for PHI
  // nodes, which are treated as a special case).
  VASTSlot *Entry = VM->getStartSlot();
  VASTSlot *IdleSubGrp = Entry->getSubGroup(0);
  assert(IdleSubGrp && "Idle subgroup does not exist?");

  std::set<VASTSlot*> Visited;
  std::vector<std::pair<VASTSlot*, VASTSlot::succ_iterator> > WorkStack;

  // Visit the child of the entry slot: Idle Group.
  handleSlot(Entry);
  WorkStack.push_back(std::make_pair(Entry, Entry->succ_begin()));

  // Prevent the entry node from being visited twice.
  Visited.insert(Entry);

  while (!WorkStack.empty()) {
    VASTSlot *CurSlot = WorkStack.back().first;
    VASTSlot::succ_iterator It = WorkStack.back().second;

    if (It == CurSlot->succ_end()) {
      WorkStack.pop_back();
      continue;
    }

    VASTSlot::EdgePtr Edge = *It;
    ++WorkStack.back().second;

    // Do not go through the implicit flow, in this case we may missed the legal
    // definition of a use.
    if (Edge.getType() == VASTSlot::ImplicitFlow)
      continue;

    VASTSlot *ChildNode = Edge;

    // Is the Slot visited?
    if (!Visited.insert(ChildNode).second)  continue;

    // Visit the slots in depth-first order.
    handleSlot(ChildNode);

    WorkStack.push_back(std::make_pair(ChildNode, ChildNode->succ_begin()));
  }

#ifndef NDEBUG
  verifyAnalysis();
#endif

  return false;
}

void SeqLiveVariables::handleSlot(VASTSlot *S) {
  std::set<VASTSeqValue*> ReadAtSlot;

  typedef VASTSlot::const_op_iterator op_iterator;
  for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
    VASTSeqOp *SeqOp = *I;

    // Process all uses.
    typedef VASTOperandList::const_op_iterator read_itetator;
    for (read_itetator UI = SeqOp->op_begin(), UE = SeqOp->op_end();
          UI != UE; ++UI)
      if (VASTValue *V = UI->unwrap().get())
        V->extractCombConeLeaves(ReadAtSlot);

    // The Slot Register are also used.
    if (SeqOp->getSlot()->isSynthesized())
      ReadAtSlot.insert(SeqOp->getSlot()->getValue());
  }

  // Process uses.
  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = ReadAtSlot.begin(), E = ReadAtSlot.end(); I != E; ++I) {
    VASTSeqValue *V = *I;

    // Ignore the directly output of functional units, there should be always
    // single cycle paths between it and the registers.
    if (V->fanin_empty()) continue;

    // Ignore the placeholder for node without timing information.
    handleUse(V, S);
  }
}

void SeqLiveVariables::createInstVarInfo(VASTModule *VM) {
  // Also add the VarInfo for the static registers.
  typedef VASTModule::seqval_iterator iterator;

  for (iterator I = VM->seqval_begin(), E = VM->seqval_end(); I != E; ++I) {
    VASTSeqValue *V = I;

    // Ignore the SeqValue that is never assigned.
    if (V->fanin_empty()) continue;

    if (V->isStatic()) {
      VarInfo *VI = new VarInfo(V->getLLVMValue());
      VarList.push_back(VI);

      // The static register is implicitly defined at the entry slot.
      VASTSlot *S = VM->getStartSlot();
      VI->initializeDefSlot(S->SlotNum);

      VarInfos[V] = VI;
      continue;
    } 

    VarInfo *VI = new VarInfo(V->getLLVMValue());
    VarList.push_back(VI);

    typedef VASTSeqValue::fanin_iterator iterator;
    for (iterator DI = V->fanin_begin(), DE = V->fanin_end(); DI != DE; ++DI) {
      VASTLatch U = *DI;
      VASTSlot *DefSlot = U.getSlot();

      // Initialize the definition slot.
      VI->initializeDefSlot(DefSlot->SlotNum);
    }

    VarInfos[V] = VI;
  }
}

bool SeqLiveVariables::dominates(BasicBlock *BB, VASTSlot *S) const {
  return DT->dominates(BB, S->getParentState()->getParent());
}

void SeqLiveVariables::handleUse(VASTSeqValue *Def, VASTSlot *UseSlot) {
  // The timing information is not avaliable.
  // if (Def->empty()) return;

  assert(Def && "Bad Def pointer!");
  Instruction *Inst = dyn_cast_or_null<Instruction>(Def->getLLVMValue());
  assert((!Inst || dominates(Inst->getParent(), UseSlot))
         && "Define not dominates its use!");

  // Get the corresponding VarInfo defined at DefSlot.
  VarInfo *VI = getVarInfo(Def);

  if (UseSlot->IsSubGrp && UseSlot->getParent() == 0) {
    VI->DefKills.set(UseSlot->SlotNum);

    // If we can reach a define slot, the define slot is not dead.
    VI->Kills.reset(UseSlot->SlotNum);

    return;
  }

  // This variable is known alive at this slot, that means there is some even
  // later use. We do not need to do anything.
  if (VI->Alives.test(UseSlot->SlotNum)) return;
  // Otherwise we can simply assume this is the kill slot. Please note that
  // This situation can occur:
  // \      ,-.
  // slot_a   |
  //   |      |
  // slot_b---'
  //
  // where slot_b is reachable via slot_a's predecessors, in this case the kill
  // flag for slot_a will be reset. And the kill flag for slot_b will be set
  // when we handling the use at slot_b. What we need to ensure is that we will
  // always visit slot_a first then slot_b, otherwise we will get the VarInfo
  // that killed at slot_a but alive at slot_b! If slot_a dominates slot_b,
  // we will visit slot_a first. In case that slot_a not dominates slot_b, this
  // may become a PROBLEM. However, such CFG should only be generated when user
  // is using the goto statement.
  VI->Kills.set(UseSlot->SlotNum);
  //assert(!VI->Defs.test(UseSlot->SlotNum));

  // The value not killed at define slots anymore.
  VI->Kills.intersectWithComplement(VI->Defs);

  // Incase the current slot also a define slot, the current slot become a
  // defkill.
  if (VI->Defs.test(UseSlot->SlotNum))
    VI->DefKills.set(UseSlot->SlotNum);

  typedef VASTSlot::pred_iterator ChildIt;
  std::vector<std::pair<VASTSlot*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(UseSlot, UseSlot->pred_begin()));

  while (!VisitStack.empty()) {
    VASTSlot *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second++;

    // We have visited all children of current node.
    if (It == Node->pred_end()) {
      VisitStack.pop_back();
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTSlot *ChildNode = *It;

    // All uses of a (SSA) define should be within the dominator tree rooted on
    // the define block.
    if (Inst && !dominates(Inst->getParent(), ChildNode))
      continue;

    // Is the value defined in this slot?
    if (VI->Defs.test(ChildNode->SlotNum)) {
      // The current slot is defined, but also killed!
      if (ChildNode == UseSlot) VI->DefKills.set(ChildNode->SlotNum);

      // Do not move across the define slot.
      continue;
    }

    // Reach the alive slot, no need to further visit other known AliveSlots.
    // Please note that test_and_set will return true if the bit is newly set.
    if (VI->Alives.test(ChildNode->SlotNum)) continue;

    // We had got a loop!
    if (UseSlot == ChildNode) continue;

    // Update the live slots.
    VI->Alives.set(ChildNode->SlotNum);
    VI->Kills.reset(ChildNode->SlotNum);

    VisitStack.push_back(std::make_pair(ChildNode, ChildNode->pred_begin()));
  }
  
  DEBUG(VI->verifyKillAndAlive();VI->dump(); dbgs() << '\n');
}
