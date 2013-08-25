//===--- SelectorPipelining.cpp - Implement the MUX Pipelining --*- C++ -*-===//
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

#include "TimingNetlist.h"

#include "shang/FUInfo.h"
#include "shang/VASTMemoryPort.h"
#include "shang/VASTExprBuilder.h"

#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"
#include "shang/Passes.h"
#include "shang/STGDistances.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-selector-mux-synthesis"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumPipelineRegBits, "Number of Pipeline register created");

namespace {
struct MUXFI {
  VASTLatch &L;
  unsigned Slack;
  unsigned StateNum;

  MUXFI(VASTLatch &L, unsigned Slack) : L(L), Slack(Slack),
    StateNum(L.getSlot()->getParentState()->SlotNum) {}

  VASTSlot *getSlot() const { return L.getSlot(); }
  VASTValPtr getCnd() const { return L.getGuard(); }
  VASTValPtr getFI() const { return L; }
};

struct MUXPipeliner {
  static unsigned RegCounter;

  std::vector<MUXFI*> Fannins;

  typedef std::map<VASTSelector*, std::map<VASTSlot*, VASTSeqValue*> > ValueMap;
  ValueMap ValueCache;

  // Lookup or create the SeqValue for the selector.
  VASTSeqValue *getValueAt(VASTRegister *R, VASTSlot *S) {
    VASTSelector *Sel = R->getSelector();
    VASTSeqValue *&V = ValueCache[Sel][S];
    // TODO: Index the pipelined SeqVal!
    if (V == 0) V = VM->createSeqValue(Sel, 0);
    return V;
  }

  VASTSelector *Sel;
  unsigned MaxPerCyleFINum;
  VASTModule *VM;

  MUXPipeliner(VASTSelector *Sel, unsigned MaxPerCyleFINum, VASTModule *VM)
    : Sel(Sel), MaxPerCyleFINum(MaxPerCyleFINum), VM(VM) {}

  ~MUXPipeliner() {
    DeleteContainerPointers(Fannins);
  }

  void addFannin(VASTLatch &L, unsigned Slack) {
    assert(Slack && "Unexpected slack!");
    Fannins.push_back(new MUXFI(L, Slack));
  }

  bool pipelineGreedy();

  // Retime to read all FIs 1 cycles earlier
  template<typename iterator>
  void retimeLatchesOneCycleEarlier(iterator I, iterator E);
  void retimeLatchesOneCycleEarlier(ArrayRef<MUXFI*> FIs);
};

struct SelectorPipelining : public VASTModulePass {
  TimingNetlist *TNL;
  STGDistances *STGDist;
  unsigned MaxSingleCyleFINum;
  VASTExprBuilder *Builder;
  VASTModule *VM;
  // Number of cycles at a specificed slot that we can move back unconditionally.
  std::map<unsigned, unsigned> SlotSlack;

  static char ID;

  SelectorPipelining() : VASTModulePass(ID), TNL(0), STGDist(0) {
    initializeSelectorPipeliningPass(*PassRegistry::getPassRegistry());

    VFUMux *Mux = getFUDesc<VFUMux>();
    MaxSingleCyleFINum = 8;
  }

  bool pipelineFanins(VASTSelector *Sel);
  // Decompose a SeqInst latching more than one SeqVal to several SeqInsts
  // where each of them only latching one SeqVal.
  void descomposeSeqInst(VASTSeqInst *SeqInst);

  typedef std::set<VASTSeqValue*> SVSet;

  unsigned getCriticalDelay(const SVSet &S, VASTValue *V);
  unsigned getAvailableInterval(const SVSet &S, VASTSlot *ReadSlot);

  unsigned getSlotSlack(VASTSlot *S);
  void buildPipelineFIs(VASTSelector *Sel, MUXPipeliner &Pipeliner);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
    AU.addRequired<TimingNetlist>();
    AU.addRequired<STGDistances>();
    AU.addPreserved<STGDistances>();
  }

  bool runOnVASTModule(VASTModule &VM);

  void releaseMemory() {
    SlotSlack.clear();
  }
};
}

INITIALIZE_PASS_BEGIN(SelectorPipelining, "sequential-selector-pipelining",
                      "Implement the MUX for the Sequantal Logic", false, true)
  INITIALIZE_PASS_DEPENDENCY(SeqLiveVariables)
  INITIALIZE_PASS_DEPENDENCY(STGDistances)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
INITIALIZE_PASS_END(SelectorPipelining, "sequential-selector-pipelining",
                    "Implement the MUX for the Sequantal Logic", false, true)

char SelectorPipelining::ID = 0;

Pass *llvm::createSelectorPipeliningPass() {
  return new SelectorPipelining();
}

bool SelectorPipelining::runOnVASTModule(VASTModule &VM) {
  MinimalExprBuilderContext Context(VM);
  Builder = new VASTExprBuilder(Context);
  this->VM = &VM;

  typedef VASTModule::selector_iterator iterator;

  TNL = &getAnalysis<TimingNetlist>();
  STGDist = &getAnalysis<STGDistances>();

  // Clear up all MUX before we perform selector pipelining.
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
    I->dropMux();

  std::vector<VASTSeqInst*> Worklist;

  typedef VASTModule::seqop_iterator seqop_iterator;
  for (seqop_iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I)
    if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(I))
      if (SeqInst->num_srcs() > 1)
        Worklist.push_back(SeqInst);

  while (!Worklist.empty()) {
    VASTSeqInst *SeqInst = Worklist.back();
    Worklist.pop_back();

    descomposeSeqInst(SeqInst);
  }

  DEBUG(dbgs() << "Before MUX pipelining:\n"; VM.dump(););

  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    // FIXME: Get the MUX delay from the timing estimator.

    // The slot assignments cannot be retime, the selectors with small fannin
    // number do not need to be retime.
    if (I->isSlot() || I->size() < MaxSingleCyleFINum) continue;

    pipelineFanins(I);
  }

  DEBUG(dbgs() << "After MUX pipelining:\n"; VM.dump(););

  delete Builder;
  return true;
}

void SelectorPipelining::descomposeSeqInst(VASTSeqInst *SeqInst) {
  for (unsigned i = 0, e = SeqInst->num_srcs(); i != e; ++i) {
    VASTLatch L = SeqInst->getSrc(i);

    VASTSeqInst *NewSeqInst = VM->lauchInst(L.getSlot(), L.getGuard(), 1,
                                            SeqInst->getValue(), true);
    NewSeqInst->addSrc(VASTValPtr(L), 0, L.getSelector(), L.getDst());
  }

  VM->eraseSeqOp(SeqInst);
}

//static bool isLatchIdentical(const VASTLatch &LHS, const VASTLatch &RHS) {
//  if (LHS.getDst() != RHS.getDst()) return false;
//
//  if (LHS.getSlot() != RHS.getSlot()) return false;
//
//  if (LHS.getPred() != RHS.getPred()) return false;
//
//  return true;
//}

bool SelectorPipelining::pipelineFanins(VASTSelector *Sel) {
  // Iterate over all fanins to build the Fanin Slack Map.
  // Try to build the pipeline register by inserting the map.
  MUXPipeliner P(Sel, MaxSingleCyleFINum, VM);
  buildPipelineFIs(Sel, P);

  return P.pipelineGreedy();
}

unsigned SelectorPipelining::getSlotSlack(VASTSlot *S) {
  S = S->getParentState();

  unsigned CurSlotNum = S->SlotNum;
  // If we had calculated the slack?
  std::map<unsigned, unsigned>::iterator at = SlotSlack.find(CurSlotNum);

  if (at != SlotSlack.end()) return at->second;

  // Otherwise we need to calculate it now.
  unsigned Slack = 0;
  while (S->pred_size() == 1) {
    VASTSlot *PredSlot = *S->pred_begin();

    if (PredSlot->IsSubGrp) break;

    S = PredSlot;
    ++Slack;
  }

  return (SlotSlack[CurSlotNum] = Slack);
}

void
SelectorPipelining::buildPipelineFIs(VASTSelector *Sel, MUXPipeliner &Pipeliner) {
  SVSet Srcs;

  typedef VASTSelector::iterator vn_itertor;
  for (vn_itertor I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch &DstLatch = *I;

    // Ignore the trivial fannins.
    if (Sel->isTrivialFannin(DstLatch)) continue;

    // Do not mess up with the operations that is guarded by the strange control
    // signals.
    if (!DstLatch.Op->guardedBySlotActive()) continue;

    VASTSlot *ReadSlot = DstLatch.getSlot();

    unsigned RetimeSlack = getSlotSlack(ReadSlot);
    if (RetimeSlack == 0) continue;

    unsigned CriticalDelay = 0;
    unsigned AvailableInterval = STGDistances::Inf;

    // Also do not retime across the SVal without liveness information.
    VASTValPtr FI = DstLatch;

    if (FI->extractSupportingSeqVal(Srcs)) {
      CriticalDelay = std::max(CriticalDelay, getCriticalDelay(Srcs, FI.get()));
      AvailableInterval
        = std::min(AvailableInterval, getAvailableInterval(Srcs, ReadSlot));
      Srcs.clear();
    }

    VASTValPtr Pred = DstLatch.getGuard();
    // We should also retime the predicate together with the fanin.
    if (Pred->extractSupportingSeqVal(Srcs)) {
      CriticalDelay = std::max(CriticalDelay, getCriticalDelay(Srcs, Pred.get()));
      AvailableInterval
        = std::min(AvailableInterval, getAvailableInterval(Srcs, ReadSlot));
      Srcs.clear();
    }

    // Make sure the Retime slack is not negative
    if (AvailableInterval < CriticalDelay) continue;

    DEBUG(dbgs() << "Fanin Pipelining opportnity: Slack: "
            << (AvailableInterval - CriticalDelay)
            << " RetimeSlack: " << RetimeSlack << '\n');

    // Adjust the retime slack according to the timing slack.
    unsigned FISlack = std::min(RetimeSlack, AvailableInterval - CriticalDelay);
    if (FISlack) Pipeliner.addFannin(DstLatch, FISlack);
  }
}

unsigned SelectorPipelining::getCriticalDelay(const SVSet &S, VASTValue *V) {
  unsigned Delay = 0;
  typedef SVSet::const_iterator iterator;
  for (iterator I = S.begin(), E = S.end(); I != E; ++I) {
    VASTSeqValue *Src = *I;

    // Do not retime across the direct output of the functional unit.
    if (Src->isFUOutput()) return STGDistances::Inf;

    // The ignore the trivial path.
    if (Src == V) continue;    

    Delay = std::max<unsigned>(Delay, ceil(TNL->getDelay(Src, V)));
  }

  return Delay;
}

unsigned
SelectorPipelining::getAvailableInterval(const SVSet &S, VASTSlot *ReadSlot) {
  unsigned Interval = STGDistances::Inf;
  typedef SVSet::const_iterator iterator;
  for (iterator I = S.begin(), E = S.end(); I != E; ++I) {
    VASTSeqValue *SV = *I;
    // Do not retime if we do not have any timing information.
    if (SV == 0) return 0;

    // Do not retime across the static register and the output of functional
    // units, we do not have the accurate timing information for them.
    if (SV->isStatic() || SV->isFUOutput()) return 0;

    Interval = std::min(Interval, STGDist->getIntervalFromDef(SV, ReadSlot));
  }

  assert(Interval && "Unexpected interval!");
  // Dirty HACK: Avoid retime to the assignment slot of the FI for now.
  Interval -= 1;

  return Interval;
}

static VASTSlot *getSlotAtLevel(VASTSlot *S, unsigned Level) {
  S = S->getParentState();

  assert(Level && "Bad level!");
  // Otherwise we need to calculate it now.
  while (S->pred_size() == 1 && Level) {
    VASTSlot *PredSlot = *S->pred_begin();
    assert(!PredSlot->IsSubGrp && "Unexpected conditional edge!");

    S = PredSlot;
    --Level;
  }

  assert(Level == 0 && "Cannot get slot specificed slot!");

  return S;
}

unsigned MUXPipeliner::RegCounter = 0;

template<typename iterator>
void MUXPipeliner::retimeLatchesOneCycleEarlier(iterator I, iterator E) {
  VASTRegister *EnSel
    = VM->createRegister("enable_" + utostr_32(RegCounter) + "r", 1, 0,
                         VASTSelector::Enable);
  ++NumPipelineRegBits;

  VASTRegister *FISel = 0;
  if (!Sel->isEnable()) {
    FISel = VM->createRegister("fannin_" + utostr_32(RegCounter) + "r",
                               Sel->getBitWidth(), 0, VASTSelector::Temp);
    NumPipelineRegBits += Sel->getBitWidth();
  }

  ++RegCounter;

  for ( ; I != E; ++I) {
    MUXFI *FI = *I;

    DEBUG(dbgs() << "Going to retime the input of:\n\t";
    FI->L.Op->dump());

    // Copy the Fannin value and condition to local variables, we will perform
    // replacement on FI later.
    VASTValPtr FIVal = FI->getFI(), FICnd = FI->getCnd();
    VASTSlot *S = getSlotAtLevel(FI->getSlot(), 1);

    DEBUG(dbgs() << "Retime the assignment at Slot#" << FI->getSlot()->SlotNum
           << " to " << S->SlotNum << " for " << Sel->getName() << '\n');

    // Pipeline the guarding condition.
    VASTSeqValue *PipelinedEn = getValueAt(EnSel, S);
    VM->assignCtrlLogic(PipelinedEn, VASTImmediate::True, S, FICnd, true);
    // Read the pipelined guarding condition instead.
    FI->L.replacePredBy(PipelinedEn, false);

    // Pipeline the fannin value.
    if (FISel) {
      VASTSeqValue *PipelinedFI = getValueAt(FISel, S);
      VM->assignCtrlLogic(PipelinedFI, FIVal, S, FICnd, true);
      FI->L.replaceUsedBy(PipelinedFI);
    }
  }

  assert(!EnSel->getSelector()->empty()
         && (!FISel
             || FISel->getSelector()->size() == EnSel->getSelector()->size())
         && "Bad Pipelining!");
}

void MUXPipeliner::retimeLatchesOneCycleEarlier(ArrayRef<MUXFI*> FIs) {
  retimeLatchesOneCycleEarlier(FIs.begin(), FIs.end());
}

static int
sort_by_slot(const void *LHS, const void *RHS) {
  typedef const MUXFI T;

  if (reinterpret_cast<T*>(LHS)->Slack < reinterpret_cast<T*>(RHS)->Slack)
    return -1;

  if (reinterpret_cast<T*>(LHS)->Slack > reinterpret_cast<T*>(RHS)->Slack)
    return 1;

  return 0;
}

bool MUXPipeliner::pipelineGreedy() {
  if (Fannins.empty()) return false;
  ArrayRef<MUXFI*> FanninsRef(Fannins);

  unsigned UsedFINum = Sel->size() - Fannins.size();
  int AvailableFINum = std::max(int(MaxPerCyleFINum - UsedFINum), 1);
  DEBUG(dbgs() << Sel->getName()
         << " #Not pipelinable FIs: " << UsedFINum
         << " #Pipelinable FIs: " << Fannins.size()
         << " #FI left: " << AvailableFINum << '\n');

  // Handle the trivial case trivially.
  if (AvailableFINum == 1) {
    retimeLatchesOneCycleEarlier(FanninsRef);
    return true;
  }

  // Iterate over the fannins, divide them into MaxSingleCyleFINum groups.
  // TODO: Put the fanins in the successive slots together.
  array_pod_sort(Fannins.begin(), Fannins.end(), sort_by_slot);

  unsigned NextLevelFINum = 1 + ((Fannins.size() - 1) / AvailableFINum);

  DEBUG(dbgs().indent(2) << " #Nextlevel FIs: " << NextLevelFINum << '\n');

  for (unsigned i = 0, e = FanninsRef.size(); i < e; i += NextLevelFINum) {
    unsigned n = std::min(NextLevelFINum, e - i);
    retimeLatchesOneCycleEarlier(FanninsRef.slice(i, n));
  }

  return true;
}
