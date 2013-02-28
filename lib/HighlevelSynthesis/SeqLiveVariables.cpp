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

#include "STGShortestPath.h"
#include "BBLandingSlots.h"
#include "SeqLiveVariables.h"

#include "shang/Passes.h"

#include "shang/VASTSeqOp.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTModule.h"
#include "shang/VASTModulePass.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/Support/ErrorHandling.h"
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

  OS << "\n  Defined in Slots: ";
  ::dump(Defs, OS);
  OS << "\n  Live-in Slots: ";
  ::dump(LiveIns, OS);
  OS << "\n  Alive in Slots: ";
  ::dump(Alives, OS);
  OS << "\n  Killed by:";
  ::dump(Kills, OS);
  OS << "\n  Killed-Defined: ";
  ::dump(DefKills, OS);
  OS << "\n  Alive-Defined: ";
  ::dump(DefAlives, OS);
  OS << "\n";
}

void SeqLiveVariables::VarInfo::dump() const {
  print(dbgs());
}

void SeqLiveVariables::VarInfo::verifyKillAndAlive() const {
  if (Alives.intersects(Kills) || Alives.intersects(Defs - DefAlives)
      || Alives.intersects(DefKills - DefAlives)) {
    dbgs() << "Bad VarInfo: \n";
    dump();
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

  if (LiveIns.empty() && Defs != Kills) {
    dbgs() << "Bad VarInfo: \n";
    dump();
    llvm_unreachable("There should always be a livein!");
    return;
  }

  SparseBitVector<> ReachableSlots(Alives);
  ReachableSlots |= Kills;
  ReachableSlots |= DefKills;

  if (ReachableSlots.contains(LiveIns)) return;

  dbgs() << "Bad VarInfo: \n";
  dump();
  llvm_unreachable("Live-in slots unreachable!");
}

char SeqLiveVariables::ID = 0;
char &llvm::SeqLiveVariablesID = SeqLiveVariables::ID;

INITIALIZE_PASS_BEGIN(SeqLiveVariables, "SeqLiveVariables", "SeqLiveVariables",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(BBLandingSlots)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
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
  AU.addRequired<DominatorTree>();
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

      if (OverlapMask.intersects(VInfo->Alives)) {
        dbgs() << "Current VASTSeqVal: " << V->getName() << '\n';
        dumpVarInfoSet(VIs);
        dbgs() << "Overlap slots:\n";
        typedef SparseBitVector<>::iterator iterator;
        for (iterator I = OverlapMask.begin(), E = OverlapMask.end(); I != E; ++I)
          dbgs() << *I << ", ";
        llvm_unreachable("VarInfo of the same SeqVal alive slot overlap!");
      }

      // Construct the union.
      OverlapMask |= VInfo->Alives;
      OverlapMask |= VInfo->Kills;
      OverlapMask |= VInfo->DefKills;
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
    ReadAtSlot.insert(SeqOp->getSlot()->getValue());

    // Process defines.
    for (unsigned i = 0, e = SeqOp->getNumDefs(); i != e; ++i)
      handleDef(SeqOp->getDef(i));
  }

  // Process uses.
  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = ReadAtSlot.begin(), E = ReadAtSlot.end(); I != E; ++I)
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

    VarInfo *&VI = InstVarInfo[V];
    if (VI == 0) {
      VI = new VarInfo(V);
      VarList.push_back(VI);
    } else
      VI->setMultiDef();

    // Set the defined slot.
    VI->initializeDefSlot(SlotNum);

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
          VI->LiveIns.set((*LI)->SlotNum);
      }
    }
  }

  // Also add the VarInfo for the static registers.
  typedef VASTModule::seqval_iterator seqval_iterator;
  for (seqval_iterator I = VM->seqval_begin(), E = VM->seqval_end(); I != E; ++I)
  {
    VASTSeqValue *V = I;

    if (V->getValType() == VASTSeqValue::StaticRegister) {
      VarInfo *VI = new VarInfo(0);
      VarList.push_back(VI);

      // The static register is implicitly defined at the entry slot.
      VASTSlot *S = VM->getStartSlot();
      VI->initializeDefSlot(S->SlotNum);

      VarName VN(V, S);
      VarInfos[VN] = VI;
      WrittenSlots[V].set(S->SlotNum);
    } else if (V->getValType() == VASTSeqValue::Slot) {
      unsigned SlotNum = V->getSlotNum();

      VarInfo *VI = new VarInfo(0);
      VarList.push_back(VI);
      // The definition of the Slot register always and only live-in to its slot!
      VI->LiveIns.set(SlotNum);

      typedef VASTSeqValue::const_iterator iterator;
      for (iterator DI = V->begin(), DE = V->end(); DI != DE; ++DI) {
        VASTSeqUse U = *DI;
        VASTSlot *DefSlot = U.getSlot();
        // Create anther VarInfo for the disable operation to the slot.
        if (DefSlot->SlotNum == SlotNum && !DefSlot->hasNextSlot(DefSlot)) {
          VarInfo *VI = new VarInfo(0);
          VarList.push_back(VI);
          VI->initializeDefSlot(SlotNum);
          WrittenSlots[V].set(SlotNum);

          VarName VN(V, DefSlot);
          VarInfos[VN] = VI;
          continue;
        }

        // Initialize the definition slot.
        VI->initializeDefSlot(DefSlot->SlotNum);
        WrittenSlots[V].set(DefSlot->SlotNum);
        VarName VN(V, DefSlot);
        VarInfos[VN] = VI;
      }
    }
  }
}

void SeqLiveVariables::handleUse(VASTSeqValue *Use, VASTSlot *UseSlot,
                                 PathVector &PathFromEntry) {
  // The timing information is not avaliable.
  if (Use->empty()) return;

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
        dbgs() << (*I)->SlotNum << '\n';

    Use->dumpFanins();

    UseSlot->dump();

    llvm_unreachable("Define of VASTSeqVal not dominates all its uses!");
  }

  DEBUG(dbgs() << "SeqVal: " << Use->getName() << " Used at Slot "
               << UseSlot->SlotNum << " Def at slot " << DefSlot->SlotNum
               << '\n');

  // Get the corresponding VarInfo defined at DefSlot.
  VarInfo *VI = getVarInfo(VarName(Use, DefSlot));

  if (UseSlot == DefSlot) {
    assert(UseSlot->SlotNum == 0 && "Unexpected Cycle!");
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

  // The value not killed at define slot anymore.
  VI->Kills.reset(DefSlot->SlotNum);

  typedef VASTSlot::pred_iterator ChildIt;
  std::vector<std::pair<VASTSlot*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(UseSlot, UseSlot->pred_begin()));

  bool HasMultiDef = VI->hasMultiDef();

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

      if (HasMultiDef) {
        SparseBitVector<> ReachableSlots(VI->Alives | VI->Kills | VI->DefKills);
        assert(!VI->LiveIns.empty() && "LiveIns should be provided for MultDef!");

        // For the MultiDef LiveVariable, it is the LiveIns that dominate all
        // its use.
        if (!ReachableSlots.intersects(VI->LiveIns)) {
#ifndef NDEBUG
          bool AnyLiveIn = false;
          for (unsigned i = 0, e = PathFromEntry.size(); i != e; ++i)
            if (VI->LiveIns.test(PathFromEntry[i]->SlotNum)) {
              AnyLiveIn = true;
              break;
            }

          assert(AnyLiveIn && "Even livein not dominates all its use!");
#endif

          VI->DefAlives.set(ChildNode->SlotNum);
        }
      }
      
      // Do not move across the define slot.
      if (!VI->DefAlives.test(ChildNode->SlotNum)) continue;
    }

    // Reach the alive slot, no need to further visit other known AliveSlots.
    // Please note that test_and_set will return true if the bit is newly set.
    if (VI->Alives.test(ChildNode->SlotNum)) continue;

    // We had got a loop!
    if (UseSlot == ChildNode) continue;

    // If we reach a Slot that dominated by UseSlot, skip it. Otherwise we will
    // get a cycle and setting the unecessary alive slots.
    //if (BasicBlock *BB = ChildNode->getParent()) {
    //  if (DT->dominates(UseSlot->getParent(), BB))
    //    continue;
    //}

    // Update the live slots.
    VI->Alives.set(ChildNode->SlotNum);
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
  V->initializeDefSlot(SlotNum);

  // Remember the written slots.
  WrittenSlots[Def].set(SlotNum);

  verifyAnalysis();
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
      // The the definitions will never live-in Finish Slot.
      if (Succ != VM->getFinishSlot()) Succs.set(Succ->SlotNum);
    }
  }

  for (var_iterator I = VarList.begin(), E = VarList.end(); I != E; ++I) {
    VarInfo *VI = I;

    if (VI->LiveIns.empty()) {
      assert(!VI->hasMultiDef() && "Unexpected multi define VI!");
      // Otherwise compute the live-ins now.
      SparseBitVector<> LiveDef(VI->Defs);
      LiveDef.intersectWithComplement(VI->Kills);

      // If the live-ins are empty, the latching operation is unpredicated.
      // This mean the definition can reach all successors of the defining slot.
      // But we need to trim the dead live-ins according to its alive slots.
      typedef SparseBitVector<>::iterator iterator;
      for (iterator I = LiveDef.begin(), E = LiveDef.end(); I != E; ++I) {
        unsigned LiveDefSlot = *I;
        VI->LiveIns |= STGBitMap[LiveDefSlot];
      }

      assert((!VI->LiveIns.empty() || VI->Alives.empty()) && "There is no livein?");
    }

    SparseBitVector<> ReachableSlots(VI->Alives | VI->Kills | VI->DefKills);
    assert((ReachableSlots.intersects(VI->LiveIns) || VI->Defs == VI->Kills)
           && "Bad liveins!");

    // Trim the dead live-ins.
    VI->LiveIns &= ReachableSlots;
    DEBUG(VI->dump());
  }
}

unsigned SeqLiveVariables::getIntervalFromDef(VASTSeqValue *V, VASTSlot *ReadSlot,
                                              STGShortestPath *SSP) const {
  const VarInfo *VI = 0;
  unsigned ReadSlotNum = ReadSlot->SlotNum;

  typedef VASTSeqValue::const_iterator iterator;
  for (iterator DI = V->begin(), DE = V->end(); DI != DE; ++DI) {
    std::map<VarName, VarInfo*>::const_iterator at = VarInfos.find(*DI);
    if (at == VarInfos.end()) continue;

    const VarInfo *CurVI = at->second;

    if (!CurVI->isSlotReachable(ReadSlotNum)) continue;

    // No need to check multiple VI reachable to this slot. We will check it
    // in verifyAnalysis.
    VI = CurVI;
    break;
  }

  // The SeqVal is kill before readslot.
  if (VI == 0) return 0;

  // Calculate the Shortest path distance from all live-in slot.
  unsigned IntervalFromLiveIn = STGShortestPath::Inf;
  typedef SparseBitVector<>::iterator livein_iterator;
  for (livein_iterator I = VI->LiveIns.begin(), E = VI->LiveIns.end();
       I != E; ++I) {
    unsigned LiveInSlotNum = *I;
    unsigned CurInterval = SSP->getShortestPath(LiveInSlotNum, ReadSlotNum);
    if (CurInterval >= STGShortestPath::Inf) {
      dbgs() << "Read at slot: " << ReadSlotNum << '\n';
      dbgs() << "Livein slot: " << LiveInSlotNum << '\n';
      VI->dump();
      dbgs() <<  "Alive slot not reachable?\n";
      continue;
    }

    IntervalFromLiveIn = std::min(IntervalFromLiveIn, CurInterval);
  }

  assert(IntervalFromLiveIn < STGShortestPath::Inf && "No live-in?");

  // The is 1 extra cycle from the definition to live in.
  return IntervalFromLiveIn + 1;
}
