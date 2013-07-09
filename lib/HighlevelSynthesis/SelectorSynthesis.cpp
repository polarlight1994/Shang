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

namespace {
typedef std::set<VASTSeqValue*> SVSet;
typedef TimingNetlist::delay_type delay_type;

class Cone {
  typedef DenseMap<VASTSelector*, delay_type> LeafSetTy;
  LeafSetTy Leaves;

public:
  void buildCone(SVSet &SVs, TimingNetlist *TNL) {
    for (SVSet::iterator I = SVs.begin(), E = SVs.end(); I != E; ++I) {
      VASTSeqValue *SV = *I;
      Leaves[SV->getSelector()] = delay_type();
    }
  }

  delay_type getDelayFrom(VASTSelector *Sel) const {
    return Leaves.lookup(Sel);
  }
};

class TimedCone {
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
  LeafSetTy Leaves;

  std::set<VASTSlot*> Slots;
public:
  void buildCone(SVSet &SVs, Cone *C) {
    for (SVSet::iterator I = SVs.begin(), E = SVs.end(); I != E; ++I) {
      VASTSeqValue *SV = *I;
      Leaves[SV].update(STGDistances::Inf, C->getDelayFrom(SV->getSelector()));
    }
  }

  typedef LeafSetTy::iterator iterator;

  void addSlot(VASTSlot *S, SeqLiveVariables *SLV) {
    if (!Slots.insert(S).second) return;

    // Update the available interval.
    for (iterator I = Leaves.begin(), E = Leaves.end(); I != E; ++I) {
      VASTSeqValue *SV = I->first;
      // Default the interval to 1, which corresponds to the interval from
      // FUOutput. We will change it after we are sure that SV is not FUOutput.
      unsigned Interval = 1;

      if (!SV->isFUOutput())
        Interval = SLV->getIntervalFromDef(SV, S);
      I->second.NumCycles = std::min(I->second.NumCycles, Interval);
    }
  }

  typedef std::set<VASTSlot*>::const_iterator slot_iterator;
  slot_iterator slot_begin() const { return Slots.begin(); }
  slot_iterator slot_end() const { return Slots.end(); }
};

class ConeMerger {
  SmallVector<TimedCone*, 8> Cones;
};

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
    C->buildCone(Leaves, TNL);
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
    AU.addRequired<CachedStrashTable>();
    AU.addRequired<SeqLiveVariables>();
    AU.addPreserved<SeqLiveVariables>();
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);
    AU.addPreserved<STGDistances>();
  }

  unsigned getMinimalCycleOfCone(VASTValPtr V, VASTSlot *S);

  bool runOnVASTModule(VASTModule &VM);

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

static VASTValPtr StripKeep(VASTValPtr V) {
  if (VASTExprPtr Expr = dyn_cast<VASTExpr>(V))
    if (Expr->getOpcode() == VASTExpr::dpKeep)
      return Expr.getOperand(0);

  return V;
}

static void keepAll(VASTExprBuilder &Builder, MutableArrayRef<VASTValPtr> Ops) {
  for (unsigned i = 0; i < Ops.size(); ++i)
    Ops[i] = Builder.buildKeep(Ops[i]);
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

  std::map<VASTValPtr, TimedCone*> FICones;

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

      // Only apply the keep attribute for multi-cycle path.
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

  // TODO: Merge fan-in cones.

  SmallVector<VASTValPtr, 4> Fanins;
  typedef std::map<VASTValPtr, TimedCone*>::iterator cone_iterator;
  for (cone_iterator I = FICones.begin(), E = FICones.end(); I != E; ++I) {
    VASTValPtr FI = I->first;
    Fanins.push_back(FI);
    //FI = Builder.buildKeep(FI);
    TimedCone *TC = I->second;

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
