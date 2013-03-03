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
  SVSet Srcs;

  typedef VASTSeqValue::iterator vn_itertor;
  for (vn_itertor I = SV->begin(), E = SV->end(); I != E; ++I) {
    Srcs.clear();

    VASTLatch &DstLatch = *I;
    VASTSlot *ReadSlot = DstLatch.getSlot();

    VASTValPtr FI = DstLatch;
    FI->extractSupporingSeqVal(Srcs);

    // In this case, we can retime as we want.
    if (Srcs.empty()) continue;

    unsigned CriticalDelay = getCriticalDelay(Srcs, FI.get());
    unsigned AvailableInterval = getAvailableInterval(Srcs, ReadSlot);

    if (CriticalDelay >= AvailableInterval) continue;

    dbgs() << "Fanin Pipelining opportnity: Slack: "
           << (AvailableInterval - CriticalDelay) << '\n';

    DstLatch.Op->dump();
  }

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
