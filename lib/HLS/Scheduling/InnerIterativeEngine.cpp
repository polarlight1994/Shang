//===- IterativeSchedulingBinding.cpp - Scheduling/Binding Loop -*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the iterative scheduling/binding engine.
//
//===----------------------------------------------------------------------===//

#include "SDCScheduler.h"

#include "vast/LuaI.h"
#include "vast/VASTModule.h"

#include "llvm/IR/Function.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-iterative-scheduling-binding"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(NumIterations, "Number of Scheduling-Binding Iterations");

typedef ArrayRef<VASTSchedUnit*> SUArrayRef;

typedef std::map<Value*, SmallVector<VASTSchedUnit*, 4> > IR2SUMapTy;

static VASTSchedUnit *LookupSU(VASTSeqOp *Op, const IR2SUMapTy &Map) {
  Value *V = Op->getValue();
  assert(V && "Unexpected virtual node!");
  IR2SUMapTy::const_iterator I = Map.find(V);
  assert(I != Map.end() && "Corresponding scheduling units not found!");
  SUArrayRef SUs(I->second);

  assert(SUs.size() && "Unexpected empty array!");
  for (unsigned i = 0; i < SUs.size(); ++i) {
    VASTSchedUnit *SU = SUs[i];
    if (SU->getSeqOp() == Op)
      return SU;
  }

  llvm_unreachable("SU not found!");
  return 0;
}


namespace {
struct SelectorSlackVerifier {
  IR2SUMapTy &IR2SUMap;
  SDCScheduler &Scheduler;
  VASTSelector *Sel;
  typedef std::map<VASTSchedUnit*, unsigned> SrcSlackMapTy;
  SrcSlackMapTy MinimalSlacks;
  typedef std::map<VASTSchedUnit*, SrcSlackMapTy> SlackMapTy;
  SlackMapTy SlackMap;
  const unsigned MaxFIPerLevel;
  const unsigned AverageMUXLevel;
  const float PenaltyFactor;

  SelectorSlackVerifier(IR2SUMapTy &IR2SUMap, SDCScheduler &Scheduler,
                        VASTSelector *Sel, float PenaltyFactor)
    : IR2SUMap(IR2SUMap), Scheduler(Scheduler), Sel(Sel),
      MaxFIPerLevel(LuaI::Get<VFUMux>()->getMaxAllowdMuxSize(Sel->getBitWidth())),
      AverageMUXLevel((Log2_32_Ceil(Sel->numNonTrivialFanins()) - 1) / Log2_32_Ceil(MaxFIPerLevel) + 1),
      PenaltyFactor(PenaltyFactor) {}

  bool preserveFaninConstraint() {
    std::vector<VASTSchedUnit*> SUs;
    std::set<VASTSeqValue*> Srcs;

    typedef VASTSelector::iterator vn_itertor;
    for (vn_itertor I = Sel->begin(), E = Sel->end(); I != E; ++I) {
      VASTLatch &DstLatch = *I;

      // Ignore the trivial fanins.
      if (Sel->isTrivialFannin(DstLatch)) continue;

      VASTSchedUnit *U = LookupSU(DstLatch.Op, IR2SUMap);
      SUs.push_back(U);

      // Get the source SU according to the structure of the combinatioal cone.
      Srcs.clear();

      VASTValue *FI = VASTValPtr(DstLatch).get();
      FI->extractCombConeLeaves(Srcs);

      VASTValue *Guard = VASTValPtr(DstLatch.getGuard()).get();
      Guard->extractCombConeLeaves(Srcs);

      MinimalSlacks[U] = buildSlackMap(U, Srcs);
    }

    return preserveFaninConstraint(MaxFIPerLevel, 1, SUs);
  }

  VASTSchedUnit *getDataDepSU(VASTSeqValue *SV) {
    Value *V = SV->getLLVMValue();
    bool IsPHI = isa<PHINode>(V);

    if (isa<Argument>(V)) return Scheduler->getEntry();

    IR2SUMapTy::const_iterator at = IR2SUMap.find(V);
    assert(at != IR2SUMap.end() && "Flow dependencies missed!");

    // Get the corresponding latch SeqOp.
    ArrayRef<VASTSchedUnit*> SUs(at->second);
    VASTSeqValue *SrcSeqVal = 0;
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *CurSU = SUs[i];

      if (isa<BasicBlock>(V) && CurSU->isBBEntry())
        return CurSU;

      // Are we got the VASTSeqVal corresponding to V?
      if (CurSU->isLatching(V)) {
        assert((SrcSeqVal == 0
                || SrcSeqVal == CurSU->getSeqOp()->getDef(0))
               && "All PHI latching SeqOp should define the same SeqOp!");
        SrcSeqVal = CurSU->getSeqOp()->getDef(0);

        if (IsPHI) continue;

        // We are done if we are looking for the Scheduling Unit for common
        // instruction.
        return CurSU;
      }

      if (IsPHI && CurSU->isPHI()) return CurSU;
    }

    (void) SrcSeqVal;

    llvm_unreachable("No source SU?");
    return 0;
  }

  unsigned buildSlackMap(VASTSchedUnit *U, std::set<VASTSeqValue*> &Srcs) {
    SrcSlackMapTy &SrcSlacks = SlackMap[U];
    unsigned MinimalSlack = UINT32_MAX;

    typedef std::set<VASTSeqValue*>::iterator iterator;

    for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      VASTSeqValue *SV = *I;
      VASTSchedUnit *Src = getDataDepSU(SV);

      int Latnecy = U->getDFLatency(Src);
      if (Latnecy < 0)
        continue;

      unsigned EdgeDistance = U->getSchedule() - Src->getSchedule();
      assert(EdgeDistance >= unsigned(U->getEdgeFrom(Src).getLatency())
             && "Bad schedule that does not preserve latency constraint!");

      unsigned CurSlack = EdgeDistance - unsigned(Latnecy);

      // Ignore the BBEntry, if they are fused together.
      if (Src->isBBEntry() && CurSlack == 0)
        continue;

     SrcSlacks[Src] = CurSlack;
     MinimalSlack = std::min(MinimalSlack, CurSlack);
    }

    return MinimalSlack;
  }

  bool preserveFaninConstraint(unsigned AvailableFanins, unsigned CurLevel,
                               MutableArrayRef<VASTSchedUnit*> SUs) {
    assert(CurLevel < 10 && "Unexpected number of pipeline stages!");
    SmallVector<VASTSchedUnit*, 8> UsedSUs;

    unsigned UsedFanins = 0;
    unsigned RemainFanins = 0;
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *SU = SUs[i];

      if (SU == 0)
        continue;

      ++RemainFanins;
      if (hasExtraSlack(SU, CurLevel))
        continue;

      // If there is no extra slack, one fanin is consumed.
      ++UsedFanins;
      // Set this element to null, so that we will not visit it again in the
      // next level.
      SUs[i] = 0;
      UsedSUs.push_back(SU);
    }

    if (AvailableFanins < UsedFanins)
      return false;

    if (AvailableFanins == UsedFanins && RemainFanins > AvailableFanins)
      return false;

    if (RemainFanins == 0 || RemainFanins <= AvailableFanins)
      return true;

    // Subtract the used fanins from the available fanins.
    AvailableFanins -= UsedFanins;
    // We get extra fanins as we go to next level.
    AvailableFanins *= MaxFIPerLevel;
    return preserveFaninConstraint(AvailableFanins, CurLevel + 1, SUs);
  }

  unsigned getMinimalSlack(VASTSchedUnit *SU) const {
    SrcSlackMapTy::const_iterator J = MinimalSlacks.find(SU);
    assert(J != MinimalSlacks.end() && "SU not in the map?");
    return J->second;
  }

  bool hasExtraSlack(VASTSchedUnit *SU, unsigned Level) const {
    return getMinimalSlack(SU) > Level;
  }

  void applyPenalties() {
    typedef SlackMapTy::const_iterator iterator;
    for (iterator I = SlackMap.begin(), E = SlackMap.end(); I != E; ++I) {
      VASTSchedUnit *Dst = I->first;
      applyPenalties(Dst, I->second);
    }
  }

  void applyPenalties(VASTSchedUnit *Dst, const SrcSlackMapTy &SrcSlacks) {
    typedef SDCScheduler::SoftConstraint SoftConstraint;
    typedef SrcSlackMapTy::const_iterator iterator;
    unsigned MinimalSlack = getMinimalSlack(Dst);
    double CurPenalty = double(PenaltyFactor) * double(AverageMUXLevel) /
                        double(MinimalSlack + 1);

    for (iterator I = SrcSlacks.begin(), E = SrcSlacks.end(); I != E; ++I) {
      VASTSchedUnit *Src = I->first;
      unsigned Slack = I->second;

      unsigned Latency = Dst->getDFLatency(Src);
      unsigned ExpectedSlack = unsigned(Latency) + MinimalSlack + 1;

      SoftConstraint &SC = Scheduler.getOrCreateSoftConstraint(Src, Dst);
      SC.C = ExpectedSlack;

      if (Slack == MinimalSlack) {
        SC.Penalty += CurPenalty;
        return;
      }

      assert(Slack > MinimalSlack && "Unexpected slack");
      SC.Penalty *= 0.9;
    }
  }
};

struct ItetrativeEngine {
  SDCScheduler Scheduler;
  DominatorTree &DT;
  PostDominatorTree &PDT;
  BlockFrequencyInfo &BFI;
  BranchProbabilityInfo &BPI;

  IR2SUMapTy &IR2SUMap;

  enum State {
    Initial, Scheduling, Binding
  };

  State S;
  unsigned ScheduleViolation, BindingViolation, MUXFIViolation;
  float TotalWeight;
  const float PerformanceFactor, ResourceFactor;

  ItetrativeEngine(VASTSchedGraph &G, LoopInfo &LI,
                   DominatorTree &DT, PostDominatorTree &PDT,
                   BlockFrequencyInfo &BFI, BranchProbabilityInfo &BPI,
                   IR2SUMapTy &IR2SUMap)
    : Scheduler(G, 1, DT, LI), DT(DT), PDT(PDT), BFI(BFI), BPI(BPI),
      IR2SUMap(IR2SUMap), S(Initial), ScheduleViolation(0), BindingViolation(0),
      MUXFIViolation(0), TotalWeight(0.0), PerformanceFactor(8.0f),
      ResourceFactor(0.1f) {
    // Build the hard linear order.
    Scheduler.addLinOrdEdge(PDT, IR2SUMap);
    Scheduler.initalizeCFGEdges();
  }

  static VASTSchedUnit *getLaunch(SUArrayRef SUs) {
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *SU = SUs[i];
      if (!SU->isLaunch())
        continue;

      return SU;
    }

    llvm_unreachable("SU not found!");
    return 0;
  }

  bool alap_less(const VASTSchedUnit *LHS, const VASTSchedUnit *RHS) const {
    if (LHS->getSchedule() < RHS->getSchedule())
      return true;

    if (RHS->getSchedule() < LHS->getSchedule())
      return false;

    // Ascending order using ALAP.
    if (Scheduler.getALAPStep(LHS) < Scheduler.getALAPStep(RHS)) return true;
    if (Scheduler.getALAPStep(LHS) > Scheduler.getALAPStep(RHS)) return false;

    // Tie breaker 1: ASAP.
    if (Scheduler.getASAPStep(LHS) < Scheduler.getASAPStep(RHS)) return true;
    if (Scheduler.getASAPStep(LHS) > Scheduler.getASAPStep(RHS)) return false;

    // Tie breaker 2: Original topological order.
    return LHS->getIdx() < RHS->getIdx();
  }

  void updateSchedulingConstraint(VASTSchedUnit *Src, VASTSchedUnit *Dst,
                                  unsigned C, float Penalty) {
    if (S != Scheduling)
      return;

    if (Penalty > 0)
      Scheduler.addSoftConstraint(Src, Dst, C, Penalty);

    ++ScheduleViolation;
  }

  bool performScheduling(VASTModule &VM);

  float assignEdgeWeight(BasicBlock *BB);

  bool performSchedulingAndAllocateMuxSlack(VASTModule &VM) {
    if (!performScheduling(VM))
      return false;

    float NumBBs = VM.getFunction()->getBasicBlockList().size();
    MUXFIViolation = 0;

    typedef VASTModule::selector_iterator iterator;
    for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
      VASTSelector *Sel = I;

      SelectorSlackVerifier SSV(IR2SUMap, Scheduler, Sel, TotalWeight / NumBBs);

      if (Sel->isSlot() || Sel->size() < SSV.MaxFIPerLevel) continue;

      if (SSV.preserveFaninConstraint())
        continue;

      SSV.applyPenalties();

      // Perform schedule again with the updated soft constraints.
      S = Scheduling;
      ++MUXFIViolation;
    }

    return MUXFIViolation > 0;
  }
};
}

bool ItetrativeEngine::performScheduling(VASTModule &VM) {
  // No need to modify the scheduling if the FU compatiblity is preserved.
  //if (S == Scheduling && !checkCompatibility())
  //  return false;

  Scheduler->resetSchedule();

  if (!Scheduler.createLPAndVariables())
    return false;

  TotalWeight = 0.0f;

  Function &F = *VM.getFunction();
  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;

    if (!Scheduler->isBBReachable(BB))
      continue;

    DEBUG(dbgs() << "Applying constraints to BB: " << BB->getName() << '\n');

    float ExitWightSum = assignEdgeWeight(BB);
    TotalWeight += ExitWightSum;
  }

  // Scheduler.addObjectCoeff(Scheduler->getExit(), - 1.0 * (TotalWeight /*+ PerformanceFactor*/));

  //Scheduler.buildOptSlackObject(1.0);

  bool success = Scheduler.schedule();
  assert(success && "SDCScheduler fail!");

  // We had made some changes.
  return success;
}

float ItetrativeEngine::assignEdgeWeight(BasicBlock *BB) {
  VASTSchedUnit *Entry = Scheduler->getEntrySU(BB);
  // Get the frequency of the block, and ensure the frequency always bigger
  // than 0.
  BlockFrequency BF = std::max(BFI.getBlockFreq(BB), BlockFrequency(1));

  float ExitWightSum = 0.0f;
  ArrayRef<VASTSchedUnit*> Exits(IR2SUMap[BB->getTerminator()]);
  for (unsigned i = 0; i < Exits.size(); ++i) {
    VASTSchedUnit *BBExit = Exits[i];
    // Ignore the return value latching operation here. We will add the fix
    // timing constraints between it and the actual terminator.
    if (!BBExit->isTerminator()) {
      assert(isa<ReturnInst>(BB->getTerminator()) && "BBExit is not terminator!");
      continue;
    }

    BranchProbability BP = BranchProbability::getOne();
    if (BasicBlock *TargetBB = BBExit->getTargetBlock())
      BP = BPI.getEdgeProbability(BB, TargetBB);

    BlockFrequency CurBranchFreq = BF * BP;
    float ScaledCurBranchFreq = float(CurBranchFreq.getFrequency()) /
                                float(BlockFrequency::getEntryFrequency());
    float ExitWeight = (PerformanceFactor * ScaledCurBranchFreq);
    // Minimize the edge weight, i.e. BBExit - BBEntry =>
    // Maximize BBEntry - BBExit
    Scheduler.addObjectCoeff(Entry, 1.0 * ExitWeight);
    Scheduler.addObjectCoeff(BBExit, - 1.0 * ExitWeight);
    DEBUG(dbgs().indent(4) << "Setting Exit Weight: " << ExitWeight
                           << ' ' << BP << '\n');

    ExitWightSum += ExitWeight;
  }

  // At the same time, maximize BBEntry for the shortest path problem.
  //float ScaledBBFreq = float(BF.getFrequency()) /
  //                     float(BlockFrequency::getEntryFrequency());
  //Scheduler.addObjectCoeff(Entry, 1.0 * PerformanceFactor * ScaledBBFreq);
  return ExitWightSum;
}

void VASTScheduling::scheduleGlobal() {
  BranchProbabilityInfo &BPI = getAnalysis<BranchProbabilityInfo>();
  BlockFrequencyInfo &BFI = getAnalysis<BlockFrequencyInfo>();
  PostDominatorTree &PDT = getAnalysis<PostDominatorTree>();

  ItetrativeEngine ISB(*G, *LI, *DT, PDT, BFI, BPI, IR2SUMap);
  while (ISB.performSchedulingAndAllocateMuxSlack(*VM)) {
    ++NumIterations;
    dbgs() << "Schedule Violations: " << ISB.ScheduleViolation << ' '
           << "Binding Violations:" << ISB.BindingViolation << ' '
           << "MUX Fanins Violations:" << ISB.MUXFIViolation << '\n';
  }

  DEBUG(G->viewGraph());
}
