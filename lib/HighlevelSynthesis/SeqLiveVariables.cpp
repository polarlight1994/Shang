//===- SeqLiveVariables.cpp - LiveVariables analysis on the STG -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the LiveVariable Analysis on the state-transition graph.
//
//===----------------------------------------------------------------------===//

#include "BBLandingSlots.h"
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
#define DEBUG_TYPE "shang-live-variables"
#include "llvm/Support/Debug.h"

using namespace llvm;

SeqLiveVariables::VarName::VarName(VASTSeqUse U)
  : Dst(U.getDst()), S(U.Op->getSlot()) {}

SeqLiveVariables::VarName::VarName(VASTSeqDef D)
  : Dst(D), S(D->getSlot()) {}


void SeqLiveVariables::VarInfo::print(raw_ostream &OS) const {
  if (hasMultiDef()) OS << "  [Multi-Def]";

  if (Value *Val = getValue()) Val->print(OS);

  OS << "  Defined in Slots: ";

  typedef SparseBitVector<>::iterator iterator;
  for (iterator I = DefSlots.begin(), E = DefSlots.end(); I != E; ++I)
    OS << *I << ", ";

  OS << "\n  Live-in Slots: ";
  for (iterator I = LiveInSlots.begin(), E = LiveInSlots.end(); I != E; ++I)
    OS << *I << ", ";

  OS << "\n  Alive in Slots: ";
  for (iterator I = AliveSlots.begin(), E = AliveSlots.end(); I != E; ++I)
    OS << *I << ", ";

  OS << "\n  Killed by:";

  if (Kills.empty())
    OS << " No VASTSeqOp.\n";
  else {
    for (iterator I = Kills.begin(), E = Kills.end(); I != E; ++I)
      OS << *I << ", ";

    OS << "\n";
  }
}

void SeqLiveVariables::VarInfo::dump() const {
  print(dbgs());
}

void SeqLiveVariables::VarInfo::verifyKillAndAlive() const {
  if (AliveSlots.intersects(Kills)) {
    dbgs() << "Bad VarInfo: \n";
    dump();
    llvm_unreachable("Kills and Alives should not intersect!");
  }

}

void SeqLiveVariables::VarInfo::verify() const {
  verifyKillAndAlive();

  SparseBitVector<> ReachableSlots(AliveSlots);
  ReachableSlots |= Kills;

  if (ReachableSlots.contains(LiveInSlots)) return;

  dbgs() << "Bad VarInfo: \n";
  dump();
  llvm_unreachable("Live-in slots unreachable!");
}

char SeqLiveVariables::ID = 0;
char &llvm::SeqLiveVariablesID = SeqLiveVariables::ID;

INITIALIZE_PASS_BEGIN(SeqLiveVariables, "SeqLiveVariables", "SeqLiveVariables",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(BBLandingSlots)
INITIALIZE_PASS_END(SeqLiveVariables, "SeqLiveVariables", "SeqLiveVariables",
                    false, true)

Pass *llvm::createSeqLiveVariablesPass() {
  return new SeqLiveVariables();
}

SeqLiveVariables::SeqLiveVariables() : VASTModulePass(ID) {
  initializeSeqLiveVariablesPass(*PassRegistry::getPassRegistry());
}

void SeqLiveVariables::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequired<BBLandingSlots>();
  AU.setPreservesAll();
}

void SeqLiveVariables::releaseMemory() {
  VarInfos.clear();
  WrittenSlots.clear();
  VarList.clear();
}

void SeqLiveVariables::print(raw_ostream &OS) const {
  typedef std::map<VarName, VarInfo*>::const_iterator iterator;
  OS << "\n\nSeqLiveVariables:\n";

  for (iterator I = VarInfos.begin(), E = VarInfos.end(); I != E; ++I) {
    OS << I->first.Dst->getName() << '@' << I->first.S->SlotNum << "\n\t";
    I->second->print(OS);
    OS << '\n';
  }

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
  SparseBitVector<> OverlapMask;
  SmallPtrSet<VarInfo*, 8> VIs;

  // Verify the VarInfo itself first.
  for (const_var_iterator I = VarList.begin(), E = VarList.end(); I != E; ++I)
    I->verify();

  // The liveness of the variable information derived from the same SeqVal should
  // not overlap.
  typedef VASTModule::seqval_iterator seqval_iterator;
  for (seqval_iterator I = VM->seqval_begin(), E = VM->seqval_end(); I != E; ++I) {
    VASTSeqValue *V = I;
    // Reset the context.
    VIs.clear();
    OverlapMask.clear();

    typedef VASTSeqValue::const_iterator iterator;
    for (iterator DI = V->begin(), DE = V->end(); DI != DE; ++DI) {
      std::map<VarName, VarInfo*>::const_iterator at = VarInfos.find(*DI);
      if (at != VarInfos.end()) VIs.insert(getVarInfo(*DI));
    }

    typedef SmallPtrSet<VarInfo*, 8>::iterator vi_iterator;
    for (vi_iterator VI = VIs.begin(), VE = VIs.end(); VI != VE; ++VI) {
      VarInfo *VInfo = *VI;

      if (OverlapMask.intersects(VInfo->AliveSlots)) {
        dbgs() << "Current VASTSeqVal: " << V->getName() << '\n';
        dumpVarInfoSet(VIs);
        dbgs() << "Overlap slots:\n";
        typedef SparseBitVector<>::iterator iterator;
        for (iterator I = OverlapMask.begin(), E = OverlapMask.end(); I != E; ++I)
          dbgs() << *I << ", ";
        llvm_unreachable("VarInfo of the same SeqVal alive slot overlap!");
      }

      // Construct the union.
      OverlapMask |= VInfo->AliveSlots;
      OverlapMask |= VInfo->Kills;
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
  std::set<VASTSlot*> Visited;
  std::vector<VASTSlot*> NodeStack;
  std::vector<VASTSlot::succ_iterator> ChildItStack;

  NodeStack.push_back(Entry);
  ChildItStack.push_back(NodeStack.back()->succ_begin());

  // Prevent the entry node from being visited twice.
  Visited.insert(Entry);
  handleSlot(Entry, NodeStack);

  while (!NodeStack.empty()) {
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

  fixLiveInSlots();

#ifndef NDEBUG
  verifyAnalysis();
#endif

  return false;
}

void SeqLiveVariables::handleSlot(VASTSlot *S, PathVector &PathFromEntry) {
  std::set<VASTSeqValue*> UsedAtSlot;

  typedef VASTSlot::const_op_iterator op_iterator;
  for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
    VASTSeqOp *SeqOp = *I;

    // Process all uses.
    typedef VASTOperandList::const_op_iterator use_itetator;
    for (use_itetator UI = SeqOp->op_begin(), UE = SeqOp->op_end();
          UI != UE; ++UI)
      if (VASTValue *V = UI->unwrap().get())
        V->extractSupporingSeqVal(UsedAtSlot);

    // Process defines.
    for (unsigned i = 0, e = SeqOp->getNumDefs(); i != e; ++i)
      handleDef(SeqOp->getDef(i));
  }

  // Process uses.
  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = UsedAtSlot.begin(), E = UsedAtSlot.end(); I != E; ++I)
    handleUse(*I, S, PathFromEntry);
}

void SeqLiveVariables::createInstVarInfo(VASTModule *VM) {
  typedef VASTModule::seqop_iterator seqop_iterator;
  BBLandingSlots &LandingSlots = getAnalysis<BBLandingSlots>();

  // Remeber the VarInfo defined
  std::map<Value*, VarInfo*> InstVarInfo;
  for (seqop_iterator I = VM->seqop_begin(), E = VM->seqop_end(); I != E; ++I) {
    VASTSeqOp *SeqOp = I;
    Value *V = SeqOp->getValue();

    if (V == 0) continue;

    VASTSlot *S = SeqOp->getSlot();
    unsigned SlotNum = S->SlotNum;

    if (SeqOp->getNumDefs() == 0) continue;

    assert(SeqOp->getNumDefs() == 1 && "Multi-definition not supported yet!");
    assert((!isa<VASTSeqInst>(SeqOp)
            || cast<VASTSeqInst>(SeqOp)->getSeqOpType() != VASTSeqInst::Launch)
            && "Launch Inst should not define anything!");
    VASTSeqDef Def = SeqOp->getDef(0);

    VASTSeqValue *SeqVal = Def;

    VarInfo *&VI = InstVarInfo[V];
    if (VI == 0) {
      VI = new VarInfo(V);
      VarList.push_back(VI);
    } else
      VI->setMultiDef();

    // Set the defined slot.
    VI->DefSlots.set(SlotNum);
    VI->Kills.set(SlotNum);

    WrittenSlots[Def].set(SlotNum);
    VarInfos[Def] = VI;

    // Compute the live-in slots if the operation is predicated. For the
    // unpredicated operations, their live-ins are computed in fixLiveInSlots.
    if (Instruction *Inst = dyn_cast<Instruction>(V)) {
      BasicBlock *BB = Inst->getParent();
      if (BB != S->getParent()) {
        // The instruction is predicated, the live-in slots of the current
        // definition is not all the successor slots of the current slot.
        const BBLandingSlots::SlotSet &S = LandingSlots.getLandingSlots(BB);
        typedef BBLandingSlots::SlotSet::const_iterator landing_iterator;
        for (landing_iterator LI = S.begin(), LE = S.end(); LI != LE; ++LI)
          VI->LiveInSlots.set((*LI)->SlotNum);
      }
    }
  }

  // Also add the VarInfo for the static registers.
  typedef VASTModule::seqval_iterator seqval_iterator;
  for (seqval_iterator I = VM->seqval_begin(), E = VM->seqval_end(); I != E; ++I)
  {
    VASTSeqValue *V = I;

    if (V->getValType() != VASTSeqValue::StaticRegister) continue;

    VarInfo *VI = new VarInfo(0);
    VarList.push_back(VI);

    // The static register is implicitly defined at the entry slot.
    VASTSlot *S = VM->getStartSlot();
    VI->DefSlots.set(S->SlotNum);
    VI->Kills.set(S->SlotNum);

    VarName VN(V, S);
    VarInfos[VN] = VI;
    WrittenSlots[V].set(S->SlotNum);
  }
}

void SeqLiveVariables::handleUse(VASTSeqValue *Use, VASTSlot *UseSlot,
                                 PathVector &PathFromEntry) {
  // The timing information is not avaliable.
  if (Use->empty() || Use->getValType() == VASTSeqValue::Slot) return;

  assert(Use && "Bad Use pointer!");
  VASTSlot *DefSlot = 0;

  // Walking backward to find the corresponding definition.
  typedef PathVector::const_reverse_iterator path_iterator;
  for (path_iterator I = PathFromEntry.rbegin(), E = PathFromEntry.rend();
       I != E; ++I) {
    VASTSlot *S = *I;

    // Find the nearest written slot in the path.
    if (isWrittenAt(Use, S)) {
      DefSlot = S;
      break;
    }
  }

  if (!DefSlot) {
    DEBUG(dbgs() << "Dumping path:\n";
      typedef PathVector::const_iterator iterator;
      for (iterator I = PathFromEntry.begin(), E = PathFromEntry.end(); I != E; ++I)
      (*I)->dump());

    llvm_unreachable("Define of VASTSeqVal not dominates all its uses!");
  }

  DEBUG(dbgs() << "SeqVal: " << Use->getName() << " Used at Slot "
               << UseSlot->SlotNum << " Def at slot " << DefSlot->SlotNum
               << '\n');

  // Get the corresponding VarInfo defined at DefSlot.
  VarInfo *VI = getVarInfo(VarName(Use, DefSlot));

  if (UseSlot == DefSlot) return;

  // This variable is known alive at this slot, that means there is some even
  // later use. We do not need to do anything.
  if (VI->AliveSlots.test(UseSlot->SlotNum)) return;
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
    if (VI->DefSlots.test(ChildNode->SlotNum)) continue;

    // Reach the alive slot, no need to further visit other known AliveSlots.
    // Please note that test_and_set will return true if the bit is newly set.
    if (VI->AliveSlots.test(ChildNode->SlotNum)) continue;

    // Update the live slots.
    VI->AliveSlots.set(ChildNode->SlotNum);
    VI->Kills.reset(ChildNode->SlotNum);

    VisitStack.push_back(std::make_pair(ChildNode, ChildNode->pred_begin()));
  }

  DEBUG(VI->verifyKillAndAlive();VI->dump(); dbgs() << '\n');
}

void SeqLiveVariables::handleDef(VASTSeqDef Def) {
  VarInfo *&V = VarInfos[Def];
  if (V) return;

  // Create and initialize the VarInfo if necessary.
  V = new VarInfo();
  VarList.push_back(V);

  unsigned SlotNum = Def->getSlotNum();

  // Initialize the define slot.
  V->DefSlots.set(SlotNum);

  // If vr is not alive in any block, then defaults to dead.
  V->Kills.set(SlotNum);

  // Remember the written slots.
  WrittenSlots[Def].set(SlotNum);
}

bool SeqLiveVariables::isWrittenAt(VASTSeqValue *V, VASTSlot *S) {
  std::map<VASTSeqValue*, SparseBitVector<> >::iterator at
    = WrittenSlots.find(V);
  assert(at != WrittenSlots.end() && "Definition of V not visited yet!");

  return at->second.test(S->SlotNum);
}

void SeqLiveVariables::fixLiveInSlots() {
  std::map<unsigned, SparseBitVector<> > STGBitMap;
  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator I = VM->slot_begin(), E = VM->slot_end(); I != E; ++I) {
    VASTSlot *S = I;
    SparseBitVector<> &Succs = STGBitMap[S->SlotNum];

    typedef VASTSlot::succ_iterator succ_iterator;
    for (succ_iterator SI = S->succ_begin(), SE = S->succ_end(); SI != SE; ++SI) {
      VASTSlot *Succ = *SI;
      // The the definitions will never live-in Finish Slot and Start Slot.
      if (Succ != VM->getFinishSlot() && Succ != VM->getStartSlot())
        Succs.set(Succ->SlotNum);
    }
  }

  for (var_iterator I = VarList.begin(), E = VarList.end(); I != E; ++I) {
    VarInfo *VI = I;

    // Trim the dead live-ins.
    if (!VI->LiveInSlots.empty()) {
      VI->LiveInSlots &= VI->AliveSlots;
      continue;
    }

    // Otherwise compute the live-ins now.
    SparseBitVector<> LiveDef(VI->DefSlots);
    LiveDef.intersectWithComplement(VI->Kills);

    SparseBitVector<> LiveLiveIn;
    typedef SparseBitVector<>::iterator iterator;
    for (iterator I = LiveDef.begin(), E = LiveDef.end(); I != E; ++I) {
      unsigned LiveDefSlot = *I;
      LiveLiveIn |= STGBitMap[LiveDefSlot];
    }

    // If the live-ins are empty, the latching operation is unpredicated.
    // This mean the definition can reach all successors of the defining slot.
    // But we need to trim the dead live-ins according to its alive slots.
    VI->LiveInSlots |= LiveLiveIn & VI->AliveSlots;

    DEBUG(VI->dump());
  }
}
