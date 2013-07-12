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

#include "STGDistances.h"
#include "TimingNetlist.h"
#include "SeqLiveVariables.h"

#include "shang/FUInfo.h"
#include "shang/VASTMemoryPort.h"
#include "shang/VASTExprBuilder.h"

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
typedef TimingNetlist::delay_type delay_type;

class Cone {
  typedef DenseMap<VASTSelector*, delay_type> LeafSetTy;
  LeafSetTy Leaves;

public:
  void buildCone(SVSet &SVs, VASTValPtr Root, TimingNetlist *TNL) {
    VASTExpr *Expr = dyn_cast<VASTExpr>(Root.get());
    for (SVSet::iterator I = SVs.begin(), E = SVs.end(); I != E; ++I) {
      VASTSeqValue *SV = *I;
      delay_type delay = delay_type();
      if (Expr)
        delay = TNL->getDelay(SV, Expr);
      delay_type &old_delay = Leaves[SV->getSelector()];
      old_delay = std::max(old_delay, delay);
    }
  }

  delay_type getDelayFrom(VASTSelector *Sel) const {
    return Leaves.lookup(Sel);
  }
};

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

  void updateIntervals(VASTSlot *S, SeqLiveVariables *SLV) {
    // Update the available interval.
    for (LeafSetTy::iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I) {
      VASTSeqValue *SV = I->first;
      unsigned Interval = SLV->getIntervalFromDef(SV, S);
      I->second.update(Interval, I->second.CriticalDelay);
    }
  }
public:
  void buildCone(SVSet &SVs, Cone *C) {
    for (SVSet::iterator I = SVs.begin(), E = SVs.end(); I != E; ++I) {
      VASTSeqValue *SV = *I;
      Leaves[SV].update(STGDistances::Inf, C->getDelayFrom(SV->getSelector()));
    }
  }

  typedef LeafSetTy::const_iterator iterator;
  iterator begin() const { return Leaves.begin(); }
  iterator end()   const { return Leaves.end(); }

  void addSlot(VASTSlot *S, SeqLiveVariables *SLV) {
    if (!Slots.insert(S).second) return;

    updateIntervals(S, SLV);
  }

  Leaf getLeaf(VASTSeqValue *SV) const {
    return Leaves.lookup(SV);
  }

  typedef std::set<VASTSlot*>::const_iterator slot_iterator;
  slot_iterator slot_begin() const { return Slots.begin(); }
  slot_iterator slot_end() const { return Slots.end(); }

  void mergeSlots(const TimedCone *LHS, SeqLiveVariables *SLV) {
    for (slot_iterator I = LHS->slot_begin(), E = LHS->slot_end(); I != E; ++I)
      addSlot(*I, SLV);
  }

  void merge(const TimedCone *LHS, SeqLiveVariables *SLV) {
    for (iterator I = LHS->begin(), E = LHS->end(); I != E; ++I)
      Leaves[I->first].update(I->second);

    Slots.insert(LHS->slot_begin(), LHS->slot_end());

    // Recalculate the intervals.
    for (slot_iterator I = slot_begin(), E = slot_end(); I != E; ++I)
      updateIntervals(*I, SLV);
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

struct SelectorSynthesis : public VASTModulePass {
  CachedStrashTable *CST;
  TimingNetlist *TNL;
  SeqLiveVariables *SLV;
  unsigned MaxSingleCyleFINum;
  VASTExprBuilder *Builder;
  VASTModule *VM;

  // The delay and available cycles for the combinational cones.
  DenseMap<unsigned, Cone*> Cones;
  DenseMap<VASTValue*, TimedCone*> TimedCones;

  TimedCone *getOrCreateTimedCone(VASTValPtr V) {
    if (TimedCone *C = TimedCones.lookup(V.get()))
      return C;

    SVSet Leaves;

    // Ignore the trivial nodes.
    if (!V->extractSupportingSeqVal(Leaves, true))
      return 0;

    Cone *C = getOrCreateCone(V, Leaves);
    TimedCone *&TC = TimedCones[V.get()];
    TC = new TimedCone();
    TC->buildCone(Leaves, C);
    return TC;
  }

  Cone *getOrCreateCone(VASTValPtr V, SVSet Leaves) {
    unsigned StrashID = CST->getOrCreateStrashID(V.get());

    if (Cone *C = Cones.lookup(StrashID)) return C;

    // Build the cone if it is not yet created.
    Cone *&C = Cones[StrashID];
    C = new Cone();
    C->buildCone(Leaves, V, TNL);
    return C;
  }

  static char ID;

  SelectorSynthesis() : VASTModulePass(ID), CST(0), TNL(0), SLV(0) {
    initializeSelectorSynthesisPass(*PassRegistry::getPassRegistry());

    VFUMux *Mux = getFUDesc<VFUMux>();
    MaxSingleCyleFINum = 2;
    while (Mux->getMuxLatency(MaxSingleCyleFINum) < 0.9
           && MaxSingleCyleFINum < Mux->MaxAllowedMuxSize)
      ++MaxSingleCyleFINum;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<TimingNetlist>();
    AU.addRequired<CachedStrashTable>();
    AU.addRequired<SeqLiveVariables>();
    AU.addPreserved<SeqLiveVariables>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
    AU.addPreserved<STGDistances>();
  }

  bool runOnVASTModule(VASTModule &VM);

  void mergeTrivialCones(FIConeVec &Cones, VASTExprBuilder &Builder,
                         unsigned BitWdith);
  bool mergeCones(FIConeVec &Cones, VASTExprBuilder &Builder,
                  unsigned BitWdith);
  bool isGoodToMerge(TimedCone *LHS, TimedCone *RHS) const;
  void synthesizeSelector(VASTSelector *Sel, VASTExprBuilder &Builder);

  void releaseMemory() {
    DeleteContainerSeconds(Cones);
  }
};
}

INITIALIZE_PASS_BEGIN(SelectorSynthesis, "sequential-selector-synthesis",
                      "Implement the MUX for the Sequantal Logic", false, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
INITIALIZE_PASS_END(SelectorSynthesis, "sequential-selector-synthesis",
                    "Implement the MUX for the Sequantal Logic", false, true)

char SelectorSynthesis::ID = 0;

char &llvm::SelectorSynthesisID = SelectorSynthesis::ID;

void SelectorSynthesis::mergeTrivialCones(FIConeVec &Cones,
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

bool SelectorSynthesis::isGoodToMerge(TimedCone *LHS, TimedCone *RHS) const {
  // It is always good to merge to trivial cones.
  if (!LHS || !RHS) return true;

  TimedCone NewTC;

  NewTC.merge(LHS, SLV);
  NewTC.merge(RHS, SLV);

  if (NewTC.hasTighterConstraints(LHS) || NewTC.hasTighterConstraints(RHS))
    return false;

  return true;
}

bool SelectorSynthesis::mergeCones(FIConeVec &Cones, VASTExprBuilder &Builder,
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
      TNL->buildTimingPathOnTheFly(NewFI);

      // Merge the cones.
      // FIXME: Use the cone returned by "isGoodToMerge"
      TimedCone *TC = getOrCreateTimedCone(NewFI);
      if (TCI)
        TC->mergeSlots(TCI, SLV);
      if (TCJ)
        TC->mergeSlots(TCJ, SLV);

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

void SelectorSynthesis::synthesizeSelector(VASTSelector *Sel,
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

      Sel->createAnnotation(S, CurGuard);
      SlotGuards.push_back(CurGuard);
      Slots.push_back(S);
    }

    VASTValPtr FIGuard = Builder.buildOrExpr(SlotGuards, 1);
    FaninGuards.push_back(FIGuard);

    VASTValPtr FIMask = Builder.buildBitRepeat(FIGuard, Bitwidth);
    VASTValPtr FIVal = Builder.buildAndExpr(SlotFanins, Bitwidth);
    VASTValPtr GuardedFIVal = Builder.buildAndExpr(FIVal, FIMask, Bitwidth);
    TNL->buildTimingPathOnTheFly(GuardedFIVal);

    TimedCone *C = getOrCreateTimedCone(GuardedFIVal);
    FICones[GuardedFIVal] = C;

    // Ignore the trivial cones.
    if (C == 0) continue;

    while (!Slots.empty())
      C->addSlot(Slots.pop_back_val(), SLV);
  }

  // Strip the keep attribute if the keeped value is directly fan into the
  // register.
  Sel->setGuard(Builder.buildOrExpr(FaninGuards, 1));

  mergeTrivialCones(FICones, Builder, Bitwidth);
  while (mergeCones(FICones, Builder, Bitwidth))
    ;

  SmallVector<VASTValPtr, 4> Fanins;
  typedef std::map<VASTValPtr, TimedCone*>::iterator cone_iterator;
  for (cone_iterator I = FICones.begin(), E = FICones.end(); I != E; ++I) {
    VASTValPtr FI = I->first;
    TimedCone *TC = I->second;

    // Apply the keep attribute if necessary.
    if (FICones.size() > 1 && TC && !TC->isSingleCycleCone())
      FI = Builder.buildKeep(FI);

    // Remember the fanin, we are going to OR them together.
    Fanins.push_back(FI);

    // Ignore the trivial cones.
    if (!TC)
      continue;

    typedef TimedCone::slot_iterator slot_iterator;
    for (slot_iterator SI = TC->slot_begin(), SE = TC->slot_end(); SI != SE; ++SI)
      Sel->createAnnotation(*SI, FI);
  }

  Sel->setFanin(Builder.buildOrExpr(Fanins, Bitwidth));

  DeleteContainerSeconds(TimedCones);
}

bool SelectorSynthesis::runOnVASTModule(VASTModule &VM) {
  SLV = &getAnalysis<SeqLiveVariables>();
  TNL = &getAnalysis<TimingNetlist>();
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
