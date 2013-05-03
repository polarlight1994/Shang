//===- CFGFoldingAnalysis - Predict the Impact of CFGFolding ------ C++ ---===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the CFGFlodingAnalysis, which try to predict the impcat
// of CFG folding, e.g. generate big MUXs for the operation that get folded.
//
//===----------------------------------------------------------------------===//

#include "SchedulerBase.h"
#include "TimingNetlist.h"

#include "shang/VASTSeqValue.h"
#include "shang/VASTSeqOp.h"
#include "shang/VASTSlot.h"

#include "llvm/IR/Function.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "cfg-folding-analysis"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumAntiFoldingConstraints,
          "Number of Constraints added to prevent the operation being duplicate.");

static cl::opt<unsigned>
MuxDelayIncThreshold("vast-cfg-folding-mux-delay-inc",
                    cl::desc("The Allowed MUX increment by CFG folding"),
                    cl::init(0));

static cl::opt<unsigned>
MaxAnalysisDepth("vast-cfg-folding-max-analysis-depth",
                 cl::desc("The Analysis depth for CFG folding"),
                 cl::init(8));
namespace {
struct CFGFoldingAnalysis {
  SchedulerBase &G;
  TimingNetlist &TNL;

  // Remember the number of predecessors and number of successors after CFG
  // Folding.
  std::map<BasicBlock*, unsigned> FoldingSize;

  explicit CFGFoldingAnalysis(SchedulerBase &G, TimingNetlist &TNL)
    : G(G), TNL(TNL) {}

  void computeFoldingSize(BasicBlock *BB, BasicBlock *FoldTo, unsigned Depth);

  void computeFoldingSize();

  // Add constraints to the scheduling graph to prevent the SeqOp from being
  // duplicated by aggressive CFG folding.
  void addConstraints();
  void addConstraints(BasicBlock *BB);
  void handleCommonSU(unsigned FoldingSize, VASTSchedUnit *SU,
                      VASTSchedUnit *Entry);
  void handleSlotCtrl(unsigned FoldingSize, VASTSchedUnit *SU,
                      VASTSchedUnit *Entry);

  void dump() const;
};
}

//===----------------------------------------------------------------------===//
void CFGFoldingAnalysis::computeFoldingSize(BasicBlock *BB, BasicBlock *FoldTo,
                                            unsigned Depth) {
  if (Depth >= MaxAnalysisDepth) return;
  
  MutableArrayRef<VASTSchedUnit*> SUs(G->getSUInBB(BB));
  VASTSchedUnit *Entry = SUs.front();
  assert(Entry->isBBEntry() && "Bad SU order!");
  unsigned EntryASAP = G.getASAPStep(Entry);

  if (BB != FoldTo) ++FoldingSize[BB];

  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *SU = SUs[i];

    if (!SU->isTerminator()) continue;

    BasicBlock *TargetBB = SU->getTargetBlock();
    if (TargetBB == 0) continue;

    if (G.getASAPStep(SU) != EntryASAP && BB != FoldTo) continue;

    // Do not fold the loop body more than once.
    if (TargetBB == BB && FoldTo == BB) continue;

    computeFoldingSize(TargetBB, FoldTo, Depth + 1);
  }  
}

void CFGFoldingAnalysis::computeFoldingSize() {
  Function &F = G->getFunction();

  // FIXME: Compute the size and add the constraints on the BB with the highest
  // frequency first.
  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;

    computeFoldingSize(BB, BB, 0);
  }
}

void CFGFoldingAnalysis::addConstraints() {
  Function &F = G->getFunction();

  // FIXME: Compute the size and add the constraints on the BB with the highest
  // frequency first.
  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    //BasicBlock *BB = I;

    //addConstraints(BB);
  }
}

void CFGFoldingAnalysis::handleCommonSU(unsigned FoldingSize, VASTSchedUnit *SU,
                                        VASTSchedUnit *Entry) {
  VASTSeqOp *Op = SU->getSeqOp();
  
  if (Op == 0) return;

  unsigned FoldedMuxDelay = 0, OriginMuxDelay = 0;

  for (unsigned i = 0, e = Op->getNumSrcs(); i != e; ++i) {
    VASTLatch L = Op->getSrc(i);
    VASTSeqValue *Dst = L.getDst();

    // Calculate the MuxSize after the current Fanin are duplicated.
    unsigned FoldedMuxSize = Dst->size() + FoldingSize - 1;
    unsigned CurFoldedDelay
      = TNL.getMuxDelay(FoldedMuxSize, Dst).getNumCycles();
    FoldedMuxDelay = std::max(FoldedMuxDelay, CurFoldedDelay);
    unsigned CurMuxDelay
      = TNL.getMuxDelay(Dst->size(), Dst).getNumCycles();
    OriginMuxDelay = std::max(OriginMuxDelay, CurMuxDelay);
  }

  DEBUG(dbgs() << "SU: ";
  SU->dump();
  dbgs() << " MuxDelay after folding: " << FoldedMuxDelay << '\n');

  // Add (soft?) constraints to prevent the SU from being scheduled to the
  // first slot and duplicated by CFG folding.
  if (FoldedMuxDelay - OriginMuxDelay > MuxDelayIncThreshold) {
    SU->addDep(Entry, VASTDep::CreateCtrlDep(1));
    ++NumAntiFoldingConstraints;
  }
}

void CFGFoldingAnalysis::handleSlotCtrl(unsigned FoldingSize, VASTSchedUnit *SU,
                                        VASTSchedUnit *Entry) {
  VASTSlotCtrl *SlotCtrl = cast<VASTSlotCtrl>(SU->getSeqOp());
  unsigned OriginMuxSize = SlotCtrl->getTargetSlot()->pred_size();
  unsigned FoldedMuxSize = OriginMuxSize + FoldingSize - 1;
  unsigned FoldedMuxDelay = TNL.getMuxDelay(FoldedMuxSize, 0).getNumCycles();
  unsigned OriginMuxDelay = TNL.getMuxDelay(OriginMuxSize, 0).getNumCycles();

  DEBUG(dbgs() << "SU: ";
  SU->dump();
  dbgs() << " MuxDelay after folding: " << FoldedMuxDelay << '\n');

  // Add (soft?) constraints to prevent the SU from being scheduled to the
  // first slot and duplicated by CFG folding.
  if (FoldedMuxDelay - OriginMuxDelay > MuxDelayIncThreshold) {
    SU->addDep(Entry, VASTDep::CreateCtrlDep(1));
    ++NumAntiFoldingConstraints;
  }
}

void CFGFoldingAnalysis::addConstraints(BasicBlock *BB) {
  MutableArrayRef<VASTSchedUnit*> SUs(G->getSUInBB(BB));
  VASTSchedUnit *Entry = SUs.front();
  assert(Entry->isBBEntry() && "Bad SU order!");
  unsigned EntryASAP = G.getASAPStep(Entry);
  unsigned Size = FoldingSize[BB];

  DEBUG(dbgs() << "Run on BB: " << BB->getName() << " Size: " << Size << "\n");

  for (unsigned i = 1; i < SUs.size(); ++i) {
    VASTSchedUnit *SU = SUs[i];

    if (SU->isTerminator() || SU->isPHI()) continue;

    if (G.getASAPStep(SU) != EntryASAP) continue;

    if (SU->isTerminator())
      handleSlotCtrl(Size, SU, Entry);
    else
      handleCommonSU(Size, SU, Entry);
  }
}

void CFGFoldingAnalysis::dump() const {
  typedef std::map<BasicBlock*, unsigned>::const_iterator iterator;
  for (iterator I = FoldingSize.begin(), E = FoldingSize.end(); I != E; ++I)
    dbgs() << "BB: " << I->first->getName() << " Size: " << I->second << '\n';
}

//===----------------------------------------------------------------------===//
void SchedulerBase::addCFGFoldingConstraints(TimingNetlist &TNL) {
  buildTimeFrameAndResetSchedule(true);

  CFGFoldingAnalysis CFA(*this, TNL);

  CFA.computeFoldingSize();

  CFA.addConstraints();
}
