//===- SelectorSynthesis.cpp - Implement the Fanin Mux for Regs -*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Selector MUX synthesis passes.
//
//===----------------------------------------------------------------------===//

#include "vast/FUInfo.h"
#include "vast/VASTMemoryBank.h"
#include "vast/VASTExprBuilder.h"
#include "vast/STGDistances.h"

#include "vast/Strash.h"
#include "vast/VASTModulePass.h"
#include "vast/VASTModule.h"
#include "vast/Passes.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-selector-mux-synthesis"
#include "llvm/Support/Debug.h"

using namespace llvm;
using namespace vast;

namespace {
struct TimingDrivenSelectorSynthesis : public VASTModulePass {
  CachedStrashTable *CST;
  STGDistances *STGDist;

  static char ID;

  TimingDrivenSelectorSynthesis() : VASTModulePass(ID), CST(0), STGDist(0) {
    initializeTimingDrivenSelectorSynthesisPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<CachedStrashTable>();
    AU.addRequired<STGDistances>();
    AU.addPreserved<STGDistances>();
    AU.addRequiredID(vast::ControlLogicSynthesisID);
    AU.addPreservedID(vast::ControlLogicSynthesisID);
  }

  bool runOnVASTModule(VASTModule &VM);

  typedef std::map<VASTSelector*, unsigned> RegSet;
  typedef std::set<VASTSeqValue*> ValSet;
  typedef ValSet::iterator leaf_iterator;

  bool requiresBarrier(const RegSet &IntersectRegs, VASTValPtr V,
                       ArrayRef<VASTSlot*> ReadSlots);
  void updateStructuralInterval(VASTValPtr Root, VASTSlot *ReadSlot,
                                RegSet &IntersectRegs);
  bool synthesizeSelector(VASTSelector *Sel, VASTExprBuilder &Builder);

  void releaseMemory() {}
};
}

INITIALIZE_PASS_BEGIN(TimingDrivenSelectorSynthesis,
                      "timing-driven-selector-synthesis",
                      "Implement the MUX for the Sequantal Logic", false, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(STGDistances)
  INITIALIZE_PASS_DEPENDENCY(CachedStrashTable)
INITIALIZE_PASS_END(TimingDrivenSelectorSynthesis,
                    "timing-driven-selector-synthesis",
                    "Implement the MUX for the Sequantal Logic", false, true)

char TimingDrivenSelectorSynthesis::ID = 0;

char &vast::TimingDrivenSelectorSynthesisID = TimingDrivenSelectorSynthesis::ID;

bool
TimingDrivenSelectorSynthesis::requiresBarrier(const RegSet &IntersectRegs,
                                               VASTValPtr V,
                                               ArrayRef<VASTSlot*> ReadSlots) {
  ValSet CurVals;
  V->extractCombConeLeaves(CurVals);

  typedef ValSet::iterator iterator;
  for (iterator I = CurVals.begin(), E = CurVals.end(); I != E; ++I) {
    VASTSeqValue *SV = *I;

    if (SV->isSlot() || SV->isFUOutput())
      continue;

    VASTSelector *Sel = SV->getSelector();
    RegSet::const_iterator J = IntersectRegs.find(Sel);
    // Not interset ...
    if (J == IntersectRegs.end())
      continue;

    // Calculate the number of cycles available according to the dataflow analysis
    unsigned FlowDistant = STGDist->getIntervalFromDef(SV, ReadSlots);
    unsigned StructuralDistant = J->second;

    // Timing barrier is required if the flow distant is not represented by the
    // structural distant.
    if (FlowDistant > StructuralDistant)
      return true;
  }

  return false;
}

void
TimingDrivenSelectorSynthesis::updateStructuralInterval(VASTValPtr Root,
                                                        VASTSlot *ReadSlot,
                                                        RegSet &IntersectRegs) {
  ValSet Vals;
  Root->extractCombConeLeaves(Vals);

  for (leaf_iterator LI = Vals.begin(), LE = Vals.end(); LI != LE; ++LI) {
    VASTSeqValue *Leaf = *LI;
    VASTSelector *CurSel = Leaf->getSelector();

    RegSet::iterator I = IntersectRegs.find(CurSel);
    if (I == IntersectRegs.end())
      continue;

    unsigned &Cycles = I->second;
    unsigned CurCycles = STGDist->getIntervalFromDef(CurSel, ReadSlot);
    assert(CurCycles && "unexpected zero interval!");
    Cycles = std::min(Cycles, CurCycles);
  }
}

namespace {
struct GuardBuilder {
  std::map<VASTSlot*, VASTValPtr> ReadAtSlot, SlotActive;
  VASTExprBuilder &Builder;
  VASTSelector *Sel;

  GuardBuilder(VASTExprBuilder &Builder, VASTSelector *Sel)
    : Builder(Builder), Sel(Sel) {}

  void reset() {
    ReadAtSlot.clear();
  }

  VASTValPtr buildGuard() {
    typedef std::map<VASTSlot*, VASTValPtr>::iterator iterator;

    SmallVector<VASTValPtr, 4> Values;
    for (iterator I = ReadAtSlot.begin(), E = ReadAtSlot.end(); I != E; ++I) {
      VASTValPtr V = I->second;

      // Build hard annotation for 1 bit guarding condition.
      V = Builder.buildAnnotation(VASTExpr::dpHAnn, V);
      Sel->annotateReadSlot(I->first, V);

      SmallVector<VASTValPtr, 2> CurGuards;
      CurGuards.push_back(V);
      std::map<VASTSlot*, VASTValPtr>::iterator J = SlotActive.find(I->first);
      if (J != SlotActive.end())
        CurGuards.push_back(J->second);

      Values.push_back(Builder.buildAndExpr(CurGuards, 1));
    }

    return Builder.buildOrExpr(Values, Values.front()->getBitWidth());
  }

  void addCondition(const VASTLatch &L) {
    VASTSlot *S = L.getSlot();

    // Collect both the slot active signal and the guarding condition.
    if (VASTValPtr Active = L.getSlotActive())
      SlotActive[S->getParentState()] = Active;

    VASTValPtr CurGuard = L.getGuard();
    // The guarding condition itself is not guard, that is, the guarding
    // condition is read whenever the slot register is set. Hence, we should
    // annotate it with the control-equivalent group instead of the guarding
    // condition equivalent group!
    // FIXME: We can build apply the keep attribute according to the STG
    // subgroup hierarchy sequentially to relax the constraints.
    VASTValPtr &GuardAtSlot = ReadAtSlot[S->getParentState()];
    GuardAtSlot = Builder.orEqual(GuardAtSlot, CurGuard);
  }
};
}

bool TimingDrivenSelectorSynthesis::synthesizeSelector(VASTSelector *Sel,
                                                       VASTExprBuilder &Builder)
{
  typedef std::vector<VASTLatch> OrVec;
  typedef std::map<unsigned, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator iterator;

  CSEMapTy CSEMap;
  GuardBuilder GB(Builder, Sel);

  for (VASTSelector::const_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U))
      continue;

    // Index the input of the assignment based on the strash id. By doing this
    // we can pack the structural identical inputs together.
    CSEMap[CST->getOrCreateStrashID(VASTValPtr(U))].push_back(U);

    GB.addCondition(U);
  }

  if (CSEMap.empty())
    return false;

  VASTValPtr Guard = GB.buildGuard();

  if (Sel->isEnable() || Sel->isSlot()) {
    Sel->setMux(VASTConstant::True, Guard);
    return true;
  }

  if (Sel->numNonTrivialFanins() == 1) {
    VASTLatch L = Sel->getUniqueFannin();
    Sel->setMux(L, Guard);
    return true;
  }

  // Build the intersect leaves set
  std::set<VASTSelector*> AllLeaves;
  std::map<VASTSelector*, unsigned> IntersectLeaves;

  // Build the intersect leaves.
  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    const OrVec &Ors = I->second;
    std::set<VASTSeqValue*> CurLeaves;

    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      const VASTLatch &L = *OI;
      L->extractCombConeLeaves(CurLeaves);
      L.getGuard()->extractCombConeLeaves(CurLeaves);
    }
      
    for (leaf_iterator LI = CurLeaves.begin(), LE = CurLeaves.end();
         LI != LE; ++LI) {
      VASTSeqValue *Leaf = *LI;
      VASTSelector *CurSel = Leaf->getSelector();

      if (!AllLeaves.insert(CurSel).second && !CurSel->isSlot() &&
          !CurSel->isFUOutput()) {
        IntersectLeaves.insert(std::make_pair(CurSel, UINT32_MAX));
      }
    }
  }

  // Initialize the structural available cycles for the intersect leaves.
  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    const OrVec &Ors = I->second;

    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      const VASTLatch &L = *OI;
      VASTSlot *S = L.getSlot();
      updateStructuralInterval(L, S, IntersectLeaves);
      // The guarding condition is read by all slots in the control equivalent
      // states instead the guarding condition equivalent states.
      updateStructuralInterval(L.getGuard(), S->getParentState(), IntersectLeaves);
    }
  }

  unsigned Bitwidth = Sel->getBitWidth();
  SmallVector<VASTValPtr, 4> AllFanins;

  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    SmallVector<VASTSlot*, 4> Slots;
    SmallVector<VASTValPtr, 4> SlotFanins;
    GuardBuilder GB(Builder, Sel);

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      const VASTLatch &L = *OI;
      SlotFanins.push_back(L);
      GB.addCondition(L);
      Slots.push_back(L.getSlot());
    }

    VASTValPtr FIGuard = GB.buildGuard();

    VASTValPtr FIVal = Builder.buildAndExpr(SlotFanins, Bitwidth);

    VASTExpr::Opcode AnnType = VASTExpr::dpSAnn;
    // We need to hard annotate the node to prevent it from being optimized
    // improperly, if it is reachable by the intersect leaves.
    if (requiresBarrier(IntersectLeaves, FIVal, Slots))
      AnnType = VASTExpr::dpHAnn;

    FIVal = Builder.buildAnnotation(AnnType, FIVal);
    Sel->annotateReadSlot(Slots, FIVal);

    VASTValPtr FIMask = Builder.buildBitRepeat(FIGuard, Bitwidth);
    VASTValPtr GuardedFIVal = Builder.buildAndExpr(FIVal, FIMask, Bitwidth);
    AllFanins.push_back(GuardedFIVal);
  }

  // Build the final fanin only if the selector is not enable.
  Sel->setMux(Builder.buildOrExpr(AllFanins, Bitwidth), Guard);
  return true;
}

bool TimingDrivenSelectorSynthesis::runOnVASTModule(VASTModule &VM) {
  bool Changed = false;
  STGDist = &getAnalysis<STGDistances>();
  CST = &getAnalysis<CachedStrashTable>();

  MinimalExprBuilderContext Context(VM);
  VASTExprBuilder Builder(Context);

  typedef VASTModule::selector_iterator iterator;

  // Eliminate the identical SeqOps.
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    I->dropMux();
    Changed |= synthesizeSelector(I, Builder);
  }

  return true;
}

namespace {
struct SelectorSynthesisForAnnotation : public VASTModulePass {
  static char ID;

  SelectorSynthesisForAnnotation() : VASTModulePass(ID) {
    initializeSelectorSynthesisForAnnotationPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<CachedStrashTable>();
    AU.addRequiredID(vast::ControlLogicSynthesisID);
    AU.addPreservedID(vast::ControlLogicSynthesisID);
    // SelectorSynthesisForAnnotation will not destroy the result of
    // TimingDrivenSelectorSynthesis.
    AU.addPreservedID(vast::TimingDrivenSelectorSynthesisID);
    AU.addPreserved<STGDistances>();
  }

  bool runOnVASTModule(VASTModule &VM);

  bool synthesizeSelector(VASTSelector *Sel, VASTExprBuilder &Builder);

  void releaseMemory() {}
};
}

INITIALIZE_PASS_BEGIN(SelectorSynthesisForAnnotation,
                      "selector-synthesis-for-timing-annotation",
                      "Implement the MUX for the Sequantal Logic",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(CachedStrashTable)
INITIALIZE_PASS_END(SelectorSynthesisForAnnotation,
                    "selector-synthesis-for-timing-annotation",
                    "Implement the MUX for the Sequantal Logic",
                    false, true)

bool SelectorSynthesisForAnnotation::runOnVASTModule(VASTModule &VM) {
  bool Changed = false;
  MinimalExprBuilderContext Context(VM);
  VASTExprBuilder Builder(Context);

  typedef VASTModule::selector_iterator iterator;

  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    // Do not thing if the MUX is already there.
    if (I->isSelectorSynthesized())
      continue;

    Changed |= synthesizeSelector(I, Builder);;
  }

  return Changed;
}

static bool intersect(const std::set<VASTSelector*> &LHS,
                      const std::set<VASTSeqValue*> &RHS) {
  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = RHS.begin(), E = RHS.end(); I != E; ++I) {
    VASTSeqValue *SV = *I;

    if (SV->isSlot() || SV->isFUOutput())
      continue;

    if (LHS.count(SV->getSelector()))
      return true;
  }

  return false;
}

bool SelectorSynthesisForAnnotation::synthesizeSelector(VASTSelector *Sel,
                                                        VASTExprBuilder &Builder)
{
  typedef std::vector<VASTLatch> OrVec;
  typedef std::map<VASTValPtr, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator iterator;

  CSEMapTy CSEMap;
  std::set<VASTSeqValue*> CurLeaves;
  std::set<VASTSelector*> AllLeaves, IntersectLeaves;
  typedef std::set<VASTSeqValue*>::iterator leaf_iterator;

  for (VASTSelector::const_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U))
      continue;

    CurLeaves.clear();
    VASTValPtr FI = U;
    FI->extractCombConeLeaves(CurLeaves);
    U.getGuard()->extractCombConeLeaves(CurLeaves);

    for (leaf_iterator LI = CurLeaves.begin(), LE = CurLeaves.end();
         LI != LE; ++LI) {
      VASTSeqValue *Leaf = *LI;
      if (!AllLeaves.insert(Leaf->getSelector()).second &&
          !Leaf->isSlot() && !Leaf->isFUOutput())
        IntersectLeaves.insert(Leaf->getSelector());
    }

    // Index the input of the assignment based on the strash id. By doing this
    // we can pack the structural identical inputs together.
    CSEMap[FI].push_back(U);
  }

  if (CSEMap.empty())
    return false;

  unsigned Bitwidth = Sel->getBitWidth();

  SmallVector<VASTValPtr, 4> SlotGuards, FaninGuards, AllFanins;
  SmallVector<VASTSlot*, 4> Slots;

  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    SlotGuards.clear();

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      SmallVector<VASTValPtr, 2> CurGuards;
      const VASTLatch &L = *OI;
      VASTSlot *S = L.getSlot();

      if (VASTValPtr SlotActive = L.getSlotActive())
        CurGuards.push_back(SlotActive);

      VASTValPtr CurGuard = L.getGuard();

      CurLeaves.clear();
      CurGuard->extractCombConeLeaves(CurLeaves);

      VASTExpr::Opcode AnnType = VASTExpr::dpSAnn;
      // We need to keep the node to prevent it from being optimized improperly,
      // if it is reachable by the intersect leaves.
      if (intersect(IntersectLeaves, CurLeaves))
        AnnType = VASTExpr::dpHAnn;

      // Simply keep all guarding condition, because they are only 1 bit nodes,
      // and their upper bound is the product of number of slots and number of
      // basic blocks.
      CurGuard = Builder.buildAnnotation(AnnType, CurGuard);
      // Also annotate S, so that we can construct the annotation to VASTSeqOp
      // mapping based on the slot.
      Sel->annotateReadSlot(S, CurGuard);

      CurGuards.push_back(CurGuard);
      CurGuard = Builder.buildAndExpr(CurGuards, 1);

      SlotGuards.push_back(CurGuard);
      Slots.push_back(S);
    }

    VASTValPtr FIGuard = Builder.buildOrExpr(SlotGuards, 1);
    FaninGuards.push_back(FIGuard);

    // No need to build the fanin for the enables, only the guard is used by
    // them.
    if (Sel->isEnable() || Sel->isSlot())
      continue;

    VASTValPtr FIVal = I->first;

    CurLeaves.clear();
    FIVal->extractCombConeLeaves(CurLeaves);

    VASTExpr::Opcode AnnType = VASTExpr::dpSAnn;
    // We need to keep the node to prevent it from being optimized improperly,
    // if it is reachable by the intersect leaves.
    if (intersect(IntersectLeaves, CurLeaves))
      AnnType = VASTExpr::dpHAnn;

    FIVal = Builder.buildAnnotation(AnnType, FIVal);
    Sel->annotateReadSlot(Slots, FIVal);

    VASTValPtr GuardedFIVal = FIVal;
    // Guarding condition is only need when we have more than 1 fanins.
    if (CSEMap.size() != 1) {
      VASTValPtr FIMask = Builder.buildBitRepeat(FIGuard, Bitwidth);
      GuardedFIVal = Builder.buildAndExpr(FIVal, FIMask, Bitwidth);
    }

    AllFanins.push_back(GuardedFIVal);
    Slots.clear();
  }

  // Build the final fanin only if the selector is not enable.
  VASTValPtr FI = VASTConstant::True;
  if (!Sel->isEnable() && !Sel->isSlot())
    FI = Builder.buildOrExpr(AllFanins, Bitwidth);

  Sel->setMux(FI, Builder.buildOrExpr(FaninGuards, 1));
  return true;
}

char SelectorSynthesisForAnnotation::ID = 0;
char &vast::SelectorSynthesisForAnnotationID = SelectorSynthesisForAnnotation::ID;
