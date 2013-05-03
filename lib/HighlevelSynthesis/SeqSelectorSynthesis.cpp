
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
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "shang-selector-mux-synthesis"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumPipelineRegBits, "Number of Pipeline register created");

static cl::opt<bool> EnableMUXPipelining("shang-enable-mux-pipelining",
  cl::desc("Perform MUX pipelining"),
  cl::init(true));

namespace {
struct MUXFI {
  VASTSeqOp *Op;
  VASTValPtr FIVal;
  MUXFI(const VASTLatch &L) : Op(L.Op), FIVal(L) {}
  MUXFI(VASTSeqOp *Op) : Op(Op), FIVal(Op->getPred()) {}
  
  VASTValPtr getPred() const { return Op->getPred(); }
  VASTSlot *getSlot() const { return Op->getSlot(); }

  bool operator<(const MUXFI &RHS) const {
    return Op < RHS.Op;
  }
};

typedef std::map<MUXFI, unsigned> FaninSlackMap;

struct MUXPipeliner {
  typedef std::map<MUXFI, VASTValPtr> FaninMap;
  typedef std::map<MUXFI, VASTSeqValue*> PredMap;
  FaninMap NewFIs;
  PredMap NewPreds;

  std::string BaseName;
  unsigned BitWidth;
  VASTSeqValue::Type ValTy;
  unsigned MaxPerCyleFINum;
  VASTModule *VM;

  MUXPipeliner(std::string BaseName, unsigned BitWidth, VASTSeqValue::Type T,
               unsigned MaxPerCyleFINum, VASTModule *VM)
    : BaseName(BaseName), BitWidth(BitWidth), ValTy(T),
      MaxPerCyleFINum(MaxPerCyleFINum), VM(VM) {}

  typedef
  MutableArrayRef<FaninSlackMap::value_type> FISlackVector;

  void AssignMUXPort(FISlackVector FIs, unsigned Level, unsigned FIsAvailable);

  static VASTSlot *getSlotAtLevel(VASTSlot *S, unsigned Level);

  VASTValPtr getFIVal(MUXFI FI) {
    FaninMap::const_iterator at = NewFIs.find(FI);

    if (at == NewFIs.end()) return FI.FIVal;

    return at->second;
  }

  VASTValPtr getFIPred(MUXFI FI) {
    PredMap::const_iterator at = NewPreds.find(FI);

    if (at == NewPreds.end()) return FI.getPred();

    return at->second;
  }
};

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
  // Decompose a SeqInst latching more than one SeqVal to several SeqInsts
  // where each of them only latching one SeqVal.
  void descomposeSeqInst(VASTSeqInst *SeqInst);

  typedef std::set<VASTSeqValue*> SVSet;

  unsigned getCriticalDelay(const SVSet &S, VASTValue *V);
  unsigned getAvailableInterval(const SVSet &S, VASTSlot *ReadSlot);
  unsigned getSlotSlack(VASTSlot *S);

  void buildFISlackMap(VASTSeqValue *SV, FaninSlackMap &FISlack);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);

    if (EnableMUXPipelining) {
      AU.addRequired<SeqLiveVariables>();
      AU.addRequiredID(DatapathNamerID);
      AU.addRequired<TimingNetlist>();
      AU.addRequired<STGShortestPath>();
    }

    AU.addPreserved<SeqLiveVariables>();

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

  typedef VASTModule::seqval_iterator iterator;

  if (EnableMUXPipelining) {
    TNL = &getAnalysis<TimingNetlist>();
    SSP = &getAnalysis<STGShortestPath>();
    SLV = &getAnalysis<SeqLiveVariables>();

    std::vector<VASTSeqInst*> Worklist;

    typedef VASTModule::seqop_iterator seqop_iterator;
    for (seqop_iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I)
      if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(I))
        if (SeqInst->getNumSrcs() > 1)
          Worklist.push_back(SeqInst);

    while (!Worklist.empty()) {
      VASTSeqInst *SeqInst = Worklist.back();
      Worklist.pop_back();

      descomposeSeqInst(SeqInst);
    }

    DEBUG(dbgs() << "Before MUX pipelining:\n"; VM.dump(););

    for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
      if (!isa<VASTMemoryBus>(I->getParent())) continue;

      pipelineFanins(I);
    }

    DEBUG(dbgs() << "After MUX pipelining:\n"; VM.dump(););
  }
  
  // Eliminate the identical SeqOps.
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I)
    I->synthesisSelector(*Builder);

  delete Builder;
  return true;
}

void SeqSelectorSynthesis::descomposeSeqInst(VASTSeqInst *SeqInst) {
  unsigned NumDefs = SeqInst->getNumDefs();

  for (unsigned i = 0, e = SeqInst->getNumSrcs(); i != e; ++i) {
    VASTLatch L = SeqInst->getSrc(i);

    VASTSeqInst *NewSeqInst = VM->lauchInst(L.getSlot(), L.getPred(), 1,
                                            SeqInst->getValue(),
                                            VASTSeqInst::Latch);
    NewSeqInst->addSrc(VASTValPtr(L), 0, i < NumDefs, L.getDst());
  }

  SeqInst->getSlot()->removeOp(SeqInst);
  VM->eraseSeqOp(SeqInst);
}

static int
sort_by_slot(const VASTSlot *LHS, const VASTSlot *RHS) {
  if (LHS < RHS) return -1;

  if (LHS > RHS) return  1;

  return 0;
}

static int
sort_by_fi(const void *LHS, const void *RHS) {
  typedef const MUXPipeliner::FaninMap::value_type T;
  const VASTValPtr &LHSV = reinterpret_cast<T*>(LHS)->second;
  const VASTValPtr &RHSV = reinterpret_cast<T*>(RHS)->second;

  if (LHSV < RHSV) return -1;

  if (LHSV > RHSV) return  1;

  return sort_by_slot(reinterpret_cast<T*>(LHS)->first.Op->getSlot(),
                      reinterpret_cast<T*>(RHS)->first.Op->getSlot());
}

static int
sort_by_slack(const void *LHS, const void *RHS) {
  typedef const FaninSlackMap::value_type T;

  if (reinterpret_cast<T*>(LHS)->second < reinterpret_cast<T*>(RHS)->second)
    return -1;

  if (reinterpret_cast<T*>(LHS)->second > reinterpret_cast<T*>(RHS)->second)
    return 1;

  return 0;
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

bool SeqSelectorSynthesis::pipelineFanins(VASTSeqValue *SV) {
  // Iterate over all fanins to build the Fanin Slack Map.
  // Try to build the pipeline register by inserting the map.
  FaninSlackMap SlackMap;
  buildFISlackMap(SV, SlackMap);

  // For now, we can build the single cycle MUX, Later we can build multi-cycle
  // MUX to save the registers.
  // FIXME: We can also reuse the assignment.
  SmallVector<FaninSlackMap::value_type, 32>
    SlackVector(SlackMap.begin(), SlackMap.end());

  // Pick the fanins that must be implemented at this level.
  array_pod_sort(SlackVector.begin(), SlackVector.end(), sort_by_slack);

  MUXPipeliner P(std::string(SV->getName()), SV->getBitWidth(), SV->getValType(),
                 MaxSingleCyleFINum, VM); 
  P.AssignMUXPort(SlackVector, 0, MaxSingleCyleFINum);

  typedef VASTSeqValue::iterator vn_itertor;
  for (vn_itertor I = SV->begin(), E = SV->end(); I != E; ++I) {
    VASTLatch L = *I;
    VASTValPtr NewPred = P.NewPreds[L];

    if (!NewPred) continue;

    L.Op->replacePredBy(NewPred, false);

    VASTValPtr NewFI = P.NewFIs[L];
    assert((NewFI || SV->getValType() == VASTSeqValue::Enable)
           && "Cannot find the corresponding fanin!");

    if (!NewFI) continue;

    DEBUG(dbgs() << "Orignal FI:\t";
    L.Op->dump(););

    assert(L.Op->getNumSrcs() == 1
           && "Cannot pipeline FI in the SeqOp with more than 1 sources!");

    L.replaceUsedBy(NewFI);

    DEBUG(dbgs() << "Retimed to:\t";
    L.Op->dump();
    dbgs() << '\n';);
  }

  return false;
}

void MUXPipeliner::AssignMUXPort(FISlackVector FIs, unsigned Level,
                                 unsigned FIsAvailable) {
  // Special case: If the slack of the Fanins are equal to the level number, it
  // means we can only assign the fanins to the current level and cannot pipeline
  // it anymore.
  FISlackVector NextLevelFISlacks;

  for (unsigned i = 0; i < FIs.size(); ++i) {
    assert(FIs[i].second >= Level && "Bad Slack!");
    if (FIs[i].second > Level) {
      FIsAvailable -= i;
      NextLevelFISlacks = FIs.slice(i);
      break;
    }
  }

  if (NextLevelFISlacks.empty()) return;

  DEBUG(dbgs().indent(Level * 2) << "NextLevelFISlacks Size: "
        << NextLevelFISlacks.size() << '\n';
  for (unsigned i = 0; i < NextLevelFISlacks.size(); ++i) {
    MUXFI FI = NextLevelFISlacks[i].first;

    dbgs().indent(Level * 2) << "Putting Op to next level:";
    FI.Op->dump();
  });

  // We have to goto next level if the number of FIs are not enought.
  if (FIsAvailable < NextLevelFISlacks.size())
    AssignMUXPort(NextLevelFISlacks, Level + 1,  FIsAvailable * MaxPerCyleFINum);

  // No need to insert pipeline register at level 0.
  if (Level == 0) return;

  DEBUG(dbgs().indent(Level * 2) << "NextLevelFISlacks Size: "
        << NextLevelFISlacks.size() << '\n');

  typedef std::vector<std::pair<MUXFI, VASTValPtr> > FaninVector;
  FaninVector PreviousLevelFIs;
  // We must assign the FIs in NextLevelFis to #FIsAvailable fanins.
  for (unsigned i = 0; i < NextLevelFISlacks.size(); ++i) {
    MUXFI FI = NextLevelFISlacks[i].first;
    PreviousLevelFIs.push_back(FaninMap::value_type(FI, getFIVal(FI)));
  }

  // Initialize CurUsedFI to MaxSingleCyleFINum to force fanin creation at the
  // first iteration.
  unsigned CurUsedFI = MaxPerCyleFINum;
  // Group the new FIs by the fanout register.
  array_pod_sort(PreviousLevelFIs.begin(), PreviousLevelFIs.end(), sort_by_fi);
  VASTValPtr LastPreviousLevelEn;
  VASTSeqValue *LastNextLevelFI = 0;
  VASTSeqValue *LastNextLevelEn = 0;

  for (unsigned i = 0; i < PreviousLevelFIs.size(); ++i) {
    MUXFI CurFI = PreviousLevelFIs[i].first;
    VASTValPtr CurPreviousLevelFI = PreviousLevelFIs[i].second;
    VASTValPtr CurPreviousLevelEn = getFIPred(CurFI);
    bool EnablePipelined = CurFI.getPred() != CurPreviousLevelEn;

    DEBUG(dbgs().indent(Level * 2) << "Handling FI for Op:";
    CurFI.Op->dump();
    dbgs().indent(Level * 2) << "Get enable: " << CurPreviousLevelEn << '\n';);
    
    // Count the number of fanins by the enables.
    if (CurPreviousLevelEn != LastPreviousLevelEn || EnablePipelined == false) {
      ++CurUsedFI;

      // We use all fanins of the MUX, create a new target register for the MUX.
      if (CurUsedFI >= MaxPerCyleFINum)  {
        CurUsedFI = 0;
        // We need to create a new register for the new fanin.
        unsigned RegNum = VM->num_seqvals();
        std::string Name = "l" + utostr_32(Level)
                            + BaseName
                            + "n" + utostr_32(VM->num_seqvals());
        if (ValTy != VASTSeqValue::Enable) {
          VASTRegister *R = VM->addRegister(Name + "r", BitWidth, 0, ValTy, RegNum);
          LastNextLevelFI = R->getValue();
          NumPipelineRegBits += LastNextLevelFI->getBitWidth();
        }

        // Do not build the enable for the SeqVal to be pipelined.
        VASTRegister *R = VM->addRegister(Name + "en", 1, 0,
                                          VASTSeqValue::Enable,
                                          VM->num_seqvals());
        LastNextLevelEn = R->getValue();
        NumPipelineRegBits += LastNextLevelEn->getBitWidth();
      }
    }

    VASTSlot *S = getSlotAtLevel(CurFI.getSlot(), Level);
    DEBUG(dbgs().indent(Level * 2)
      << "Retime the assignment at Slot#" << CurFI.getSlot()->SlotNum
      << " to " << S->SlotNum << " for " << BaseName << '\n');

    // Create the register assignment enable by the previous pipelined enable.
    // without the slot active if the predicte is from a pipeline register..
    if (LastNextLevelFI) {
      VASTSeqCtrlOp *Op
        = VM->assignCtrlLogic(LastNextLevelFI, CurPreviousLevelFI, S,
                              CurPreviousLevelEn, !EnablePipelined);
      // Update the mapping, the new Fanin is assigned to a new pipeline register.
      NewFIs[CurFI] = LastNextLevelFI;

      DEBUG(dbgs().indent(Level * 2) << "Inserting pipeline register: ";
      Op->dump();
      dbgs().indent(Level * 2) << "For: ";
      CurFI.Op->dump(););
    }

    // Also assign to the current level pipeline enable.
    VASTSeqCtrlOp *Op
      = VM->assignCtrlLogic(LastNextLevelEn, VASTImmediate::True, S,
                            CurPreviousLevelEn, !EnablePipelined);
    NewPreds[CurFI] = LastNextLevelEn;

    DEBUG(dbgs().indent(Level * 2) << "Inserting pipeline register: ";
    Op->dump();
    dbgs().indent(Level * 2) << "For: ";
    CurFI.Op->dump(););

    // Update the enable.
    LastPreviousLevelEn = CurPreviousLevelEn;
  }
}

void SeqSelectorSynthesis::buildFISlackMap(VASTSeqValue *SV,
                                           FaninSlackMap &FISlack) {
  SVSet Srcs;

  typedef VASTSeqValue::iterator vn_itertor;
  for (vn_itertor I = SV->begin(), E = SV->end(); I != E; ++I) {
    VASTLatch &DstLatch = *I;
    // Assume the slack is zero.
    FISlack[DstLatch] = 0;

    // Do not mess up with the operations that is guarded by the strange control
    // signals.
    if (!DstLatch.getSlotActive()) continue;

    VASTSlot *ReadSlot = DstLatch.getSlot();

    unsigned RetimeSlack = getSlotSlack(ReadSlot);
    if (RetimeSlack == 0) continue;

    unsigned CriticalDelay = 0;
    unsigned AvailableInterval = STGShortestPath::Inf;

    // Also do not retime across the SVal without liveness information.
    VASTValPtr FI = DstLatch;

    if (FI->extractSupporingSeqVal(Srcs)) {
      CriticalDelay = std::max(CriticalDelay, getCriticalDelay(Srcs, FI.get()));
      AvailableInterval
        = std::min(AvailableInterval, getAvailableInterval(Srcs, ReadSlot));
      Srcs.clear();
    }

    VASTValPtr Pred = DstLatch.getPred();
    // We should also retime the predicate together with the fanin.
    if (Pred->extractSupporingSeqVal(Srcs)) {
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
  for (iterator I = S.begin(), E = S.end(); I != E; ++I) {
    VASTSeqValue *SV = *I;
    // Do not retime if we do not have any timing information.
    if (SV->empty()) return 0;

    // Do not retime across the static register as well, we do not have the
    // accurate timing information for them.
    if (SV->getValType() == VASTSeqValue::StaticRegister) return 0;

    Interval = std::min(Interval, SLV->getIntervalFromDef(SV, ReadSlot, SSP));
  }

  assert(Interval && "Unexpected interval!");
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

VASTSlot *MUXPipeliner::getSlotAtLevel(VASTSlot *S, unsigned Level) {
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
