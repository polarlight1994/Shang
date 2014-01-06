//===- OptimizePHINodes.cpp - Optimize PHINodes in VASTModule  --*- C++ -*-===//
//
//                      The VAST HLS framework                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the PHINode elimination for VASTModule.
//
//===----------------------------------------------------------------------===//

#include "vast/Passes.h"
#include "vast/VASTModule.h"
#include "vast/VASTModulePass.h"
#include "vast/VASTExprBuilder.h"

#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/Dominators.h"

#define DEBUG_TYPE "vast-phi-node-optimization"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct OptimizePHINodes : public VASTModulePass {
  static char ID;
  OptimizePHINodes() : VASTModulePass(ID) {
    //initializeOptimizePHINodesPass(*PassRegistry::getPassRegistry());
  }

  bool isDominatingFanin(const VASTLatch &L, BasicBlock *BB, DominatorTree &DT);
  bool canBeEliminated(VASTSelector *Sel, BasicBlock *BB, DominatorTree &DT);
  bool runOnVASTModule(VASTModule &VM);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<DominatorTree>();
  }
};
}

char OptimizePHINodes::ID = 0;

bool OptimizePHINodes::isDominatingFanin(const VASTLatch &L, BasicBlock *BB,
                                         DominatorTree &DT) {
  std::set<VASTSeqValue*> Srcs;
  VASTValue *FI = VASTValPtr(L).get();
  FI->extractCombConeLeaves(Srcs);

  VASTValue *Guard = VASTValPtr(L.getGuard()).get();
  Guard->extractCombConeLeaves(Srcs);

  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    Value *V = (*I)->getLLVMValue();

    // Strange value, we do not have any idea about the dominance information of
    // this node.
    if (V == NULL)
      return false;

    // Argument and Constant dominate everything in the function.
    if (isa<Argument>(V) || isa<Constant>(V) || isa<GlobalValue>(V))
      continue;

    if (Instruction *Inst = dyn_cast<Instruction>(V)) {
      if (!DT.dominates(Inst, BB))
        return false;

      continue;
    }

    // Strange LLVM Value ...
    return false;
  }

  return true;
}

bool OptimizePHINodes::canBeEliminated(VASTSelector *Sel, BasicBlock *BB,
                                       DominatorTree &DT) {
  typedef VASTSelector::iterator iterator;
  for (iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    VASTLatch &L = *I;

    if (!isDominatingFanin(L, BB, DT))
      return false;
  }

  return true;
}

bool OptimizePHINodes::runOnVASTModule(VASTModule &VM) {
  bool changed = false;
  DominatorTree &DT = getAnalysis<DominatorTree>();

  DatapathContainer &DP = VM;
  MinimalExprBuilderContext Context(DP);
  VASTExprBuilder Builder(Context);

  typedef VASTModule::seqval_iterator iterator;
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *V = I;
    PHINode *PN = dyn_cast_or_null<PHINode>(V->getLLVMValue());

    if (PN == NULL)
      continue;

    VASTSelector *Sel = V->getSelector();
    if (!canBeEliminated(Sel, PN->getParent(), DT))
      continue;

    unsigned Bitwidth = Sel->getBitWidth();
    SmallVector<VASTValPtr, 8> GuardedFanins;

    typedef VASTSelector::iterator latch_iterator;
    for (latch_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
      VASTLatch &L = *I;

      VASTValue *Guard = VASTValPtr(L.getGuard()).get();
      VASTValPtr FIMask = Builder.buildBitRepeat(Guard, Bitwidth);
      VASTValue *FI = VASTValPtr(L).get();
      GuardedFanins.push_back(Builder.buildAndExpr(FI, FIMask, Bitwidth));
    }

    Builder.replaceAllUseWith(V, Builder.buildOrExpr(GuardedFanins, Bitwidth));
  }

  return changed;
}

Pass *vast::createOptimizePHINodesPass() {
  return new OptimizePHINodes();
}
