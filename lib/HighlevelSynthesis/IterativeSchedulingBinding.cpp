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

namespace {
struct ItetrativeEngine {
  SDCScheduler Scheduler;
  PreSchedBinding &PSB;
  DominatorTree &DT;
  BlockFrequencyInfo &BFI;
  BranchProbabilityInfo &BPI;

  typedef ArrayRef<VASTSchedUnit*> SUArrayRef;
  typedef std::map<Value*, SmallVector<VASTSchedUnit*, 4> > IR2SUMapTy;
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

  VASTSchedUnit *lookupSU(VASTSeqOp *Op) {
    Value *V = Op->getValue();
    assert(V && "Unexpected virtual node!");
    SUArrayRef SUs(IR2SUMap[V]);

    assert(SUs.size() && "Corresponding scheduling units not found!");
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *SU = SUs[i];
      if (SU->getSeqOp() == Op)
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
    VASTSchedUnit *SrcKill = lookupSU(*I);

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
  if (S == Scheduling && !checkCompatibility())
    return false;

  Scheduler->resetSchedule();

  if (S == Initial) {
    // Build the step variables, and no need to schedule at all if all SUs have
    // been scheduled.
    if (!Scheduler.createLPAndVariables())
      return false;

    Scheduler.addDependencyConstraints();
  }

  TotalWeight = 0.0f;

  Function &F = VM.getLLVMFunction();
  typedef Function::iterator iterator;
  for (iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    DEBUG(dbgs() << "Applying constraints to BB: " << BB->getName() << '\n');
    //BlockFrequency BF = BFI.getBlockFreq(BB);

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

      // BlockFrequency CurBF = BF * BP;
      float ExitWeight = (PerformanceFactor * BP.getNumerator()) / BP.getDenominator();
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

  Scheduler.addSoftConstraints();
  Scheduler.buildOptSlackObject(1.0);

  bool success = Scheduler.schedule();
  assert(success && "SDCScheduler fail!");

  // We had made some changes.
  return success;
}

void VASTScheduling::scheduleGlobal() {
  PreSchedBinding &PSB = getAnalysis<PreSchedBinding>();
  BranchProbabilityInfo &BPI = getAnalysis<BranchProbabilityInfo>();
  BlockFrequencyInfo &BFI = *reinterpret_cast<BlockFrequencyInfo*>(0);
  //getAnalysis<BlockFrequencyInfo>();

  ItetrativeEngine ISB(*G, PSB, *DT, BFI, BPI, IR2SUMap);
  while (ISB.iterate(*VM)) {
    ++NumIterations;
    dbgs() << "Schedule Violations: " << ISB.ScheduleViolation << ' '
                 << "Binding Violations:" << ISB.BindingViolation << '\n';
  }

  DEBUG(G->viewGraph());
}