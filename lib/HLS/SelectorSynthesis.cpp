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

#include "TimingNetlist.h"

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
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-selector-mux-synthesis"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumTrivialConesMerged, "Number of trivial cones merged");
STATISTIC(NumConesMerged, "Number of cones merged");

namespace {
struct TimingDrivenSelectorSynthesis : public VASTModulePass {
  CachedStrashTable *CST;
  STGDistances *STGDist;
  VASTModule *VM;

  static char ID;

  TimingDrivenSelectorSynthesis() : VASTModulePass(ID), CST(0), STGDist(0) {
    initializeTimingDrivenSelectorSynthesisPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<CachedStrashTable>();
    AU.addRequired<STGDistances>();
    AU.addPreserved<STGDistances>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
  }

  bool runOnVASTModule(VASTModule &VM);

  void synthesizeSelector(VASTSelector *Sel, VASTExprBuilder &Builder);

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

char &llvm::TimingDrivenSelectorSynthesisID = TimingDrivenSelectorSynthesis::ID;

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

      V = Builder.buildKeep(V);
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
    if (GuardAtSlot)
      GuardAtSlot = Builder.orEqual(GuardAtSlot, CurGuard);
    else
      GuardAtSlot = CurGuard;
  }
};
}

void
TimingDrivenSelectorSynthesis::synthesizeSelector(VASTSelector *Sel,
                                                  VASTExprBuilder &Builder) {
  typedef std::vector<VASTLatch> OrVec;
  typedef std::map<unsigned, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator iterator;

  CSEMapTy CSEMap;
  GuardBuilder GB(Builder, Sel);

  for (VASTSelector::const_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch U = *I;

    if (Sel->isTrivialFannin(U)) continue;

    // Index the input of the assignment based on the strash id. By doing this
    // we can pack the structural identical inputs together.
    CSEMap[CST->getOrCreateStrashID(VASTValPtr(U))].push_back(U);

    GB.addCondition(U);
  }

  if (CSEMap.empty()) return;

  VASTValPtr Guard = GB.buildGuard();

  if (Sel->isEnable() || Sel->isSlot()) {
    Sel->setMux(VASTConstant::True, Guard);
    return;
  }

  if (Sel->numNonTrivialFanins() == 1) {
    VASTLatch L = Sel->getUniqueFannin();
    Sel->setMux(L, Guard);
    return;
  }

  // Build the intersect leaves set
  std::set<VASTSeqValue*> CurLeaves;
  std::set<VASTSelector*> AllLeaves, IntersectLeaves;
  typedef std::set<VASTSeqValue*>::iterator leaf_iterator;

  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    const OrVec &Ors = I->second;
    CurLeaves.clear();
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI)
      (*OI)->extractSupportingSeqVal(CurLeaves);

    for (leaf_iterator LI = CurLeaves.begin(), LE = CurLeaves.end();
         LI != LE; ++LI) {
      VASTSeqValue *Leaf = *LI;
      if (!AllLeaves.insert(Leaf->getSelector()).second &&
          !Leaf->isSlot() && !Leaf->isFUOutput())
        IntersectLeaves.insert(Leaf->getSelector());
    }
  }

  unsigned Bitwidth = Sel->getBitWidth();
  SmallVector<VASTValPtr, 4> SlotFanins, AllFanins;
  SmallVector<VASTSlot*, 4> Slots;

  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    SlotFanins.clear();
    GB.reset();

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      const VASTLatch &L = *OI;
      SlotFanins.push_back(L);
      GB.addCondition(L);
      Slots.push_back(L.getSlot());
    }

    VASTValPtr FIGuard = GB.buildGuard();

    VASTValPtr FIVal = Builder.buildAndExpr(SlotFanins, Bitwidth);
    CurLeaves.clear();
    FIVal->extractSupportingSeqVal(CurLeaves);

    // We need to keep the node to prevent it from being optimized improperly,
    // if it is reachable by the intersect leaves.
    if (intersect(IntersectLeaves, CurLeaves)) {
      FIVal = Builder.buildKeep(FIVal);
      while (!Slots.empty())
        Sel->annotateReadSlot(Slots.pop_back_val(), FIVal);
    }

    VASTValPtr FIMask = Builder.buildBitRepeat(FIGuard, Bitwidth);
    VASTValPtr GuardedFIVal = Builder.buildAndExpr(FIVal, FIMask, Bitwidth);
    AllFanins.push_back(GuardedFIVal);
    Slots.clear();
  }

  // Build the final fanin only if the selector is not enable.
  Sel->setMux(Builder.buildOrExpr(AllFanins, Bitwidth), Guard);
}

bool TimingDrivenSelectorSynthesis::runOnVASTModule(VASTModule &VM) {
  STGDist = &getAnalysis<STGDistances>();
  CST = &getAnalysis<CachedStrashTable>();

  {
    MinimalExprBuilderContext Context(VM);
    VASTExprBuilder Builder(Context);
    this->VM = &VM;

    typedef VASTModule::selector_iterator iterator;

    // Eliminate the identical SeqOps.
    for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
      I->dropMux();
      synthesizeSelector(I, Builder);
    }
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
    AU.addPreservedID(PreSchedBindingID);
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
    VASTExprBuilder Builder(Context);
    this->VM = &VM;

    typedef VASTModule::selector_iterator iterator;

    // Eliminate the identical SeqOps.
    for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
      I->dropMux();
      synthesizeSelector(I, Builder);
    }
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

      VASTValPtr CurGuard = L.getGuard();

      CurLeaves.clear();
      CurGuard->extractSupportingSeqVal(CurLeaves);
      // We need to keep the node to prevent it from being optimized improperly,
      // if it is reachable by the intersect leaves.
      if (intersect(IntersectLeaves, CurLeaves)) {
        // Simply keep all guarding condition, because they are only 1 bit nodes,
        // and their upper bound is the product of number of slots and number of
        // basic blocks.
        CurGuard = Builder.buildKeep(CurGuard);

        // Also annotate S, so that we can construct the annotation to VASTSeqOp
        // mapping based on the slot.
        Sel->annotateReadSlot(S, CurGuard);
      }

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

    VASTValPtr FIVal = Builder.buildAndExpr(SlotFanins, Bitwidth);

    CurLeaves.clear();
    FIVal->extractSupportingSeqVal(CurLeaves);

    // We need to keep the node to prevent it from being optimized improperly,
    // if it is reachable by the intersect leaves.
    if (intersect(IntersectLeaves, CurLeaves)) {
      FIVal = Builder.buildKeep(FIVal);
      while (!Slots.empty())
        Sel->annotateReadSlot(Slots.pop_back_val(), FIVal);
    }

    VASTValPtr FIMask = Builder.buildBitRepeat(FIGuard, Bitwidth);
    VASTValPtr GuardedFIVal = Builder.buildAndExpr(FIVal, FIMask, Bitwidth);
    AllFanins.push_back(GuardedFIVal);
    Slots.clear();
  }

  // Build the final fanin only if the selector is not enable.
  VASTValPtr FI = VASTConstant::True;
  if (!Sel->isEnable() && !Sel->isSlot())
    FI = Builder.buildOrExpr(AllFanins, Bitwidth);

  Sel->setMux(FI, Builder.buildOrExpr(FaninGuards, 1));
}
