//===- IterativeSchedulingBinding.cpp - Scheduling/Binding Loop -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
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
#include "PreSchedBinding.h"

#include "shang/VASTModule.h"

#include "llvm/IR/Function.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
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
  typedef std::map<VASTSchedUnit*, SrcSlackMapTy> SlackMapTy;
  SlackMapTy SlackMap;
  typedef std::set<VASTSchedUnit*> ViolatedSUSet;
  ViolatedSUSet ViolatedSUs;
  const unsigned MaxFIPerLevel;
  const float PenaltyFactor;

  SelectorSlackVerifier(IR2SUMapTy &IR2SUMap, SDCScheduler &Scheduler,
                        VASTSelector *Sel, float PenaltyFactor)
    : IR2SUMap(IR2SUMap), Scheduler(Scheduler), Sel(Sel),
      MaxFIPerLevel(getFUDesc<VFUMux>()->getMaxAllowdMuxSize(Sel->getBitWidth())),
      PenaltyFactor(PenaltyFactor) {}

  bool preserveFaninConstraint() {
    std::vector<VASTSchedUnit*> SUs;

    typedef VASTSelector::iterator vn_itertor;
    for (vn_itertor I = Sel->begin(), E = Sel->end(); I != E; ++I) {
      VASTLatch &DstLatch = *I;

      // Ignore the trivial fanins.
      if (Sel->isTrivialFannin(DstLatch)) continue;

      VASTSchedUnit *U = LookupSU(DstLatch.Op, IR2SUMap);
      SUs.push_back(U);

      buildSlackMap(U);
    }

    return preserveFaninConstraint(MaxFIPerLevel, 1, SUs);
  }

  VASTSchedUnit *buildSlackMap(VASTSchedUnit *U) {
    SrcSlackMapTy &SrcSlacks = SlackMap[U];

    typedef VASTSchedUnit::dep_iterator dep_iterator;
    int NumValDeps = 0;
    for (dep_iterator I = U->dep_begin(), E = U->dep_end(); I != E; ++I) {
      int Latnecy = I.getDFLatency();
      if (Latnecy < 0)
        continue;

      VASTSchedUnit *Src = *I;
      unsigned EdgeDistance = U->getSchedule() - Src->getSchedule();
      assert(EdgeDistance >= unsigned(I.getLatency())
             && "Bad schedule that does not preserve latency constraint!");

      unsigned CurSlack = EdgeDistance - unsigned(Latnecy);

      // Ignore the BBEntry, if they are fused together.
      if (Src->isBBEntry() && CurSlack == 0)
        continue;

     SrcSlacks[Src] = CurSlack;
    }

    return U;
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

    if (AvailableFanins < UsedFanins) {
      while (!UsedSUs.empty())
        ViolatedSUs.insert(UsedSUs.pop_back_val());

      return false;
    }

    if (AvailableFanins == UsedFanins && RemainFanins > AvailableFanins) {
      while (!UsedSUs.empty())
        ViolatedSUs.insert(UsedSUs.pop_back_val());

      return false;
    }

    if (RemainFanins == 0 || RemainFanins <= AvailableFanins)
      return true;

    // Subtract the used fanins from the available fanins.
    AvailableFanins -= UsedFanins;
    // We get extra fanins as we go to next level.
    AvailableFanins *= MaxFIPerLevel;
    return preserveFaninConstraint(AvailableFanins, CurLevel + 1, SUs);
  }

  bool hasExtraSlack(VASTSchedUnit *SU, unsigned Level) const {
    SlackMapTy::const_iterator J = SlackMap.find(SU);
    assert(J != SlackMap.end() && "SU not in the map?");
    const SrcSlackMapTy &SrcSlacks = J->second;

    typedef SrcSlackMapTy::const_iterator iterator;
    for (iterator I = SrcSlacks.begin(), E = SrcSlacks.end(); I != E; ++I)
      // There is no extra slack if there is any fanin do not have extra slack.
      if (I->second <= Level)
        return false;

    return true;
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
    bool IsDstVoilatedSlackConstraint = ViolatedSUs.count(Dst);

    for (iterator I = SrcSlacks.begin(), E = SrcSlacks.end(); I != E; ++I) {
      VASTSchedUnit *Src = I->first;
      unsigned Slack = I->second;

      unsigned Latency = Dst->getDFLatency(Src);
      unsigned AverageMUXLevel = (Log2_32_Ceil(Sel->numNonTrivialFanins()) - 1)
                                 / Log2_32_Ceil(MaxFIPerLevel) + 1;
      unsigned ExpectedSlack = unsigned(Latency) + Slack + 1;

      double CurrentSlackRatio = double(Slack + 1) / double(AverageMUXLevel);
      double CurPenalty = double(PenaltyFactor) * double(AverageMUXLevel) /
                          pow(double(MaxFIPerLevel), int(Slack + 1));

      if (IsDstVoilatedSlackConstraint)
        CurPenalty *= 2.0;
      else
        CurPenalty /= 2.0;

      // Do not add the soft constraint if its penalty is too small
      if (CurPenalty < 1e-2)
        continue;

      SoftConstraint &SC = Scheduler.getOrCreateSoftConstraint(Src, Dst);

      SC.Penalty += CurPenalty;
      SC.C = ExpectedSlack;
    }
  }
};

struct ItetrativeEngine {
  SDCScheduler Scheduler;
  PreSchedBinding &PSB;
  DominatorTree &DT;
  BlockFrequencyInfo &BFI;
  BranchProbabilityInfo &BPI;

  IR2SUMapTy &IR2SUMap;

  enum State {
    Initial, Scheduling, Binding
  };

  State S;
  unsigned ScheduleViolation, BindingViolation;
  float TotalWeight;
  const float PerformanceFactor, ResourceFactor;

  ItetrativeEngine(VASTSchedGraph &G, PreSchedBinding &PSB,
                   DominatorTree &DT, BlockFrequencyInfo &BFI,
                   BranchProbabilityInfo &BPI, IR2SUMapTy &IR2SUMap)
    : Scheduler(G, 1), PSB(PSB), DT(DT), BFI(BFI), BPI(BPI), IR2SUMap(IR2SUMap),
      S(Initial), ScheduleViolation(0), BindingViolation(0), TotalWeight(0.0),
      PerformanceFactor(8.0f), ResourceFactor(0.1f) {
    // Build the hard linear order.
    Scheduler.addLinOrdEdge(DT, IR2SUMap);
  }

  bool checkCompatibility() {
    bool AnyViolation = false;
    typedef CompGraphBase::cluster_iterator cluster_iterator;
    for (cluster_iterator I = PSB->cluster_begin(), E = PSB->cluster_end();
         I != E; ++I) {
      AnyViolation |= checkCompatibility(*I);
    }

    return AnyViolation;
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

  typedef CompGraphBase::ClusterType ClusterType;
  bool checkCompatibility(const ClusterType &Cluster);
  unsigned checkCompatibility(PSBCompNode *Src, SUArrayRef SrcSUs,
                              PSBCompNode *Dst, SUArrayRef DstSUs);

  typedef SmallVector<std::pair<VASTSchedUnit*, VASTSchedUnit*>, 8>
          ViolationPairs;
  unsigned
  checkLatchCompatibility(PSBCompNode *Src, PSBCompNode *Dst, SUArrayRef DstSUs,
                          ViolationPairs &Violations);

  void flushViolations(ViolationPairs &Violations, float Penalty) {
    while (!Violations.empty()) {
      VASTSchedUnit *Src = Violations.back().first,
                    *Dst = Violations.back().second;
      Violations.pop_back();

      updateSchedulingConstraint(Src, Dst, 0, Penalty);
    }
  }

  void updateBindingCost(PSBCompNode *Src, PSBCompNode *Dst, float Cost) {
    if (S != Binding)
      return;

    Src->increaseSchedulingCost(Dst, Cost);
    ++BindingViolation;
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

  bool iterate(VASTModule &VM) {
    ScheduleViolation = BindingViolation = 0;
    // First of all, perform the scheduling.
    if (!performScheduling(VM))
      return false;

    // Perfrom binding after scheduing.
    S = Binding;

    // Check the bindings that are invalided by the scheduler.
    if (checkCompatibility()) {
      // And bind again.
      PSB->performBinding();
      // Perform scheduling after binding.
      S = Scheduling;
      return true;
    }

    return false;
  }

  bool performSchedulingAndAllocateMuxSlack(VASTModule &VM) {
    if (!performScheduling(VM))
      return false;

    float NumBBs = VM.getLLVMFunction().getBasicBlockList().size();

    bool FaninConstraintsViolated = false;
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
      FaninConstraintsViolated = true;
    }

    return FaninConstraintsViolated;
  }
};
}

unsigned
ItetrativeEngine::checkLatchCompatibility(PSBCompNode *Src,
                                          PSBCompNode *Dst,
                                          SUArrayRef DstSUs,
                                          ViolationPairs &Violations) {
  unsigned NegativeSlack = 0;

  typedef PSBCompNode::kill_iterator kill_iterator;
  for (kill_iterator I = Src->kill_begin(), E = Src->kill_end(); I != E; ++I) {
    VASTSchedUnit *SrcKill = LookupSU(*I, IR2SUMap);

    for (unsigned i = 0; i < DstSUs.size(); ++i) {
      VASTSchedUnit *DstDef = DstSUs[i];
      if (!DstDef->isLatch())
        continue;

      // FIXME: Test dominance after global code motion is enabled.
      if (DstDef->getParent() != SrcKill->getParent())
        continue;

      if (SrcKill->getSchedule() <= DstDef->getSchedule())
        continue;

      DEBUG(dbgs() << "Latch SU not compatible:\n";
      SrcKill->dump();
      DstDef->dump();
      dbgs() << "\n\n");

      Violations.push_back(std::make_pair(SrcKill, DstDef));
      NegativeSlack += SrcKill->getSchedule() - DstDef->getSchedule();
    }
  }

  return NegativeSlack;
}

unsigned ItetrativeEngine::checkCompatibility(PSBCompNode *Src,
                                                        SUArrayRef SrcSUs,
                                                        PSBCompNode *Dst,
                                                        SUArrayRef DstSUs) {
  assert(Src->Inst.IsLauch() == Dst->Inst.IsLauch()
         && "Unexpected binding launch/latch to the same physical unit!");
  // Get the benefit by getting the negative of the cost.
  float BindingBenefit = PSB->getEntry()->getCostTo(Dst).InterconnectCost -
                         Src->getCostTo(Dst).InterconnectCost;

  float Penalty = BindingBenefit * ResourceFactor * TotalWeight;

  if (!Src->Inst.IsLauch()) {
    // For latch (that define a variable), the conflict only introduced by
    // scheduling when their are in the same subtree of the dominator tree.
    // Otherwise their live interval will never overlap, because the use of a
    // variable are all dominated by the define, hence the live interval rooted
    // on two node without dominance relationship will never overlap.
    BasicBlock *SrcBlock = Src->getDomBlock(), *DstBlock = Dst->getDomBlock();
    if (SrcBlock == DstBlock) {
      ViolationPairs Src2DstSPairs;
      unsigned Src2DstSlack
        = checkLatchCompatibility(Src, Dst, DstSUs, Src2DstSPairs);
      ViolationPairs Dst2SrcPairs;
      unsigned Dst2SrcSlack
        = checkLatchCompatibility(Dst, Src, SrcSUs, Dst2SrcPairs);

      if (Src2DstSlack && Dst2SrcSlack) {
        if (Src2DstSlack < Dst2SrcSlack) {
          flushViolations(Src2DstSPairs, Penalty);
          return Src2DstSlack;
        }

        flushViolations(Dst2SrcPairs, Penalty);
        return Dst2SrcSlack;
      }

      return 0;
    }

    ViolationPairs Pairs;
    if (DT.dominates(Src->getDomBlock(), Dst->getDomBlock())) {
      if (unsigned Slack = checkLatchCompatibility(Src, Dst, DstSUs, Pairs)) {
        flushViolations(Pairs, Penalty);
        return Slack;
      }
    }

    if (DT.dominates(Dst->getDomBlock(), Src->getDomBlock())) {
      if (unsigned Slack = checkLatchCompatibility(Dst, Src, SrcSUs, Pairs)) {
        flushViolations(Pairs, Penalty);
        return Slack;
      }
    }

    return 0;
  }

  VASTSchedUnit *SrcSU = getLaunch(SrcSUs), *DstSU = getLaunch(DstSUs);
  // The execution time will never overlap if they are located in different BB
  // FIXME: Not true alter global code motion is enabled.
  if (SrcSU->getParent() != DstSU->getParent())
    return 0;

  // Src and Dst is compatible if Dst is scheduled after Src is finished.
  if (SrcSU->getSchedule() + SrcSU->getII() <= DstSU->getSchedule())
    return 0;

  // Or Src is scheduled after Dst is finished.
  if (SrcSU->getSchedule() >= DstSU->getSchedule() + DstSU->getII())
    return 0;
  
  DEBUG(dbgs() << "Launch SU not compatible:\n";
  SrcSU->dump();
  DstSU->dump();
  dbgs() << "\n\n");

  // Apply the constraint in the same way that we assign the linear order.
  if (!alap_less(SrcSU, SrcSU))
    std::swap(SrcSU, SrcSU);

  updateSchedulingConstraint(SrcSU, DstSU, SrcSU->getII(), Penalty);

  return SrcSU->getII();
}

bool
ItetrativeEngine::checkCompatibility(const ClusterType &Cluster) {
  unsigned AnyViolation = false;

  for (unsigned i = 0; i < Cluster.size(); ++i) {
    PSBCompNode *Src = static_cast<PSBCompNode*>(Cluster[i]);
    ArrayRef<VASTSchedUnit*> SrcSUs(IR2SUMap[Src->Inst]);

    // Also check the compatibility between Src and its descendant
    for (unsigned j = i + 1; j < Cluster.size(); ++j)  {
      PSBCompNode *Desc = static_cast<PSBCompNode*>(Cluster[j]);
      ArrayRef<VASTSchedUnit*> DescSUs(IR2SUMap[Desc->Inst]);

      unsigned NegativeSlack = checkCompatibility(Src, SrcSUs, Desc, DescSUs);
      if (NegativeSlack == 0)
        continue;

      AnyViolation |= true;
      // Prevent the this descendant from being bind to the same physical unit
      // with its ancestor. Essentially, we want to separate this node from the
      // cluster.
      for (unsigned k = i; k < j; ++k)  {
        PSBCompNode *Ancestor = static_cast<PSBCompNode*>(Cluster[k]);
        float Penalty = (PerformanceFactor * NegativeSlack) / (j - k);
        updateBindingCost(Ancestor, Desc, Penalty);
      }
    }
  }

  return AnyViolation;
}

bool ItetrativeEngine::performScheduling(VASTModule &VM) {
  // No need to modify the scheduling if the FU compatiblity is preserved.
  //if (S == Scheduling && !checkCompatibility())
  //  return false;

  Scheduler->resetSchedule();

  if (!Scheduler.createLPAndVariables())
    return false;

  TotalWeight = 0.0f;

  Function &F = VM.getLLVMFunction();
  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    DEBUG(dbgs() << "Applying constraints to BB: " << BB->getName() << '\n');
    // Get the frequency of the block, and ensure the frequency always bigger
    // than 0.
    BlockFrequency BF = std::max(BFI.getBlockFreq(BB), BlockFrequency(1));

    float ExitWeigthSum = 0;
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
      Scheduler.addObjectCoeff(BBExit, - 1.0 * ExitWeight);
      DEBUG(dbgs().indent(4) << "Setting Exit Weight: " << ExitWeight
                              << ' ' << BP << '\n');

      ExitWeigthSum += ExitWeight;
      TotalWeight += ExitWeight;
    }

    ArrayRef<VASTSchedUnit*> SUs(IR2SUMap[BB]);
    VASTSchedUnit *BBEntry = 0;

    // First of all we need to locate the header.
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *SU = SUs[i];
      if (SU->isBBEntry()) {
        BBEntry = SU;
        break;
      }
    }

    assert(BBEntry && "Cannot find BB Entry!");

    assert(ExitWeigthSum && "Unexpected zero weight!");
    Scheduler.addObjectCoeff(BBEntry, ExitWeigthSum);
    DEBUG(dbgs().indent(2) << "Setting Entry Weight: "
                            << ExitWeigthSum << '\n');
  }

  Scheduler.addObjectCoeff(Scheduler->getExit(), - 1.0 * (TotalWeight /*+ PerformanceFactor*/));

  //Scheduler.buildOptSlackObject(1.0);

  bool success = Scheduler.schedule();
  assert(success && "SDCScheduler fail!");

  // We had made some changes.
  return success;
}

void VASTScheduling::scheduleGlobal() {
  PreSchedBinding &PSB = getAnalysis<PreSchedBinding>();
  BranchProbabilityInfo &BPI = getAnalysis<BranchProbabilityInfo>();
  BlockFrequencyInfo &BFI = getAnalysis<BlockFrequencyInfo>();

  ItetrativeEngine ISB(*G, PSB, *DT, BFI, BPI, IR2SUMap);
  while (ISB.performSchedulingAndAllocateMuxSlack(*VM)) {
    ++NumIterations;
    dbgs() << "Schedule Violations: " << ISB.ScheduleViolation << ' '
           << "Binding Violations:" << ISB.BindingViolation << '\n';
  }

  DEBUG(G->viewGraph());
}