//===-- SeqSelectorSynthesis.cpp - Implement Fanin Mux for Regs -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the control logic synthesis pass.
//
//===----------------------------------------------------------------------===//

#include "STGShortestPath.h"
#include "TimingNetlist.h"
#include "SeqLiveVariables.h"

#include "shang/VASTExprBuilder.h"

#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"
#include "shang/Passes.h"

#include "llvm/IR/DataLayout.h"
#define DEBUG_TYPE "shang-control-logic-synthesis"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct SeqSelectorSynthesis : public VASTModulePass {
  TimingNetlist *TNL;
  SeqLiveVariables *SLV;
  STGShortestPath *SSP;

  // Number of cycles at a specificed slot that we can move back unconditionally.
  std::map<unsigned, unsigned> SlotSlack;

  static char ID;

  SeqSelectorSynthesis() : VASTModulePass(ID), TNL(0), SLV(0), SSP(0) {
    initializeSeqSelectorSynthesisPass(*PassRegistry::getPassRegistry());
  }

  bool pipelineFanins(VASTSeqValue *SV, VASTExprBuilder &Builder, VASTModule &VM);

  typedef std::set<VASTSeqValue*> SVSet;

  unsigned getCriticalDelay(const SVSet &S, VASTValue *V);
  unsigned getAvailableInterval(const SVSet &S, VASTSlot *ReadSlot);
  unsigned getSlotSlack(VASTSlot *S);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<TimingNetlist>();
    AU.addRequired<SeqLiveVariables>();
    AU.addRequired<STGShortestPath>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
    AU.addPreserved<SeqLiveVariables>();
    AU.addPreserved<STGShortestPath>();
  }

  bool runOnVASTModule(VASTModule &VM);

  void releaseMemory() {
    SlotSlack.clear();
  }
};
}

INITIALIZE_PASS_BEGIN(SeqSelectorSynthesis, "sequential-selector-synthesis",
                      "Implement the MUX for the Sequantal Logic", false, true)
  INITIALIZE_PASS_DEPENDENCY(SeqLiveVariables)
  INITIALIZE_PASS_DEPENDENCY(STGShortestPath)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
INITIALIZE_PASS_END(SeqSelectorSynthesis, "sequential-selector-synthesis",
                      "Implement the MUX for the Sequantal Logic", false, true)

char SeqSelectorSynthesis::ID = 0;

char &llvm::SeqSelectorSynthesisID = SeqSelectorSynthesis::ID;

bool SeqSelectorSynthesis::runOnVASTModule(VASTModule &VM) {
  MinimalExprBuilderContext Context(VM);
  VASTExprBuilder Builder(Context);
  SLV = &getAnalysis<SeqLiveVariables>();
  TNL = &getAnalysis<TimingNetlist>();
  SSP = &getAnalysis<STGShortestPath>();

  // Building the Slot active signals.
  typedef VASTModule::seqval_iterator iterator;

  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I)
    pipelineFanins(I, Builder, VM);

  return true;
}

bool SeqSelectorSynthesis::pipelineFanins(VASTSeqValue *SV,
                                          VASTExprBuilder &Builder,
                                          VASTModule &VM) {
  // Iterate over all fanins to build the Fanin Slack Map.
  SVSet Srcs;

  typedef VASTSeqValue::iterator vn_itertor;
  for (vn_itertor I = SV->begin(), E = SV->end(); I != E; ++I) {
    Srcs.clear();

    VASTLatch &DstLatch = *I;
    VASTSlot *ReadSlot = DstLatch.getSlot();

    unsigned RetimeSlack = getSlotSlack(ReadSlot);

    VASTValPtr FI = DstLatch;
    FI->extractSupporingSeqVal(Srcs);

    // In this case, we can retime as we want.
    if (Srcs.empty()) continue;

    unsigned CriticalDelay = getCriticalDelay(Srcs, FI.get());
    unsigned AvailableInterval = getAvailableInterval(Srcs, ReadSlot);

    if (CriticalDelay >= AvailableInterval) continue;

    DEBUG(dbgs() << "Fanin Pipelining opportnity: Slack: "
           << (AvailableInterval - CriticalDelay)
           << " RetimeSlack: " << RetimeSlack << '\n');

    // Adjust the retime slack according to the timing slack.
    RetimeSlack = std::min(RetimeSlack, AvailableInterval - CriticalDelay);
  }

  // Try to build the pipeline register by inserting the map.
  // For now, we can build the single cycle MUX, Later we can build multi-cycle
  // MUX to save the registers.
  // FIXME: We can also reuse the assignment.

  // First of all, assign the MUX levels.

  return false;
}

unsigned SeqSelectorSynthesis::getCriticalDelay(const SVSet &S, VASTValue *V) {
  unsigned Delay = 0;
  typedef SVSet::const_iterator iterator;
  for (iterator I = S.begin(), E = S.end(); I != E; ++I) {
    VASTSeqValue *Src = *I;
    // The register to register assignment can be directly retime.
    if (Src == V) continue;

    Delay = std::max(Delay, TNL->getDelay(Src, V).getNumCycles());
  }

  return Delay;
}

unsigned SeqSelectorSynthesis::getAvailableInterval(const SVSet &S,
                                                    VASTSlot *ReadSlot) {
  unsigned Interval = STGShortestPath::Inf;
  typedef SVSet::const_iterator iterator;
  for (iterator I = S.begin(), E = S.end(); I != E; ++I)
    Interval = std::min(Interval, SLV->getIntervalFromDef(*I, ReadSlot, SSP));

  return Interval;
}

unsigned SeqSelectorSynthesis::getSlotSlack(VASTSlot *S) {
  unsigned CurSlotNum = S->SlotNum;
  // If we had calculated the slack?
  std::map<unsigned, unsigned>::iterator at = SlotSlack.find(CurSlotNum);

  if (at != SlotSlack.end()) return at->second;

  // Otherwise we need to calculate it now.
  unsigned Slack = 0;
  while (S->pred_size() == 1) {
    VASTSlot *PredSlot = *S->pred_begin();
    // Find the assignment operation that enable the current slot, check if
    // the guarding condition is always true.
    VASTSeqValue *CurSlotReg = S->getValue();

    typedef VASTSlot::op_iterator op_iterator;
    
    bool IsConditional = true;
    bool AnyPred = false;
    // We need to read the S->op_end() at every iteration because it may be
    // changed by removeOp.
    for (op_iterator I = PredSlot->op_begin(), E = PredSlot->op_end(); I != E; ++I) {
      VASTSeqCtrlOp *CtrlOp = dyn_cast<VASTSeqCtrlOp>(*I);
      if (CtrlOp == 0 || CtrlOp->getNumDefs() == 0)  continue;

      VASTSeqValue *Dst = CtrlOp->getDef(0).getDst();
      if (Dst != CurSlotReg) continue;

      // Ok, we find the operation that assign the slot register.
      assert(VASTValPtr(CtrlOp->getDef(0)) == VASTImmediate::True
             && "Expect enabling the next slot!");

      IsConditional
        = VASTValPtr(CtrlOp->getPred()) != VASTValPtr(VASTImmediate::True);

      AnyPred = true;

      break;
    }

    assert(AnyPred && "Cannot find the enable operation?");
    (void) AnyPred;

    if (IsConditional) break;

    S = PredSlot;
    ++Slack;
  }

  return (SlotSlack[CurSlotNum] = Slack);
}
