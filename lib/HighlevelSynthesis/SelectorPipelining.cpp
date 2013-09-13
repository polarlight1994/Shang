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
#include "Dataflow.h"

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
  Dataflow *DF;
  STGDistances *STGDist;
  unsigned MaxSingleCyleFINum;
  VASTModule *VM;
  // Number of cycles at a specificed slot that we can move back unconditionally.
  std::map<unsigned, unsigned> SlotSlack;

  static char ID;

  SelectorPipelining() : VASTModulePass(ID), TNL(0), DF(0), STGDist(0) {
    initializeSelectorPipeliningPass(*PassRegistry::getPassRegistry());

    VFUMux *Mux = getFUDesc<VFUMux>();
    MaxSingleCyleFINum = 8;
  }

  bool pipelineFanins(VASTSelector *Sel);
  // Decompose a SeqInst latching more than one SeqVal to several SeqInsts
  // where each of them only latching one SeqVal.
  void descomposeSeqInst(VASTSeqInst *SeqInst);

  typedef std::set<VASTSeqValue*> SVSet;

  float getFaninSlack(const SVSet &S, const VASTLatch &L, VASTValue *FI);
  float getArrivialTime(VASTSeqValue *SV, const VASTLatch &L, VASTValue *FI);

  unsigned getSlotSlack(VASTSlot *S);
  void buildPipelineFIs(VASTSelector *Sel, MUXPipeliner &Pipeliner);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
    AU.addRequired<TimingNetlist>();
    // FIXME: Require dataflow annotation.
    AU.addRequired<Dataflow>();
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
  this->VM = &VM;

  typedef VASTModule::selector_iterator iterator;

  TNL = &getAnalysis<TimingNetlist>();
  DF = &getAnalysis<Dataflow>();
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

    // The slot assignments cannot be retime, the selectors with small fanin
    // number do not need to be retime.
    if (I->isSlot() || I->size() < MaxSingleCyleFINum) continue;

    pipelineFanins(I);
  }

  DEBUG(dbgs() << "After MUX pipelining:\n"; VM.dump(););

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

    // Ignore the trivial fanins.
    if (Sel->isTrivialFannin(DstLatch)) continue;

    // Do not mess up with the operations that is guarded by the strange control
    // signals.
    if (!DstLatch.Op->guardedBySlotActive()) continue;

    VASTSlot *ReadSlot = DstLatch.getSlot();

    if (ReadSlot->getValue()->getLLVMValue() && DstLatch.Op->getValue()) {
      if (DF->getDelay(ReadSlot->getValue(), DstLatch.Op, ReadSlot) <= 1.0f)
        continue;
    }

    unsigned RetimeSlack = getSlotSlack(ReadSlot);
    if (RetimeSlack == 0) continue;

    Srcs.clear();
    VASTValue *FI = VASTValPtr(DstLatch).get();
    FI->extractSupportingSeqVal(Srcs);
    float MinSlack = getFaninSlack(Srcs, DstLatch, FI);
    Srcs.clear();
    VASTValue *Guard = VASTValPtr(DstLatch.getGuard()).get();
    Guard->extractSupportingSeqVal(Srcs);
    MinSlack = std::min(MinSlack, getFaninSlack(Srcs, DstLatch, Guard));

    // Make sure the Retime slack is not negative
    if (MinSlack <= 0.0f) continue;

    DEBUG(dbgs() << "Fanin Pipelining opportnity: Slack: " << MinSlack
                 << " RetimeSlack: " << RetimeSlack << '\n');

    // Adjust the retime slack according to the timing slack.
    unsigned FISlack = std::min<unsigned>(RetimeSlack, floor(MinSlack));
    if (FISlack) Pipeliner.addFannin(DstLatch, FISlack);
  }
}

float SelectorPipelining::getFaninSlack(const SVSet &S, const VASTLatch &L,
                                        VASTValue *FI) {
  float slack = 256.0f;
  typedef SVSet::const_iterator iterator;
  for (iterator I = S.begin(), E = S.end(); I != E; ++I) {
    VASTSeqValue *SV = *I;
    // Do not retime if we do not have any timing information.
    if (SV == 0) return 0;

    // Do not retime across the static register and the output of functional
    // units, we do not have the accurate timing information for them.
    if (SV->isStatic() || SV->isFUOutput()) return 0;

    unsigned Interval = STGDist->getIntervalFromDef(SV, L.getSlot());

    float Arrival = getArrivialTime(SV, L, FI);

    slack = std::min(slack, float(Interval) - Arrival);
  }

  // Dirty HACK: Avoid retime to the assignment slot of the FI for now.
  slack -= 1.0f;

  return slack;
}

float SelectorPipelining::getArrivialTime(VASTSeqValue *SV, const VASTLatch &L,
                                          VASTValue *FI) {
  if (!SV->getLLVMValue() || DataflowInst(L.Op).getPointer() == 0)
    return TNL->getDelay(SV, FI);

  VASTSlot *S = L.getSlot();
  BasicBlock *ParentBB = S->getParent();

  // Adjust to actual parent BB for the incoming value.
  if (S->IsSubGrp) {
    S = S->getParentGroup();
    if (BasicBlock *BB = S->getParent())
      ParentBB = BB;
  }

  float TotalDelay = DF->getDelay(SV, L.Op, S);
  float EnableDelay = DF->getDelay(DataflowValue(ParentBB, true), L.Op, S);
  return std::max(TotalDelay - EnableDelay, 0.0f);
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
    FISel = VM->createRegister("fanin_" + utostr_32(RegCounter) + "r",
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

    // Pipeline the fanin value.
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

  // Iterate over the fanins, divide them into MaxSingleCyleFINum groups.
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
