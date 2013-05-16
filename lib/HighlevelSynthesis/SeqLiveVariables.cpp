//===- SeqLiveVariables.cpp - LiveVariables analysis on the STG -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the LiveVariable Analysis on the state-transition graph.
//
//===----------------------------------------------------------------------===//

#include "STGShortestPath.h"
#include "OverlappedSlots.h"
#include "SeqLiveVariables.h"

#include "shang/Passes.h"

#include "shang/VASTSeqOp.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTModule.h"
#include "shang/VASTModulePass.h"

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
  OS << "\n  Landings: ";
  ::dump(Landings, OS);
  OS << "\n  Overlappeds: ";
  ::dump(Overlappeds, OS);
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
char &llvm::SeqLiveVariablesID = SeqLiveVariables::ID;

INITIALIZE_PASS_BEGIN(SeqLiveVariables, "shang-seq-live-variables",
                      "Seq Live Variables Analysis", false, true)
  INITIALIZE_PASS_DEPENDENCY(OverlappedSlots)
INITIALIZE_PASS_END(SeqLiveVariables, "shang-seq-live-variables",
                    "Seq Live Variables Analysis", false, true)

Pass *llvm::createSeqLiveVariablesPass() {
  return new SeqLiveVariables();
}

SeqLiveVariables::SeqLiveVariables() : VASTModulePass(ID) {
  initializeSeqLiveVariablesPass(*PassRegistry::getPassRegistry());
}

void SeqLiveVariables::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequired<OverlappedSlots>();
  AU.setPreservesAll();
}

void SeqLiveVariables::releaseMemory() {
  VarInfos.clear();
  WrittenSlots.clear();
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
    // Reset the context.
    VIs.clear();
    UnionMask.clear();
    OverlappedMask.clear();

    typedef VASTSelector::def_iterator def_iterator;
    for (def_iterator DI = Sel->def_begin(), DE = Sel->def_end(); DI != DE; ++DI) {
      VASTSeqValue *V = *DI;
      VIs.insert(getVarInfo(V));
    }

    typedef SmallPtrSet<VarInfo*, 8>::iterator vi_iterator;
    for (vi_iterator VI = VIs.begin(), VE = VIs.end(); VI != VE; ++VI) {
      VarInfo *VInfo = *VI;

      // FIXME: Check the overlapped slots more carefully.
      if (UnionMask.intersects(VInfo->Alives)
          || OverlappedMask.intersects(VInfo->Alives)
          || UnionMask.intersects(VInfo->Overlappeds)) {
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
      OverlappedMask |= VInfo->Overlappeds;
    }
  }
}

bool SeqLiveVariables::runOnVASTModule(VASTModule &M) {
  VM = &M;

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
  std::vector<VASTSlot*> NodeStack;
  std::vector<VASTSlot::succ_iterator> ChildItStack;

  // Start from the Idle subgroup.
  NodeStack.push_back(IdleSubGrp);
  // Visit the child of the entry slot: Idle Group.
  handleSlot(Entry, NodeStack);
  NodeStack.push_back(Entry);
  ChildItStack.push_back(NodeStack.back()->succ_begin());

  // Prevent the entry node from being visited twice.
  Visited.insert(Entry);

  while (NodeStack.size() > 1) {
    VASTSlot *CurSlot = NodeStack.back();
    VASTSlot::succ_iterator It = ChildItStack.back();

    if (It == CurSlot->succ_end()) {
      NodeStack.pop_back();
      ChildItStack.pop_back();
      continue;
    }

    VASTSlot *ChildNode = *It;
    ++ChildItStack.back();

    // Is the Slot visited?
    if (!Visited.insert(ChildNode).second)  continue;

    // Visit the slots in depth-first order.
    handleSlot(ChildNode, NodeStack);

    NodeStack.push_back(ChildNode);
    ChildItStack.push_back(NodeStack.back()->succ_begin());
  }

  initializeLandingSlots();
  initializeOverlappedSlots();

#ifndef NDEBUG
  verifyAnalysis();
#endif

  return false;
}

static void setLandingSlots(VASTSlot *S, SparseBitVector<> &Landings) {
  typedef df_iterator<VASTSlot*> slot_df_iterator;
  for (slot_df_iterator DI = df_begin(S), DE = df_end(S); DI != DE; /*++DI*/) {
    VASTSlot *Child = *DI;

    // Ignore the current slot and any immediate reachable subgroups.
    if (Child == S || Child->IsSubGrp) {
      ++DI;
      continue;
    }

    Landings.set(Child->SlotNum);
    DI.skipChildren();
  }

  DEBUG(dbgs() << "Slot #" << S->SlotNum << " landing: ";
  ::dump(Landings, dbgs()));
}

void SeqLiveVariables::initializeLandingSlots() {
  // Setup the linding slot map.
  std::map<unsigned, SparseBitVector<> > LandingMap;
  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator I = VM->slot_begin(), E = VM->slot_end(); I != E; ++I) {
    VASTSlot *S = I;
    setLandingSlots(S, LandingMap[S->SlotNum]);
  }

  for (var_iterator I = VarList.begin(), E = VarList.end(); I != E; ++I) {
    VarInfo *VI = I;

    typedef SparseBitVector<>::iterator iterator;
    SparseBitVector<> CurLandings;
    for (iterator DI = VI->Defs.begin(), DE = VI->Defs.end(); DI != DE; ++DI) {
      unsigned DefSlotNum = *DI;
      std::map<unsigned, SparseBitVector<> >::const_iterator
        at = LandingMap.find(DefSlotNum);
      assert(at != LandingMap.end() && "Landing information does not exist!");
      CurLandings |= at->second;
    }

    // Trim the unreachable slots from the landing slot.
    for (iterator LI = CurLandings.begin(), LE = CurLandings.end();
         LI != LE; ++LI) {
      unsigned Landing = *LI;
      if (VI->isSlotReachable(Landing)) VI->Landings.set(Landing);
    }
  }
}

void SeqLiveVariables::initializeOverlappedSlots() {
  OverlappedSlots &Overlappeds = getAnalysis<OverlappedSlots>();

  for (var_iterator I = VarList.begin(), E = VarList.end(); I != E; ++I) {
    VarInfo *VI = I;

    // Iterate over all reachable slots to set the overlapped slots.
    Overlappeds.setOverlappedSlots(VI->Alives, VI->Overlappeds);
    Overlappeds.setOverlappedSlots(VI->Kills, VI->Overlappeds);
    Overlappeds.setOverlappedSlots(VI->DefKills, VI->Overlappeds);
  }
}

void SeqLiveVariables::handleSlot(VASTSlot *S, PathVector PathFromEntry) {
  std::set<VASTSeqValue*> ReadAtSlot;

  typedef VASTSlot::const_op_iterator op_iterator;
  for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
    VASTSeqOp *SeqOp = *I;

    // Process all uses.
    typedef VASTOperandList::const_op_iterator read_itetator;
    for (read_itetator UI = SeqOp->op_begin(), UE = SeqOp->op_end();
          UI != UE; ++UI)
      if (VASTValue *V = UI->unwrap().get())
        V->extractSupporingSeqVal(ReadAtSlot);

    // The Slot Register are also used.
    if (SeqOp->getSlot()->isSynthesized())
      ReadAtSlot.insert(SeqOp->getSlot()->getValue());
  }

  // Process uses.
  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = ReadAtSlot.begin(), E = ReadAtSlot.end(); I != E; ++I)
    // Ignore the placeholder for node without timing information.
    if (VASTSeqValue *V = *I) handleUse(V, S, PathFromEntry);
}

void SeqLiveVariables::createInstVarInfo(VASTModule *VM) {
  // Also add the VarInfo for the static registers.
  typedef VASTModule::seqval_iterator iterator;

  for (iterator I = VM->seqval_begin(), E = VM->seqval_end(); I != E; ++I) {
    VASTSeqValue *V = I;

    if (V->isStatic()) {
      VarInfo *VI = new VarInfo(0);
      VarList.push_back(VI);

      // The static register is implicitly defined at the entry slot.
      VASTSlot *S = VM->getStartSlot();
      VI->initializeDefSlot(S->SlotNum);

      VarInfos[V] = VI;
      WrittenSlots[V].set(S->SlotNum);
      continue;
    } 

    VarInfo *VI = new VarInfo(0);
    VarList.push_back(VI);

    typedef VASTSeqValue::fanin_iterator iterator;
    for (iterator DI = V->fanin_begin(), DE = V->fanin_end(); DI != DE; ++DI) {
      VASTLatch U = *DI;
      VASTSlot *DefSlot = U.getSlot();

      // Initialize the definition slot.
      VI->initializeDefSlot(DefSlot->SlotNum);
      WrittenSlots[V].set(DefSlot->SlotNum);
    }

    VarInfos[V] = VI;
  }
}

void SeqLiveVariables::handleUse(VASTSeqValue *Use, VASTSlot *UseSlot,
                                 PathVector PathFromEntry) {
  // The timing information is not avaliable.
  // if (Use->empty()) return;

  assert(Use && "Bad Use pointer!");
  VASTSlot *DefSlot = 0;

  // Walking backward to find the corresponding definition.
  // Provide the path which stop at prior slot (not a subgroup of the slot),
  // otherwise we will calculate a wrong define slot when the current slot is
  // a virtual slot. Consider the following example:
  //   S1 <- D1
  //   S2 <- D2
  //   S3' <- U1
  // In the example, there is definition D1 at S1 and definition D2 at S2.
  // There is a read U1 at S3', the subgroup of S2. For U1, it reads the value
  // produced by D1 instead of D2, because D2 and U1 are actually scheduled
  // to the same slot, hence the assignment at S2 is not available at S3'.
  bool IgnoreSlot = UseSlot->IsSubGrp;
  typedef PathVector::reverse_iterator path_iterator;
  for (path_iterator I = PathFromEntry.rbegin(), E = PathFromEntry.rend();
       I != E; ++I) {
    VASTSlot *S = *I;
    if (IgnoreSlot) {

      if (!S->IsSubGrp) IgnoreSlot = false;

      continue;
    }

    // Find the nearest written slot in the path.
    if (isWrittenAt(Use, S)) {
      DefSlot = S;
      break;
    }
  }

  if (!DefSlot) {
    dbgs() << "Dumping path:[\n";
    typedef PathVector::iterator iterator;
    for (iterator I = PathFromEntry.begin(), E = PathFromEntry.end(); I != E; ++I)
      dbgs() << (*I)->SlotNum << '\n';
    dbgs() << "]\n";

    Use->dumpFanins();

    UseSlot->dump();

    llvm_unreachable("Define of VASTSeqVal not dominates all its uses!");
  }

  DEBUG(dbgs() << "SeqVal: " << Use->getName() << " Used at Slot "
               << UseSlot->SlotNum << " Def at slot " << DefSlot->SlotNum
               << '\n');

  // Get the corresponding VarInfo defined at DefSlot.
  VarInfo *VI = getVarInfo(Use);

  if (UseSlot == DefSlot) {
    assert(UseSlot->IsSubGrp && UseSlot->getParent() == 0
           && "Unexpected Cycle!");
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

  // The value not killed at define slot anymore.
  VI->Kills.reset(DefSlot->SlotNum);

  typedef VASTSlot::pred_iterator ChildIt;
  std::vector<std::pair<VASTSlot*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(UseSlot, UseSlot->pred_begin()));

  while (!VisitStack.empty()) {
    VASTSlot *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == Node->pred_end()) {
      VisitStack.pop_back();
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTSlot *ChildNode = *It;
    ++VisitStack.back().second;

    // Is the value defined in this slot?
    if (VI->Defs.test(ChildNode->SlotNum)) {
      // The current slot is defined, but also killed!
      if (ChildNode == UseSlot) VI->DefKills.set(UseSlot->SlotNum);

      // If we can reach a define slot, the define slot is not dead.
      VI->Kills.reset(ChildNode->SlotNum);

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

bool SeqLiveVariables::isWrittenAt(VASTSeqValue *V, VASTSlot *S) {
  std::map<VASTSeqValue*, SparseBitVector<> >::iterator at
    = WrittenSlots.find(V);
  assert(at != WrittenSlots.end() && "Definition of V not visited yet!");

  return at->second.test(S->SlotNum);
}

unsigned SeqLiveVariables::getIntervalFromDef(const VASTSeqValue *V,
                                              VASTSlot *ReadSlot,
                                              STGShortestPath *SSP) const {
  const VarInfo *VI = getVarInfo(V);
  unsigned ReadSlotNum = ReadSlot->SlotNum;

  // Calculate the Shortest path distance from all live-in slot.
  unsigned IntervalFromLanding = STGShortestPath::Inf;
  typedef SparseBitVector<>::iterator def_iterator;
  for (def_iterator I = VI->Landings.begin(), E = VI->Landings.end();
       I != E; ++I) {
    unsigned LandingSlotNum = *I;

    // Directly read at the landing slot, the interval is 0.
    if (LandingSlotNum == ReadSlotNum) {
      IntervalFromLanding = 0;
      break;
    }

    unsigned CurInterval = SSP->getShortestPath(LandingSlotNum, ReadSlotNum);
    if (CurInterval >= STGShortestPath::Inf) {
      dbgs() << "Read at slot: " << ReadSlotNum << '\n';
      dbgs() << "Landing slot: " << LandingSlotNum << '\n';
      VI->dump();
      dbgs() <<  "Alive slot not reachable?\n";
      continue;
    }

    IntervalFromLanding = std::min(IntervalFromLanding, CurInterval);
  }

  assert(IntervalFromLanding < STGShortestPath::Inf && "No live-in?");

  // The is 1 extra cycle from the definition to live in.
  return IntervalFromLanding + 1;
}
