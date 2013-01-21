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
  if (IsPHI) dbgs() << "  [PHI]";

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
  if (AliveSlots.intersects(Kills))
    llvm_unreachable("Kills and Alives should not intersect!");
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

void SeqLiveVariables::verifyAnalysis() const {

}

bool SeqLiveVariables::runOnMachineFunction(MachineFunction &MF) {
  VASTModule *VM = getAnalysis<VerilogModuleAnalysis>();

  // Compute the PHI joins.
  createPHIVarInfo(VM);

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

void SeqLiveVariables::createPHIVarInfo(VASTModule *VM) {
  typedef VASTModule::seqop_iterator seqop_iterator;

  // Compute the join slots for corresponding to a basic block.
  // Because of BB bypassing during VAST construction, there may be no any
  // VASTSlot corresponding to a BB
  std::map<MachineBasicBlock*, std::map<VASTSeqValue*, VarInfo*> > PHIInfos;
  for (seqop_iterator I = VM->seqop_begin(), E = VM->seqop_end(); I != E; ++I) {
    VASTSeqOp *SeqOp = I;
    if (MachineInstr *DefMI = SeqOp->getDefMI()) {
      // Only compute the PHIJoins for VOpMvPhi.
      if (DefMI->getOpcode() != VTM::VOpMvPhi) continue;

      VASTSeqDef Def = SeqOp->getDef(0);
      MachineBasicBlock *TargetBB = DefMI->getOperand(2).getMBB();

      // Create the VarInfo for the PHI.
      VarInfo *&VI = PHIInfos[TargetBB][Def];
      if (VI == 0) VI = new (Allocator) VarInfo(true);
      // Set the defined slot.
      VI->DefSlots.set(SeqOp->getSlotNum());

      VarInfos[Def] = VI;
      WrittenSlots[Def].set(SeqOp->getSlotNum());
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
    dbgs() << "Dumping path:\n";
    typedef PathVector::const_iterator iterator;
    for (iterator I = PathFromEntry.begin(), E = PathFromEntry.end(); I != E; ++I)
      (*I)->dump();

    llvm_unreachable("Define of VASTSeqVal not dominates all its uses!");
  }

  dbgs() << "SeqVal: " << Use->getName() << " Used at Slot " << UseSlot->SlotNum
         << " Def at slot " << DefSlot->SlotNum << '\n';

  // Get the corresponding VarInfo defined at DefSlot.
  VarInfo *VI = getVarInfo(VarName(Use, DefSlot));

  if (UseSlot == DefSlot) return;

  // This variable is known alive at this slot, that means there is some even
  // later use. We do not need to do anything.
  if (VI->AliveSlots.test(UseSlot->SlotNum)) return;

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
    if (!VI->AliveSlots.test_and_set(ChildNode->SlotNum)) continue;

    VI->Kills.reset(ChildNode->SlotNum);

    VisitStack.push_back(std::make_pair(ChildNode, ChildNode->pred_begin()));
  }

  VI->Kills.set(UseSlot->SlotNum);

  VI->dump();

  dbgs() << '\n';
}

void SeqLiveVariables::handleDef(VASTSeqDef Def) {
  VarInfo *V = getVarInfo(Def);

  // VarInfo for PHI is already setuped.
  if (V->IsPHI) return;

  assert(V->AliveSlots.empty() && "Unexpected alive slots!");

  // Initialize the define slot.
  V->DefSlots.set(Def->getSlotNum());

  // If vr is not alive in any block, then defaults to dead.
  V->Kills.set(Def->getSlotNum());

  // Remember the written slots.
  VASTSlot *DefSlot = Def->getSlot();
  WrittenSlots[Def].set(Def->getSlotNum());
}

bool SeqLiveVariables::isWrittenAt(VASTSeqValue *V, VASTSlot *S) {
  std::map<VASTSeqValue*, SparseBitVector<> >::iterator at
    = WrittenSlots.find(V);
  assert(at != WrittenSlots.end() && "Definition of V not visited yet!");

  return at->second.test(S->SlotNum);
}
