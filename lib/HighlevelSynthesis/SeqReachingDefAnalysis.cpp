//===- RtlSSAAnalysis.cpp - Analyse the dependency between registers - C++ --=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass collect the slots information of register and map them into a map
// vector. then it will analyse dependency between registers.
//
//
//===----------------------------------------------------------------------===//

#include "SeqLiveVariables.h"
#include "SeqReachingDefAnalysis.h"
#include "shang/VASTModulePass.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/Support/SourceMgr.h"
#define DEBUG_TYPE "vtm-rtl-ssa"
#include "llvm/Support/Debug.h"

#include <map>

using namespace llvm;

void SlotInfo::dump() const {
  print(dbgs());
}

void SlotInfo::insertOvewritten(VASTSeqValue *V) {  
  OverWrittenValue.insert(V);
}

void SlotInfo::print(raw_ostream &OS) const {
  OS << S->getName() << "\nGen:\n";
  for (gen_iterator I = gen_begin(), E = gen_end(); I != E; ++I) {
    const VASTSeqValue *D = (*I).Dst;
    OS.indent(2) << D->getName() << "\n";
  }

  OS << "\n\nIn:\n";
  for (VASCycMapTy::const_iterator I = in_begin(), E = in_end(); I != E; ++I) {
    const VASTSeqValue *D = I->first.Dst;
    OS.indent(2) << D->getName() << '[' << I->second.getCycles() << "]\n";
  }

  OS << "\n\n";
}

// Any VAS whose value is overwritten at this slot is killed.
bool SlotInfo::isVASKilled(VNInfo VN) const {
  return OverWrittenValue.count(VN.Dst);
}

void SlotInfo::initOutSet() {
  // Build the initial out set ignoring the kill set.
  for (gen_iterator I = gen_begin(), E = gen_end(); I != E; ++I)
    SlotOut.insert(std::make_pair(*I, SlotInfo::LiveInInfo()));
}

SeqReachingDefAnalysis::SeqReachingDefAnalysis() : VASTModulePass(ID), VM(0) {
  initializeSeqReachingDefAnalysisPass(*PassRegistry::getPassRegistry());
}


void SeqReachingDefAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequired<SeqLiveVariables>();
  AU.setPreservesAll();
}

bool SeqReachingDefAnalysis::runOnVASTModule(VASTModule &M) {
  VM = &M;

  // Push back all the slot into the SlotVec for the purpose of view graph.
  typedef VASTModule::slot_iterator slot_it;
  for (slot_it I = VM->slot_begin(), E = VM->slot_end(); I != E; ++I) {
    VASTSlot *S = I;

    SlotVec.push_back(S);
    // Create a new SlotInfo if it is not defined before.
    SlotInfo *SI = new (Allocator) SlotInfo(S);
    bool inserted = SlotInfos.insert(std::make_pair(S, SI)).second;
    assert(inserted && "SlotInfo inserted?");
    (void) inserted;
  }

  // Define the VAS.
  ComputeReachingDefinition();

  DEBUG(VM->viewGraph());

  return false;
}

SlotInfo *SeqReachingDefAnalysis::getSlotInfo(const VASTSlot *S) const {
  slotinfo_it It = SlotInfos.find(S);
  assert(It != SlotInfos.end() && "SlotInfo not exist!");
  return It->second;
}

bool SeqReachingDefAnalysis::addLiveIns(SlotInfo *From, SlotInfo *To,
                                        bool FromAliasSlot) {
  bool Changed = false;
  typedef SlotInfo::vascyc_iterator it;
  // Store the slot numbers in signed integer, we will perform subtraction on
  // them and may produce negative result.
  int FromSlotNum = From->getSlot()->SlotNum,
      ToSlotNum = To->getSlot()->SlotNum;
  BasicBlock *ToBB = To->getSlot()->getParentBB();
  BasicBlock *FromBB = From->getSlot()->getParentBB();
  bool FromLaterAliasSlot = FromAliasSlot && FromSlotNum > ToSlotNum;

  for (it I = From->out_begin(), E = From->out_end(); I != E; ++I) {
    const SlotInfo::VNInfo &PredOut = I->first;
    VASTSeqValue *V = PredOut.Dst;

    VASTSlot *DefSlot = PredOut.Op->getSlot();
    SlotInfo::LiveInInfo LI = I->second;

    bool IsDefSlot = LI.getCycles() == 0;
    // Increase the cycles by 1 after the value lives to next slot.
    LI.incCycles();

    bool LiveInToSlot = !FromLaterAliasSlot || V->isTimingUndef();
    bool LiveOutToSlot = !To->isVASKilled(PredOut);

    // Check if the register is killed according MachineFunction level liveness
    // information.
    switch (V->getValType()) {
    case VASTSeqValue::Data: {
      unsigned RegNum = V->getDataRegNum();
      // The registers are not propagate from the slot to its alias slot.
      if (RegNum && FromAliasSlot) LiveInToSlot = false;

      const Instruction *Inst = PredOut.Op->getInst();
      // Do not add live out if data register R not live in the new BB.
      if (Inst) {
        if (isa<PHINode>(Inst) && IsDefSlot) {
          // The register is defined by the VOpMvPhi at FromSlot.
          // However, the copy maybe disabled when jumping to ToBB.
          // Then this define is even not live-in ToSlot, we can simply skip the
          // rest of the code.
          //if (DefMI->getOperand(2).getMBB() != ToBB)
          //  continue;
        }

        // If the FromSlot is bigger than the ToSlot, then we are looping back.
        bool ToNewBB = (FromBB != ToBB || FromSlotNum >= ToSlotNum) && ToBB;

        // The register may be defined at the first slot of current BB, which
        // slot is alias with the last slot of Current BB's predecessor. In this
        // case we should not believe the live-ins information of the BB.
        // And if the register are not defined at the current slot (the last
        // slot of current BB) then we can trust the live-ins information.
        //bool ShouldTrustBBLI = ToNewBB
        //                       && (!IsDefSlot || DefMI->getParent() == FromBB);
      }
      break;
    }
    case VASTSeqValue::Slot:
      // Ignore the assignment that reset the slot enable register, even the
      // signal may take more than one cycles to propagation, the shortest path
      // should be the path that propagating the "1" value of the enable
      // register.
      if (V->getSlotNum() == DefSlot->SlotNum && LI.getCycles() > 1)
        LiveOutToSlot = false;
      break;
    default: break;
    }

    if (!LiveInToSlot) continue;

    Changed |= To->insertIn(PredOut, LI);

    if (!LiveOutToSlot) continue;

    Changed |= To->insertOut(PredOut, LI);
  }

  return Changed;
}

void SeqReachingDefAnalysis::ComputeReachingDefinition() {
  ComputeGenAndKill();
  // TODO: Simplify the data-flow, some slot may neither define new VAS nor
  // kill any VAS.

  bool Changed = false;

  do {
    Changed = false;

    for (slot_vec_it I = SlotVec.begin(), E = SlotVec.end(); I != E; ++I) {
      VASTSlot *S =*I;
      assert(S && "Unexpected null slot!");

      SlotInfo *CurSI = getSlotInfo(S);

      // Compute the out set.
      typedef VASTSlot::pred_iterator iterator;
      for (iterator PI = S->pred_begin(), PE = S->pred_end(); PI != PE; ++PI) {
        VASTSlot *PredSlot = *PI;

        SlotInfo *PredSI = getSlotInfo(PredSlot);

        Changed |= addLiveIns(PredSI, CurSI, false);
      }
    }
  } while (Changed);
}

void SeqReachingDefAnalysis::ComputeGenAndKill(const VASTSeqDef &D) {
  VASTSlot *S = D->getSlot();
  SlotInfo *SI = getSlotInfo(S);
  SI->insertGen(D);
}

void SeqReachingDefAnalysis::ComputeGenAndKill() {
  // Collect the generated statements to the SlotGenMap.
  typedef VASTModule::slot_iterator it;
  for (it SI = VM->slot_begin(), SE = VM->slot_end(); SI != SE; ++SI) {
    VASTSlot *S = SI;

    typedef VASTSlot::const_op_iterator def_iterator;
    for (def_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
      VASTSeqOp *Op = *I;
      for (unsigned i = 0, e = Op->getNumDefs(); i != e; ++i)
        ComputeGenAndKill(Op->getDef(i));      
    }
  }

  // Build the Out set from Gen set.
  for (slot_vec_it I = SlotVec.begin(), E = SlotVec.end(); I != E; ++I) {
    VASTSlot *S =*I;
    assert(S && "Unexpected null slot!");
    SlotInfo *SI = getSlotInfo(S);
    SI->initOutSet();
  }
}

char SeqReachingDefAnalysis::ID = 0;
INITIALIZE_PASS_BEGIN(SeqReachingDefAnalysis, "SeqReachingDefAnalysis",
                      "SeqReachingDefAnalysis", false, false)
  INITIALIZE_PASS_DEPENDENCY(SeqLiveVariables);
INITIALIZE_PASS_END(SeqReachingDefAnalysis, "SeqReachingDefAnalysis",
                    "SeqReachingDefAnalysis", false, false)

Pass *llvm::createSeqReachingDefAnalysisPass() {
  return new SeqReachingDefAnalysis();
}
