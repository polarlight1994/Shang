
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

#include "shang/FUInfo.h"
#include "shang/VASTMemoryPort.h"
#include "shang/VASTExprBuilder.h"

#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"
#include "shang/Passes.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-selector-mux-synthesis"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumPipelineRegBits, "Number of Pipeline register created");

namespace {
struct SeqSelectorSynthesis : public VASTModulePass {
  TimingNetlist *TNL;
  SeqLiveVariables *SLV;
  STGShortestPath *SSP;
  unsigned MaxSingleCyleFINum;
  VASTExprBuilder *Builder;
  VASTModule *VM;
  // Number of cycles at a specificed slot that we can move back unconditionally.
  std::map<unsigned, unsigned> SlotSlack;

  static char ID;

  SeqSelectorSynthesis() : VASTModulePass(ID), TNL(0), SLV(0), SSP(0) {
    initializeSeqSelectorSynthesisPass(*PassRegistry::getPassRegistry());

    VFUMux *Mux = getFUDesc<VFUMux>();
    MaxSingleCyleFINum = 2;
    while (Mux->getMuxLatency(MaxSingleCyleFINum) < 0.9
           && MaxSingleCyleFINum < Mux->MaxAllowedMuxSize)
      ++MaxSingleCyleFINum;
  }

  bool pipelineFanins(VASTSeqValue *SV);

  typedef std::set<VASTSeqValue*> SVSet;

  unsigned getCriticalDelay(const SVSet &S, VASTValue *V);
  unsigned getAvailableInterval(const SVSet &S, VASTSlot *ReadSlot);
  unsigned getSlotSlack(VASTSlot *S);
  VASTSlot *getSlotAtLevel(VASTSlot *S, unsigned Level);

  typedef std::map<VASTLatch, unsigned> FaninSlackMap;
  void buildFISlackMap(VASTSeqValue *SV, FaninSlackMap &FISlack);

  typedef std::map<VASTLatch, VASTLatch> FaninMap;
  void AssignMUXPort(FaninSlackMap &SlackMap, VASTSeqValue *SV, FaninMap &NewFIs);

  typedef MutableArrayRef<FaninSlackMap::value_type> FISlackVector;

  void AssignMUXPort(FISlackVector FIs, unsigned Level, unsigned FIsAvailable,
                     FaninMap &NewFIs);


  typedef MutableArrayRef<VASTLatch> FIVector;
  FIVector implementFIAtLevel(FIVector FIs, unsigned Level, FaninMap &NewFIs,
                              unsigned AvaialbeFIs);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredID(DatapathNamerID);
    AU.addRequired<TimingNetlist>();
    AU.addRequired<SeqLiveVariables>();
    AU.addRequired<STGShortestPath>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
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
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
INITIALIZE_PASS_END(SeqSelectorSynthesis, "sequential-selector-synthesis",
                      "Implement the MUX for the Sequantal Logic", false, true)

char SeqSelectorSynthesis::ID = 0;

char &llvm::SeqSelectorSynthesisID = SeqSelectorSynthesis::ID;

bool SeqSelectorSynthesis::runOnVASTModule(VASTModule &VM) {
  MinimalExprBuilderContext Context(VM);
  Builder = new VASTExprBuilder(Context);
  this->VM = &VM;

  SLV = &getAnalysis<SeqLiveVariables>();
  TNL = &getAnalysis<TimingNetlist>();
  SSP = &getAnalysis<STGShortestPath>();

  // Building the Slot active signals.
  typedef VASTModule::seqval_iterator iterator;

  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I)
    pipelineFanins(I);

  delete Builder;

  return true;
}

bool SeqSelectorSynthesis::pipelineFanins(VASTSeqValue *SV) {
  // Dirty hack: only try to pipeline the fanin of the memory port at this moment.
  if (!isa<VASTMemoryBus>(SV->getParent())) return false;

  // Do not handle enable until we have the slot predicate retiming.
  if (SV->getValType() == VASTSeqValue::Enable) return false;

  // Iterate over all fanins to build the Fanin Slack Map.
  // Try to build the pipeline register by inserting the map.
  FaninSlackMap SlackMap;
  buildFISlackMap(SV, SlackMap);

  // For now, we can build the single cycle MUX, Later we can build multi-cycle
  // MUX to save the registers.
  // FIXME: We can also reuse the assignment.
  FaninMap NewFIs;
  AssignMUXPort(SlackMap, SV, NewFIs);

  return false;
}
static int
sort_by_fo(const void *LHS, const void *RHS) {
  typedef const SeqSelectorSynthesis::FaninSlackMap::value_type T;
  const VASTLatch &LHSL = reinterpret_cast<T*>(LHS)->first;
  const VASTLatch &RHSL = reinterpret_cast<T*>(RHS)->first;

  if (LHSL.getDst() < RHSL.getDst()) return -1;

  if (LHSL.getDst() > RHSL.getDst()) return  1;

  return 0;
}

static int
sort_by_fi(const void *LHS, const void *RHS) {
  typedef const SeqSelectorSynthesis::FaninSlackMap::value_type T;
  const VASTLatch &LHSL = reinterpret_cast<T*>(LHS)->first;
  const VASTLatch &RHSL = reinterpret_cast<T*>(RHS)->first;

  if (VASTValPtr(LHSL) < VASTValPtr(RHSL)) return -1;

  if (VASTValPtr(LHSL) > VASTValPtr(RHSL)) return  1;

  return 0;
}

static int
sort_by_slack(const void *LHS, const void *RHS) {
  typedef const SeqSelectorSynthesis::FaninSlackMap::value_type T;

  if (reinterpret_cast<T*>(LHS)->second < reinterpret_cast<T*>(RHS)->second)
    return -1;

  if (reinterpret_cast<T*>(LHS)->second > reinterpret_cast<T*>(RHS)->second)
    return 1;

  return sort_by_fi(LHS, RHS);
}

void
SeqSelectorSynthesis::AssignMUXPort(FaninSlackMap &SlackMap, VASTSeqValue *SV,
                                    FaninMap &NewFIs) {
  SmallVector<FaninSlackMap::value_type, 32>
    SlackVector(SlackMap.begin(), SlackMap.end());

  // Pick the fanins that must be implemented at this level.
  array_pod_sort(SlackVector.begin(), SlackVector.end(), sort_by_slack);
  AssignMUXPort(SlackVector, 0, MaxSingleCyleFINum, NewFIs);

  typedef VASTSeqValue::iterator vn_itertor;
  for (vn_itertor I = SV->begin(), E = SV->end(); I != E; ++I) {
    VASTLatch L = *I;
    VASTLatch NewL = NewFIs[L];

    if (bool(NewL) == false || NewL == L) continue;

    dbgs() << "Orignal FI:\t";
    L.Op->dump();
    L.replaceUsedBy(NewL.getDst());
    dbgs() << "Retimed to:\t";
    L.Op->dump();
    dbgs() << '\n';
  }
}

SeqSelectorSynthesis::FIVector
SeqSelectorSynthesis::implementFIAtLevel(FIVector FIs, unsigned Level,
                                         FaninMap &NewFIs, unsigned AvailabeFIs) {
  // Nothing to do.
  if (Level == 0 || FIs.size() == 0) return FIVector();

  VASTSeqValue *Dst = FIs.front().getDst();
  unsigned RegNum = VM->num_seqvals();
  std::string Name = std::string(Dst->getName()) + "p" + utohexstr(Level)
                     + "n" + utohexstr(RegNum) + "r";

  // FIXME: Create the SeqVal with the same type as the Dst.
  VASTRegister *R = VM->addDataRegister(Name, Dst->getBitWidth(), RegNum);
  VASTSeqValue *PipelineSVal = R->getValue();
  NumPipelineRegBits += PipelineSVal->getBitWidth();

  unsigned UsedFIs = 0;

  VASTValPtr LastFI = 0;

  for (unsigned i = 0; i < FIs.size(); ++i) {
    VASTLatch L = FIs[i];
    VASTValPtr CurFI = L;
    if (CurFI != LastFI) {
      --AvailabeFIs;
      // Return if we use all the available fanins.
      if (AvailabeFIs == 0) return FIs.slice(i, FIs.size() - i);
    }

    // Get the slot at current level.
    VASTSlot *S = getSlotAtLevel(L.getSlot(), Level);
    VASTSeqCtrlOp *Op
      = VM->assignCtrlLogic(PipelineSVal, CurFI, S, L.getPred(), true);
    NewFIs[L] = Op->getDef(0);
    assert(PipelineSVal->verify() && "Bad retiming?");

    dbgs().indent(Level * 2) << "Retimed fanin:\n\t";
    Op->dump();
    dbgs().indent(Level * 2 + 2) << "For\n\t";
    L.Op->dump();

    LastFI = CurFI;
  }

  // All fanin are pipelined.
  return FIVector();
}

void SeqSelectorSynthesis::AssignMUXPort(FISlackVector FIs, unsigned Level,
                                         unsigned FIsAvailable, FaninMap &NewFIs) {
  // Special case: If the slack of the Fanins are equal to the level number, it
  // means we can only assign the fanins to the current level and cannot pipeline
  // it anymore.
  FISlackVector NextLevelFISlacks;

  for (unsigned i = 0; i < FIs.size(); ++i) {
    assert(FIs[i].second >= Level && "Bad Slack!");
    if (FIs[i].second > Level) {
      // FIXME: count the identical fainins.
      FIsAvailable -= i;
      NextLevelFISlacks = FIs.slice(i, FIs.size() - i);
      break;
    }
  }

  if (NextLevelFISlacks.empty()) return;

  // Assign the other registers to the left fanins.
  // FIXME: Count the identical fanins instead of using the size of the array!
  if (FIsAvailable >= NextLevelFISlacks.size()) {
    SmallVector<VASTLatch, 8> NextLevelFIs;
    for (unsigned i = 0; i < NextLevelFISlacks.size(); ++i)
      NextLevelFIs.push_back(NextLevelFISlacks[i].first);
    // All fanins can be fitted at this level.
    FIVector CurFIs = NextLevelFIs;

    // Sort the fanins according to their slacks, so that the Fanins with similar
    // slacks are grouped together.
    array_pod_sort(CurFIs.begin(), CurFIs.end(), sort_by_fi);
    while (CurFIs.size())
      CurFIs = implementFIAtLevel(CurFIs, Level, NewFIs, MaxSingleCyleFINum);

    return;
  }

  // We have to goto next level if the number of FIs are not enought.
  AssignMUXPort(NextLevelFISlacks, Level + 1,  FIsAvailable * MaxSingleCyleFINum,
                NewFIs);

  if (Level == 0) return;

  // We must assign the FIs in NextLevelFis to #FIsAvailable fanins.
  FaninMap ReverseMap;
  SmallVector<VASTLatch, 8> NewFIVector;

  for (unsigned i = 0; i < NextLevelFISlacks.size(); ++i) {
    VASTLatch L = FIs[i].first;
    VASTLatch NewFIL = NewFIs[L];
    if (bool(NewFIL) == false) NewFIL = L;

    ReverseMap[NewFIL]=L;
    NewFIVector.push_back(NewFIL);
  }

  // Group the new FIs by the fanout register.
  array_pod_sort(NewFIVector.begin(), NewFIVector.end(), sort_by_fi);
  VASTSeqValue *LastFI = 0;
  VASTSeqValue *LastSeqVal = 0;
  for (unsigned i = 0; i < NewFIVector.size(); ++i) {
    VASTLatch L = NewFIVector[i];
    VASTSeqValue *CurFI = L.getDst();
    if (CurFI != LastFI) {
      // We need to create a new register for the new fanin.
      unsigned RegNum = VM->num_seqvals();
      std::string Name = std::string(CurFI->getName()) + "p" + utohexstr(Level)
                         + "n" + utohexstr(RegNum) + "r";

      VASTRegister *R = VM->addDataRegister(Name, CurFI->getBitWidth(), RegNum);
      LastSeqVal = R->getValue();
      NumPipelineRegBits += LastSeqVal->getBitWidth();
    }

    // Get the slot at current level.
    VASTLatch FinalL = ReverseMap[L];
    VASTSlot *S = getSlotAtLevel(FinalL.getSlot(), Level);
    // TODO: Also optimize the predicate, do not use the slot active.
    VASTSeqCtrlOp *Op
      = VM->assignCtrlLogic(LastSeqVal, CurFI, S, VASTImmediate::True, true);
    // Update the mapping, the new Fanin is assigned to a new pipeline register.
    NewFIs[FinalL] = Op->getDef(0);

    LastFI = CurFI;
  }
}

void SeqSelectorSynthesis::buildFISlackMap(VASTSeqValue *SV,
                                           FaninSlackMap &FISlack) {
  SVSet Srcs;

  typedef VASTSeqValue::iterator vn_itertor;
  for (vn_itertor I = SV->begin(), E = SV->end(); I != E; ++I) {
    Srcs.clear();

    VASTLatch &DstLatch = *I;
    VASTSlot *ReadSlot = DstLatch.getSlot();

    unsigned RetimeSlack = getSlotSlack(ReadSlot);

    unsigned CriticalDelay = 0;
    unsigned AvailableInterval = STGShortestPath::Inf;

    VASTValPtr FI = DstLatch;
    FI->extractSupporingSeqVal(Srcs);
    if (!Srcs.empty()) {
      CriticalDelay = std::max(CriticalDelay, getCriticalDelay(Srcs, FI.get()));
      AvailableInterval
        = std::min(AvailableInterval, getAvailableInterval(Srcs, ReadSlot));
      Srcs.clear();
    }

    VASTValPtr Pred = DstLatch.getPred();
    // We should also retime the predicate together with the fanin.
    Pred->extractSupporingSeqVal(Srcs);
    if (!Srcs.empty()) {
      CriticalDelay = std::max(CriticalDelay, getCriticalDelay(Srcs, Pred.get()));
      AvailableInterval
        = std::min(AvailableInterval, getAvailableInterval(Srcs, ReadSlot));
    }

    if (CriticalDelay >= AvailableInterval) continue;

    DEBUG(dbgs() << "Fanin Pipelining opportnity: Slack: "
           << (AvailableInterval - CriticalDelay)
           << " RetimeSlack: " << RetimeSlack << '\n');

    // Adjust the retime slack according to the timing slack.
    RetimeSlack = std::min(RetimeSlack, AvailableInterval - CriticalDelay);

    FISlack[DstLatch] = RetimeSlack;
  }

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

  // Dirty HACK: Avoid retime to the assignment slot of the FI for now.
  Interval -= 1;

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

VASTSlot *SeqSelectorSynthesis::getSlotAtLevel(VASTSlot *S, unsigned Level) {
  assert(Level && "Bad level!");
  // Otherwise we need to calculate it now.
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

    if (IsConditional) return 0;

    S = PredSlot;
    --Level;

    // If we reach the slot at destination level, return the slot.
    if (Level == 0) return S;
  }

  return 0;
}
