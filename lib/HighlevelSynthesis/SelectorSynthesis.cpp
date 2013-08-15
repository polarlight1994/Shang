//===- SelectorSynthesis.cpp - Implement the Fanin Mux for Regs -*- C++ -*-===//
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

#include "Dataflow.h"

#include "shang/FUInfo.h"
#include "shang/VASTMemoryPort.h"
#include "shang/VASTExprBuilder.h"
#include "shang/STGDistances.h"

#include "shang/Strash.h"
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
STATISTIC(NumTrivialConesMerged, "Number of trivial cones merged");
STATISTIC(NumConesMerged, "Number of cones merged");

namespace {
typedef std::set<VASTSeqValue*> SVSet;
typedef float delay_type;
typedef DenseMap<DataflowValue, float> ArrivalTimeSet;

class TimedCone {
public:
  struct Leaf {
    unsigned NumCycles;
    float CriticalDelay;
    Leaf(float CriticalDelay = 0.0f, unsigned NumCycles = STGDistances::Inf)
      : NumCycles(NumCycles), CriticalDelay(CriticalDelay) {}

    // Update the source information with tighter cycles constraint and larger
    // critical delay
    void update(unsigned NumCycles, float CriticalDelay) {
      this->NumCycles = std::min(this->NumCycles, NumCycles);
      this->CriticalDelay = std::max(this->CriticalDelay, CriticalDelay);
    }

    void update(const Leaf &RHS) {
      update(RHS.NumCycles, RHS.CriticalDelay);
    }
  };

  typedef DenseMap<VASTSeqValue*, Leaf> LeafSetTy;
private:
  LeafSetTy Leaves;

  std::set<VASTSlot*> Slots;

  void updateIntervals(VASTSlot *S, STGDistances *STGDist) {
    // Update the available interval.
    for (LeafSetTy::iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I) {
      VASTSeqValue *SV = I->first;
      unsigned Interval = STGDist->getIntervalFromDef(SV, S);
      I->second.update(Interval, I->second.CriticalDelay);
    }
  }
public:
  void buildCone(SVSet &SVs, const ArrivalTimeSet &ALT) {
    for (SVSet::iterator I = SVs.begin(), E = SVs.end(); I != E; ++I) {
      VASTSeqValue *SV = *I;
      Leaves[SV].update(STGDistances::Inf, ALT.lookup(DataflowValue(SV)));
    }
  }

  typedef LeafSetTy::const_iterator iterator;
  iterator begin() const { return Leaves.begin(); }
  iterator end()   const { return Leaves.end(); }

  void addSlot(VASTSlot *S, STGDistances *STGDist) {
    if (!Slots.insert(S).second) return;

    updateIntervals(S, STGDist);
  }

  Leaf getLeaf(VASTSeqValue *SV) const {
    return Leaves.lookup(SV);
  }

  typedef std::set<VASTSlot*>::const_iterator slot_iterator;
  slot_iterator slot_begin() const { return Slots.begin(); }
  slot_iterator slot_end() const { return Slots.end(); }

  void mergeSlots(const TimedCone *LHS, STGDistances *STGDist) {
    for (slot_iterator I = LHS->slot_begin(), E = LHS->slot_end(); I != E; ++I)
      addSlot(*I, STGDist);
  }

  void merge(const TimedCone *LHS, STGDistances *STGDist) {
    for (iterator I = LHS->begin(), E = LHS->end(); I != E; ++I)
      Leaves[I->first].update(I->second);

    Slots.insert(LHS->slot_begin(), LHS->slot_end());

    // Recalculate the intervals.
    for (slot_iterator I = slot_begin(), E = slot_end(); I != E; ++I)
      updateIntervals(*I, STGDist);
  }

  bool hasTighterConstraints(const TimedCone *LHS) const {
    for (iterator I = LHS->begin(), E = LHS->end(); I != E; ++I) {
      Leaf CurLeaf = getLeaf(I->first);

      // If the current cone have fewer available cycles or have bigger critical
      // delay, there is a tighter constraint.
      if (CurLeaf.NumCycles < I->second.NumCycles
          || CurLeaf.CriticalDelay > I->second.CriticalDelay) {
        unsigned MinCycles = std::min(CurLeaf.NumCycles, I->second.NumCycles);
        delay_type CriticalDelay
          = std::max(CurLeaf.CriticalDelay, I->second.CriticalDelay);

        // FIXME: Allow user to specify the ratio.
        // Temporary choose 2 because the (unpredictable) wire delay usually
        // take 50% of the total delay. Here we make sure even though we merge
        // the fanin, we still have 50% slack for the wire delay.
        if (CriticalDelay * 2 > MinCycles)
          return true;
      }
    }

    return false;
  }

  bool isSingleCycleCone() const {
    for (iterator I = begin(), E = end(); I != E; ++I)
      if (I->second.NumCycles > 1)
        return false;

    return true;
  }
};

typedef std::map<VASTValPtr, TimedCone*> FIConeVec;

struct TimingDrivenSelectorSynthesis : public VASTModulePass {
  CachedStrashTable *CST;
  Dataflow *DF;
  STGDistances *STGDist;
  VASTExprBuilder *Builder;
  VASTModule *VM;

  // The delay and available cycles for the combinational cones.
  DenseMap<VASTValue*, TimedCone*> TimedCones;

  TimedCone *getOrCreateTimedCone(VASTValPtr V, const ArrivalTimeSet &ALT) {
    if (TimedCone *C = TimedCones.lookup(V.get()))
      return C;

    SVSet Leaves;

    // Ignore the trivial nodes.
    if (!V->extractSupportingSeqVal(Leaves, true))
      return 0;

    TimedCone *&TC = TimedCones[V.get()];
    TC = new TimedCone();
    if (ALT.empty())
      TC->buildCone(Leaves, ALT);
    return TC;
  }

  static char ID;

  TimingDrivenSelectorSynthesis() : VASTModulePass(ID), CST(0), DF(0), STGDist(0) {
    initializeTimingDrivenSelectorSynthesisPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<Dataflow>();
    AU.addRequired<CachedStrashTable>();
    AU.addRequired<STGDistances>();
    AU.addPreserved<STGDistances>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
  }

  bool runOnVASTModule(VASTModule &VM);

  void mergeTrivialCones(FIConeVec &Cones, VASTExprBuilder &Builder,
                         unsigned BitWdith);
  bool mergeCones(FIConeVec &Cones, VASTExprBuilder &Builder,
                  unsigned BitWdith);
  bool isGoodToMerge(TimedCone *LHS, TimedCone *RHS) const;
  void synthesizeSelector(VASTSelector *Sel, VASTExprBuilder &Builder);

  void releaseMemory() {
    DeleteContainerSeconds(TimedCones);
  }
};
}

INITIALIZE_PASS_BEGIN(TimingDrivenSelectorSynthesis,
                      "timing-driven-selector-synthesis",
                      "Implement the MUX for the Sequantal Logic", false, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
INITIALIZE_PASS_END(TimingDrivenSelectorSynthesis,
                    "timing-driven-selector-synthesis",
                    "Implement the MUX for the Sequantal Logic", false, true)

char TimingDrivenSelectorSynthesis::ID = 0;

char &llvm::TimingDrivenSelectorSynthesisID = TimingDrivenSelectorSynthesis::ID;

static bool intersect(const std::set<VASTSelector*> &LHS,
                      const std::set<VASTSeqValue*> &RHS) {
  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = RHS.begin(), E = RHS.end(); I != E; ++I)
    if (LHS.count((*I)->getSelector()))
      return true;

  return false;
}

void TimingDrivenSelectorSynthesis::mergeTrivialCones(FIConeVec &Cones,
                                                      VASTExprBuilder &Builder,
                                                      unsigned Bitwidth) {
  // Nothing to merge.
  if (Cones.size() == 1) return;

  std::vector<VASTValPtr> TrivialFIs;

  for (FIConeVec::iterator I = Cones.begin(), E = Cones.end(); I != E; ++I) {
    if (I->second) continue;

    TrivialFIs.push_back(I->first);
    ++NumTrivialConesMerged;
  }

  // Return if nothing to merge.
  if (TrivialFIs.empty()) return;

  // Combine the trivial cones by OR expression.
  VASTValPtr MergedFI = Builder.buildOrExpr(TrivialFIs, Bitwidth);

  // Erase the merged cones.
  while (!TrivialFIs.empty()) {
    Cones.erase(TrivialFIs.back());
    TrivialFIs.pop_back();
  }

  // Add the newly created fanin.
  Cones[MergedFI] = 0;
}

bool TimingDrivenSelectorSynthesis::isGoodToMerge(TimedCone *LHS,
                                                  TimedCone *RHS) const {
  // It is always good to merge to trivial cones.
  if (!LHS || !RHS) return true;

  TimedCone NewTC;

  NewTC.merge(LHS, STGDist);
  NewTC.merge(RHS, STGDist);

  if (NewTC.hasTighterConstraints(LHS) || NewTC.hasTighterConstraints(RHS))
    return false;

  return true;
}

bool TimingDrivenSelectorSynthesis::mergeCones(FIConeVec &Cones,
                                               VASTExprBuilder &Builder,
                                               unsigned BitWdith) {
  // Nothing to merge.
  if (Cones.size() == 1) return false;

  std::vector<std::pair<VASTValPtr, TimedCone*> > FIs(Cones.begin(), Cones.end());

  bool AnyChange = false;

  for (unsigned i = 0, e = FIs.size(); i < e; ++i) {
    VASTValPtr FaninI = FIs[i].first;

    // Do not merge the same fanin twice.
    if (!FaninI)
      continue;

    TimedCone *TCI = FIs[i].second;

    for (unsigned j = i + 1; j < e; ++j) {
      VASTValPtr FaninJ = FIs[j].first;

      // Do not merge the same fanin twice.
      if (!FaninJ)
        continue;

      TimedCone *TCJ = FIs[j].second;

      if (!isGoodToMerge(TCI, TCJ)) continue;

      // Merge the FI values.
      VASTValPtr NewFI = Builder.buildOrExpr(FaninI, FaninJ, BitWdith);

      // Merge the cones.
      // FIXME: Use the cone returned by "isGoodToMerge"
      TimedCone *TC = getOrCreateTimedCone(NewFI, ArrivalTimeSet());
      if (TCI)
        TC->merge(TCI, STGDist);
      if (TCJ)
        TC->merge(TCJ, STGDist);

      // Modify Cone vector, so that we do not merge the same cone twice.
      Cones.erase(FaninI);
      Cones.erase(FaninJ);
      bool Inserted = Cones.insert(std::make_pair(NewFI, TC)).second;
      assert(Inserted && "Fanin had already existed?");

      FaninI = NewFI;
      TCI = TC;
      FIs[j].first = VASTValPtr();

      AnyChange = true;
      ++NumConesMerged;
    }
  }

  return AnyChange;
}

static void
ApplyKeepAttribute(VASTSelector *Sel, MutableArrayRef<VASTValPtr> Values,
                   ArrayRef<VASTSlot*> Slots, VASTExprBuilder &Builder) {
  std::set<VASTSeqValue*> CurLeaves;
  std::set<VASTSelector*> AllLeaves, IntersectLeaves;
  typedef std::set<VASTSeqValue*>::iterator leaf_iterator;

  for (unsigned i = 0, e = Values.size(); i != e; ++i) {
    VASTValPtr V = Values[i];

    CurLeaves.clear();
    V->extractSupportingSeqVal(CurLeaves, true);

    for (leaf_iterator LI = CurLeaves.begin(), LE = CurLeaves.end();
         LI != LE; ++LI) {
      VASTSeqValue *SV = *LI;
      if (!AllLeaves.insert(SV->getSelector()).second &&
          !SV->isSlot() && !SV->isFUOutput())
        IntersectLeaves.insert(SV->getSelector());
    }
  }

  for (unsigned i = 0, e = Values.size(); i != e; ++i) {
    VASTValPtr &V = Values[i];
    CurLeaves.clear();
    V->extractSupportingSeqVal(CurLeaves, true);

    if (!intersect(IntersectLeaves, CurLeaves))
      continue;

    V = Builder.buildKeep(V);

    // The guarding condition itself is not guard, that is, the guarding
    // condition is read whenever the slot register is set. Hence, we should
    // annotate it with the control-equivalent group instead of the guarding
    // condition equivalent group!
    // FIXME: We can build apply the keep attribute according to the STG
    // subgroup hierarchy sequentially to relax the constraints.
    Sel->annotateReadSlot(Slots[i]->getParentState(), Values[i]);
  }
}

void
TimingDrivenSelectorSynthesis::synthesizeSelector(VASTSelector *Sel,
                                                  VASTExprBuilder &Builder) {
  typedef std::vector<VASTLatch> OrVec;
  typedef std::map<unsigned, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator iterator;

  CSEMapTy CSEMap;

  for (VASTSelector::const_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U)) continue;

    // Index the input of the assignment based on the strash id. By doing this
    // we can pack the structural identical inputs together.
    CSEMap[CST->getOrCreateStrashID(VASTValPtr(U))].push_back(U);
  }

  if (CSEMap.empty()) return;

  unsigned Bitwidth = Sel->getBitWidth();

  SmallVector<VASTValPtr, 4> SlotGuards, SlotFanins, FaninGuards;
  SmallVector<VASTSlot*, 4> Slots;

  FIConeVec FICones;

  ArrivalTimeSet EstimatedDelays;
  std::set<VASTSeqValue*> CurLeaves;
  typedef std::set<VASTSeqValue*>::iterator leaf_iterator;

  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    SlotGuards.clear();
    SlotFanins.clear();
    EstimatedDelays.clear();

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      SmallVector<VASTValPtr, 2> CurGuards;
      const VASTLatch &L = *OI;
      SlotFanins.push_back(L);
      VASTSlot *S = L.getSlot();

      if (VASTValPtr SlotActive = L.getSlotActive())
        CurGuards.push_back(SlotActive);

      CurGuards.push_back(L.getGuard());
      VASTValPtr CurGuard = Builder.buildAndExpr(CurGuards, 1);

      SlotGuards.push_back(CurGuard);
      Slots.push_back(S);

      // Collect all leaves for current fanin.
      CurLeaves.clear();
      L->extractSupportingSeqVal(CurLeaves);
      L.getGuard()->extractSupportingSeqVal(CurLeaves);


      for (leaf_iterator LI = CurLeaves.begin(), LE = CurLeaves.end();
           LI != LE; ++LI) {
        VASTSeqValue *SV = *LI;

        // Get the delay from last generation.
        float delay = DF->getDelay(SV, L.Op, L.getSlot());

        if (delay == 0.0f)
          continue;

        float &OldDelay = EstimatedDelays[SV];
        OldDelay = std::max(OldDelay, delay);
      }
    }

    ApplyKeepAttribute(Sel, SlotGuards, Slots, Builder);

    VASTValPtr FIGuard = Builder.buildOrExpr(SlotGuards, 1);
    FaninGuards.push_back(FIGuard);

    // No need to build the fanin for the enables, only the guard is used by
    // them.
    if (Sel->isEnable() || Sel->isSlot())
      continue;

    VASTValPtr FIMask = Builder.buildBitRepeat(FIGuard, Bitwidth);
    VASTValPtr FIVal = Builder.buildAndExpr(SlotFanins, Bitwidth);
    VASTValPtr GuardedFIVal = Builder.buildAndExpr(FIVal, FIMask, Bitwidth);

    TimedCone *C = getOrCreateTimedCone(GuardedFIVal, EstimatedDelays);
    FICones[GuardedFIVal] = C;

    // Ignore the trivial cones.
    if (C == 0) continue;

    while (!Slots.empty())
      C->addSlot(Slots.pop_back_val(), STGDist);
  }

  mergeTrivialCones(FICones, Builder, Bitwidth);
  while (mergeCones(FICones, Builder, Bitwidth))
    ;

  SmallVector<VASTValPtr, 4> Fanins;
  typedef std::map<VASTValPtr, TimedCone*>::iterator cone_iterator;
  for (cone_iterator I = FICones.begin(), E = FICones.end(); I != E; ++I) {
    VASTValPtr FI = I->first;
    TimedCone *TC = I->second;
    bool KeepApplied = false;

    // Apply the keep attribute if necessary.
    if (FICones.size() > 1 && TC && !TC->isSingleCycleCone()) {
      FI = Builder.buildKeep(FI);
      KeepApplied = true;
    }

    // Remember the fanin, we are going to OR them together.
    Fanins.push_back(FI);

    // Ignore the trivial cones.
    if (!TC)
      continue;

    if (KeepApplied) {
      typedef TimedCone::slot_iterator iterator;
      for (iterator SI = TC->slot_begin(), SE = TC->slot_end(); SI != SE; ++SI)
        Sel->annotateReadSlot(*SI, FI);
    }
  }

  // Build the final fanin only if the selector is not enable.
  VASTValPtr FI = VASTImmediate::True;
  if (!Sel->isEnable() && !Sel->isSlot())
    FI = Builder.buildOrExpr(Fanins, Bitwidth);

  Sel->setMux(FI, Builder.buildOrExpr(FaninGuards, 1));

  DeleteContainerSeconds(TimedCones);
}

bool TimingDrivenSelectorSynthesis::runOnVASTModule(VASTModule &VM) {
  STGDist = &getAnalysis<STGDistances>();
  DF = &getAnalysis<Dataflow>();
  CST = &getAnalysis<CachedStrashTable>();

  {
    MinimalExprBuilderContext Context(VM);
    Builder = new VASTExprBuilder(Context);
    this->VM = &VM;

    typedef VASTModule::selector_iterator iterator;

    // Eliminate the identical SeqOps.
    for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
      synthesizeSelector(I, *Builder);

    delete Builder;
  }

  // Perform LUT Mapping on the newly synthesized selectors.
  VASTModulePass *P = static_cast<VASTModulePass*>(createLUTMappingPass());
  AnalysisResolver *AR = new AnalysisResolver(*getResolver());
  P->setResolver(AR);
  P->runOnVASTModule(VM);
  delete P;

  return true;
}

namespace {
struct SimpleSelectorSynthesis : public VASTModulePass {
  CachedStrashTable *CST;
  VASTExprBuilder *Builder;
  VASTModule *VM;

  static char ID;

  SimpleSelectorSynthesis() : VASTModulePass(ID), CST(0) {
    initializeSimpleSelectorSynthesisPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<CachedStrashTable>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
  }

  bool runOnVASTModule(VASTModule &VM);

  void synthesizeSelector(VASTSelector *Sel, VASTExprBuilder &Builder);

  void releaseMemory() {}
};
}


INITIALIZE_PASS_BEGIN(SimpleSelectorSynthesis,
                      "simple-selector-synthesis",
                      "Implement the MUX for the Sequantal Logic", false, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
INITIALIZE_PASS_END(SimpleSelectorSynthesis,
                    "simple-selector-synthesis",
                    "Implement the MUX for the Sequantal Logic", false, true)

char SimpleSelectorSynthesis::ID = 0;

char &llvm::SimpleSelectorSynthesisID = SimpleSelectorSynthesis::ID;

bool SimpleSelectorSynthesis::runOnVASTModule(VASTModule &VM) {
  CST = &getAnalysis<CachedStrashTable>();

  {
    MinimalExprBuilderContext Context(VM);
    Builder = new VASTExprBuilder(Context);
    this->VM = &VM;

    typedef VASTModule::selector_iterator iterator;

    // Eliminate the identical SeqOps.
    for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I)
      synthesizeSelector(I, *Builder);

    delete Builder;
  }

  // Perform LUT Mapping on the newly synthesized selectors.
  VASTModulePass *P = static_cast<VASTModulePass*>(createLUTMappingPass());
  AnalysisResolver *AR = new AnalysisResolver(*getResolver());
  P->setResolver(AR);
  P->runOnVASTModule(VM);
  delete P;

  return true;
}

void SimpleSelectorSynthesis::synthesizeSelector(VASTSelector *Sel,
                                                 VASTExprBuilder &Builder) {
  typedef std::vector<VASTLatch> OrVec;
  typedef std::map<unsigned, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator iterator;

  CSEMapTy CSEMap;
  std::set<VASTSeqValue*> CurLeaves;
  std::set<VASTSelector*> AllLeaves, IntersectLeaves;
  typedef std::set<VASTSeqValue*>::iterator leaf_iterator;

  for (VASTSelector::const_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U)) continue;

    CurLeaves.clear();
    VASTValPtr FI = U;
    FI->extractSupportingSeqVal(CurLeaves);
    U.getGuard()->extractSupportingSeqVal(CurLeaves);

    for (leaf_iterator LI = CurLeaves.begin(), LE = CurLeaves.end();
         LI != LE; ++LI) {
      VASTSeqValue *Leaf = *LI;
      if (!AllLeaves.insert(Leaf->getSelector()).second &&
          !Leaf->isSlot() && !Leaf->isFUOutput())
        IntersectLeaves.insert(Leaf->getSelector());
    }

    // Index the input of the assignment based on the strash id. By doing this
    // we can pack the structural identical inputs together.
    CSEMap[CST->getOrCreateStrashID(FI)].push_back(U);
  }

  if (CSEMap.empty()) return;

  unsigned Bitwidth = Sel->getBitWidth();

  SmallVector<VASTValPtr, 4> SlotGuards, SlotFanins, FaninGuards, AllFanins;
  SmallVector<VASTSlot*, 4> Slots;

  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    SlotGuards.clear();
    SlotFanins.clear();

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      SmallVector<VASTValPtr, 2> CurGuards;
      const VASTLatch &L = *OI;
      SlotFanins.push_back(L);
      VASTSlot *S = L.getSlot();

      if (VASTValPtr SlotActive = L.getSlotActive())
        CurGuards.push_back(SlotActive);

      CurGuards.push_back(L.getGuard());
      VASTValPtr CurGuard = Builder.buildAndExpr(CurGuards, 1);

      // Simply keep all guarding condition, because they are only 1 bit nodes,
      // and their upper bound is the product of number of slots and number of
      // basic blocks.
      CurGuard = Builder.buildKeep(CurGuard);

      // The guarding condition itself is not guard, that is, the guarding
      // condition is read whenever the slot register is set. Hence, we should
      // annotate it with the control-equivalent group instead of the guarding
      // condition equivalent group!
      // FIXME: We can build apply the keep attribute according to the STG
      // subgroup hierarchy sequentially to relax the constraints.
      Sel->annotateReadSlot(S->getParentState(), CurGuard);
      // Also annotate S, so that we can construct the annoation to VASTSeqOp
      // mapping based on the slot.
      Sel->annotateReadSlot(S, CurGuard);
      SlotGuards.push_back(CurGuard);
      Slots.push_back(S);
    }

    VASTValPtr FIGuard = Builder.buildOrExpr(SlotGuards, 1);
    FaninGuards.push_back(FIGuard);

    // No need to build the fanin for the enables, only the guard is used by
    // them.
    if (Sel->isEnable() || Sel->isSlot())
      continue;

    VASTValPtr FIMask = Builder.buildBitRepeat(FIGuard, Bitwidth);
    VASTValPtr FIVal = Builder.buildAndExpr(SlotFanins, Bitwidth);
    VASTValPtr GuardedFIVal = Builder.buildAndExpr(FIVal, FIMask, Bitwidth);

    CurLeaves.clear();
    GuardedFIVal->extractSupportingSeqVal(CurLeaves);

    // We need to keep the node to prevent it from being optimized improperly,
    // if it is reachable by the intersect leaves.
    if (intersect(IntersectLeaves, CurLeaves)) {
      GuardedFIVal = Builder.buildKeep(GuardedFIVal);
      while (!Slots.empty())
        Sel->annotateReadSlot(Slots.pop_back_val(), GuardedFIVal);
    }

    AllFanins.push_back(GuardedFIVal);
  }

  // Build the final fanin only if the selector is not enable.
  VASTValPtr FI = VASTImmediate::True;
  if (!Sel->isEnable() && !Sel->isSlot())
    FI = Builder.buildOrExpr(AllFanins, Bitwidth);

  Sel->setMux(FI, Builder.buildOrExpr(FaninGuards, 1));
}
