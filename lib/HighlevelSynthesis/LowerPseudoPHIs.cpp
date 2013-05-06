//==- LowerPseudoPHIs.cpp - Lower the Pseudo PHIs in the Design -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the LowerPseudoPHIs pass, which first identify the PHI
// nodes in form:
// pn = phi [ incoming, ...] [pn|undef, ...]+
// This means these PHI nodes only takes the value of incoming and keeps unchanged.
// Once such phis are identified, the pass replace pn by incoming.
//
//===----------------------------------------------------------------------===//
/*
#include "SeqLiveVariables.h"

#include "shang/VASTHandle.h"
#include "shang/VASTExprBuilder.h"
#include "shang/VASTModule.h"
#include "shang/VASTModulePass.h"
#include "shang/Passes.h"

#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#define  DEBUG_TYPE "shang-pseudo-phi-lowering"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumLoweredPHIs, "Number of lowered pseudo phi nodes");

namespace {
struct LowerPseudoPHIs : public VASTModulePass {
  static char ID;
  SeqLiveVariables *LVS;
  VASTModule *VM;
  std::map<VASTSeqValue*, SeqLiveVariables::VarInfo> CachedSrc;

  void resetCache() {
    CachedSrc.clear();
  }

  SeqLiveVariables::VarInfo &getCachedVI(VASTSeqValue *V) {
    return CachedSrc[V];
  }

  LowerPseudoPHIs() : VASTModulePass(ID), LVS(0), VM(0) {
    initializeLowerPseudoPHIsPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addPreservedID(ControlLogicSynthesisID);

    AU.addRequired<SeqLiveVariables>();
    AU.addPreserved<SeqLiveVariables>();
  }

  bool runOnVASTModule(VASTModule &VM);

  void lowerPseudoPHI(VASTSeqValue *V, VASTValPtr ForwordedValue);
  void extendLiveInterval(VASTSeqValue *SrcV, VASTSeqValue *DstV);
};
}

INITIALIZE_PASS_BEGIN(LowerPseudoPHIs, "shang-pseudo-phi-lowering",
                      "Lower pseudo PHIs in the design", false, true)
  INITIALIZE_PASS_DEPENDENCY(SeqLiveVariables)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
INITIALIZE_PASS_END(LowerPseudoPHIs, "shang-pseudo-phi-lowering",
                    "Lower pseudo PHIs in the design", false, true)

Pass *llvm::createLowerPseudoPHIsPass() {
  return new LowerPseudoPHIs();
}

char LowerPseudoPHIs::ID = 0;

// Identify the PHI node which in the following form:
// pn = phi [ incoming, ...] [pn|undef, ...]+
static VASTValPtr isPseudoPHI(VASTSeqValue *V) {
  if (V->getValType() != VASTSeqValue::Data || V->size() <= 1)
    return VASTValPtr();
  
  VASTValPtr Forwarded;

  for (VASTSeqValue::const_iterator I = V->begin(), E = V->end(); I != E; ++I) {
    VASTValPtr Val = *I;

    // Undefined incoming is allowed.
    if (isa<VASTUDef>(Val)) continue;

    // Only incoming from the current SeqVal (i.e. a loop) is allowed.
    if (VASTWire *W = dyn_cast<VASTWire>(Val)) {
      if (!W->isWrapper()) return VASTValPtr();

      if (W->getDriver() != V) return VASTValPtr();

      continue;
    }

    // Otherwise, this should be the forwared value.
    if (Forwarded == Val) continue;
    
    if (Forwarded) return VASTValPtr();

    Forwarded = Val;
  }

  return Forwarded;
}

bool LowerPseudoPHIs::runOnVASTModule(VASTModule &VM) {
  LVS = &getAnalysis<SeqLiveVariables>();
  this->VM = &VM;

  MinimalExprBuilderContext C(VM);
  VASTExprBuilder Builder(C);

  bool changed = false;

  std::vector<std::pair<VASTSeqValue*, VASTHandle> > Worklist;

  typedef VASTModule::seqval_iterator iterator;

  for (iterator I = VM.seqval_begin(), IE = VM.seqval_end(); I != IE; /*++I*//*) {
    VASTSeqValue *V = I++;
    VASTValPtr Forwarded = isPseudoPHI(V);

    if (!Forwarded) continue;
    Worklist.push_back(std::make_pair(V, Forwarded));
    lowerPseudoPHI(V, Forwarded);
    ++NumLoweredPHIs;
    changed = true;
  }

  while (!Worklist.empty()) {
    Builder.replaceAllUseWith(Worklist.back().first, Worklist.back().second);
    Worklist.pop_back();
  }

  resetCache();

  return changed;
}

// Extend the live interval of Src by Dst.
void LowerPseudoPHIs::extendLiveInterval(VASTSeqValue *SrcV, VASTSeqValue *DstV)
{
  SeqLiveVariables::VarInfo *Src = 0; // LVS->getUniqueVarInfo(SrcV);
  SeqLiveVariables::VarInfo *Dst = 0; // LVS->getUniqueVarInfo(DstV);
  SeqLiveVariables::VarInfo &CachedSrc = getCachedVI(SrcV);
  // Initialize the cached VI if it is not yet intialized.
  if (CachedSrc.Defs.empty()) CachedSrc = *Src;
  
  dbgs() << "\n\nGoing to extend\n";
  //Src->dump();
  CachedSrc.dump();
  dbgs() << "by\n";
  Dst->dump();

  typedef VASTSeqValue::const_iterator iterator;
  // Create a wrapper wire to break the cycle which will generated by the later
  // replacement.
  for (iterator I = SrcV->begin(), E = SrcV->end(); I != E; ++I) {
    VASTLatch L = *I;
    VASTValPtr FI = L;

    if (FI != DstV) continue;;

    unsigned BitWidth = FI->getBitWidth();
    FI = VM->createWrapperWire(SrcV->getName(), BitWidth, DstV);
    L.replaceUsedBy(FI);
  }

  SparseBitVector<> DstDefs = Dst->Defs, LoopDefs;
  // Clear the def bit for the Undef incoming.
  for (iterator I = DstV->begin(), E = DstV->end(); I != E; ++I) {
    VASTLatch L = *I;
    VASTValPtr Val = L;

    // Ignore the undef incoming.
    if (isa<VASTUDef>(Val)) DstDefs.reset(L.getSlotNum());
    // Mark the loop incoming
    else if (VASTWire *W = dyn_cast<VASTWire>(Val)) {
      if (W->isWrapper() && W->getDriver() == DstV) {
        LoopDefs.set(L.getSlotNum());
        DstDefs.reset(L.getSlotNum());
      }
    }
  }

  SparseBitVector<> ConnectedDefs = CachedSrc.Kills & DstDefs;
  // FIXME: Assert the fanins of CachedSrc.DefKills and
  // CachedSrc.DefKills & DstDefs must be identical.
  ConnectedDefs |= CachedSrc.DefKills & DstDefs;
  dbgs() << "Get connected defines: (The define of dst that kills src)\n";
  ::dump(ConnectedDefs, dbgs());

  assert((!ConnectedDefs.empty() || DstDefs.intersects(CachedSrc.Alives))
         && "Not able to extand the disjointed interval!");

  // Unset the kills at the connected slots.
  Src->Kills.intersectWithComplement(ConnectedDefs);
  Src->Kills.intersectWithComplement(Dst->Alives);
  Src->DefKills.intersectWithComplement(ConnectedDefs);

  // Update the kill slots.
  SparseBitVector<> DstKills = Dst->Kills;
  // Fix the DstKills
  DstKills.intersectWithComplement(CachedSrc.Alives);
  DstKills.intersectWithComplement(ConnectedDefs);
  // Update the def kills.
  Src->DefKills |= Src->Defs & DstKills;

  // Remove the define kills since they are taken care of.
  DstKills.intersectWithComplement(CachedSrc.DefKills);
  Src->Kills |= DstKills;
  // Because of the loops in dst, the define of dst become the kill of src.
  Src->Kills |= LoopDefs - CachedSrc.Alives;

  Src->Alives |= ConnectedDefs;
  Src->Alives |= Dst->Alives;

  dbgs() << "After extantion:\n";
  Src->dump();

  Src->verify();

  return;
}

void LowerPseudoPHIs::lowerPseudoPHI(VASTSeqValue *V, VASTValPtr ForwardedValue)
{
  // Extend the live interval of the forwarded value.
  std::set<VASTSeqValue*> SrcVals;
  ForwardedValue->extractSupporingSeqVal(SrcVals);

  //SeqLiveVariables::VarInfo *VI = SLV->getVarInfo()

  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = SrcVals.begin(), E = SrcVals.end(); I != E; ++I) {
    VASTSeqValue *SrcV = *I;

    if (SrcV == V) continue;

    extendLiveInterval(SrcV, V);
  }
}
*/
