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

#include "SeqLiveVariables.h"

#include "vtm/Passes.h"
#include "vtm/VerilogBackendMCTargetDesc.h"

#include "vtm/VInstrInfo.h"

#include "vtm/VASTModule.h"
#include "vtm/VASTControlPathNodes.h"
#include "vtm/VerilogModuleAnalysis.h"

#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineBasicBlock.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/DepthFirstIterator.h"
#define DEBUG_TYPE "vtm-live-variables"
#include "llvm/Support/Debug.h"

using namespace llvm;

SeqLiveVariables::VarName::VarName(VASTSeqUse U)
  : Dst(U.getDst()), S(U.Op->getSlot()) {}

SeqLiveVariables::VarName::VarName(VASTSeqDef D)
  : Dst(D), S(D->getSlot()) {}

void SeqLiveVariables::VarInfo::dump() const {
  if (isPHI()) dbgs() << "  [PHI]";

  if (MachineInstr *MI = DefMI.getPointer())
    MI->print(dbgs());

  dbgs() << "  Defined in Slots: ";

  typedef SparseBitVector<>::iterator iterator;
  for (iterator I = DefSlots.begin(), E = DefSlots.end(); I != E; ++I)
    dbgs() << *I << ", ";

  dbgs() << "\n  Alive in Slots: ";

  typedef SparseBitVector<>::iterator iterator;
  for (iterator I = AliveSlots.begin(), E = AliveSlots.end(); I != E; ++I)
    dbgs() << *I << ", ";

  dbgs() << "\n  Killed by:";

  if (Kills.empty())
    dbgs() << " No VASTSeqOp.\n";
  else {
    for (iterator I = Kills.begin(), E = Kills.end(); I != E; ++I)
      dbgs() << *I << ", ";

    dbgs() << "\n";
  }
}

void SeqLiveVariables::VarInfo::verify() const {
  if (AliveSlots.intersects(Kills)) {
    dbgs() << "Bad VarInfo: \n";
    dump();
    llvm_unreachable("Kills and Alives should not intersect!");
  }
}

char SeqLiveVariables::ID = 0;

INITIALIZE_PASS_BEGIN(SeqLiveVariables, "SeqLiveVariables",
                      "SeqLiveVariables", false, false)
  INITIALIZE_PASS_DEPENDENCY(VerilogModuleAnalysis);
INITIALIZE_PASS_END(SeqLiveVariables, "SeqLiveVariables",
                    "SeqLiveVariables", false, false)

Pass *llvm::createSeqLiveVariablesPass() {
  return new SeqLiveVariables();
}

SeqLiveVariables::SeqLiveVariables() : MachineFunctionPass(ID) {
  initializeSeqLiveVariablesPass(*PassRegistry::getPassRegistry());
}

void SeqLiveVariables::getAnalysisUsage(AnalysisUsage &AU) const {
  MachineFunctionPass::getAnalysisUsage(AU);
  AU.addRequired<VerilogModuleAnalysis>();
  AU.setPreservesAll();
}

void SeqLiveVariables::releaseMemory() {
  VarInfos.clear();
  WrittenSlots.clear();
  Allocator.Reset();
}

void SeqLiveVariables::dumpVarInfoSet(SmallPtrSet<VarInfo*, 8> VIs) {
  typedef SmallPtrSet<VarInfo*, 8>::iterator vi_iterator;
  for (vi_iterator VI = VIs.begin(), VE = VIs.end(); VI != VE; ++VI)
    (*VI)->dump();
}

void SeqLiveVariables::verifyAnalysis() const {
  VASTModule *VM = getAnalysis<VerilogModuleAnalysis>();
  SparseBitVector<> OverlapMask;
  SmallPtrSet<VarInfo*, 8> VIs;

  // The liveness of the variable information derived from the same SeqVal should
  // not overlap.
  typedef VASTModule::seqval_iterator seqval_iterator;
  for (seqval_iterator I = VM->seqval_begin(), E = VM->seqval_end(); I != E; ++I) {
    VASTSeqValue *V = I;
    // Reset the context.
    VIs.clear();
    OverlapMask.clear();

    typedef VASTSeqValue::const_itertor iterator;
    for (iterator DI = V->begin(), DE = V->end(); DI != DE; ++DI) {
      std::map<VarName, VarInfo*>::const_iterator at = VarInfos.find(*DI);
      if (at != VarInfos.end()) VIs.insert(getVarInfo(*DI));
    }

    typedef SmallPtrSet<VarInfo*, 8>::iterator vi_iterator;
    for (vi_iterator VI = VIs.begin(), VE = VIs.end(); VI != VE; ++VI) {
      VarInfo *VInfo = *VI;
      // Verify the VarInfo itself first.
      VInfo->verify();

      if (OverlapMask.intersects(VInfo->AliveSlots)) {
        dumpVarInfoSet(VIs);
        llvm_unreachable("VarInfo of the same SeqVal alive slot overlap!");
      }

      // Construct the union.
      OverlapMask |= VInfo->AliveSlots;
      OverlapMask |= VInfo->Kills;
    }
  }
}

bool SeqLiveVariables::runOnMachineFunction(MachineFunction &MF) {
  VASTModule *VM = getAnalysis<VerilogModuleAnalysis>();

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

  // Compute the join slots for corresponding to a basic block.
  // Because of BB bypassing during VAST construction, there may be no any
  // VASTSlot corresponding to a BB
  std::map<MachineBasicBlock*, std::map<VASTSeqValue*, VarInfo*> > PHIInfos;
  // Remeber the VarInfo defined
  std::map<MachineInstr *, VarInfo*> InstVarInfo;
  for (seqop_iterator I = VM->seqop_begin(), E = VM->seqop_end(); I != E; ++I) {
    VASTSeqOp *SeqOp = I;
    MachineInstr *DefMI = SeqOp->getDefMI();

    if (DefMI == 0) continue;

    unsigned SlotNum = SeqOp->getSlotNum();

    // Only compute the PHIJoins for VOpMvPhi.
    if (DefMI->getOpcode() == VTM::VOpMvPhi) {
      VASTSeqDef Def = SeqOp->getDef(0);
      MachineBasicBlock *TargetBB = DefMI->getOperand(2).getMBB();

      // Create the VarInfo for the PHI.
      VarInfo *&VI = PHIInfos[TargetBB][Def];
      if (VI == 0) VI = new (Allocator) VarInfo(DefMI, true);
      // Set the defined slot.
      VI->DefSlots.set(SlotNum);
      VI->Kills.set(SlotNum);
      WrittenSlots[Def].set(SlotNum);
      VarInfos[Def] = VI;
    } else if (SeqOp->getNumDefs()) {
      assert(SeqOp->getNumDefs() == 1 && "Multi-definition not supported yet!");
      VASTSeqDef Def = SeqOp->getDef(0);

      VarInfo *&VI = InstVarInfo[DefMI];
      if (VI == 0) VI = new (Allocator) VarInfo(DefMI);
      // Set the defined slot.
      VI->DefSlots.set(SlotNum);
      VI->Kills.set(SlotNum);
      WrittenSlots[Def].set(SlotNum);
      VarInfos[Def] = VI;
    }
  }
}

void SeqLiveVariables::handleUse(VASTSeqValue *Use, VASTSlot *UseSlot,
                                 PathVector &PathFromEntry) {
  // The timing information is not avaliable.
  if (Use->empty() || Use->getValType() == VASTNode::Slot) return;

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

  DEBUG(VI->verify();VI->dump(); dbgs() << '\n');
}

void SeqLiveVariables::handleDef(VASTSeqDef Def) {
  VarInfo *&V = VarInfos[Def];
  if (V) return;

  // Create and initialize the VarInfo if necessary.
  V = new (Allocator) VarInfo();

  // Multi-
  if (V->DefSlots.count()) return;

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
