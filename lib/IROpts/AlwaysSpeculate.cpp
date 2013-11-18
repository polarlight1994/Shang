//===- AlwaysSpeculate.cpp - Speculate all operations -----------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the implementation of the AlwaysSpeculate pass.
//
//===----------------------------------------------------------------------===//
#include "shang/Passes.h"
#include "shang/Utilities.h"

#include "llvm/Pass.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Function.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/CFG.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-data-path-speculation"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumSpeculated, "Number of operations Speculated");
STATISTIC(NumBBSimplified, "Number of blocks simplified");

namespace {
struct AlwaysSpeculate : public FunctionPass {
  static char ID;
  AlwaysSpeculate() : FunctionPass(ID) {
    initializeAlwaysSpeculatePass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F);
  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<TargetTransformInfo>();
    AU.addRequired<DominatorTree>();
    AU.addRequired<LoopInfo>();
  }

  bool optimizePHI(PHINode *PN, DominatorTree &DT);
  bool speculateInst(Instruction *Inst, DominatorTree &DT);
  bool speculateInstInFunction(Function &F, DominatorTree &DT);
};
}

Pass *llvm::createAlwaysSpeculatePass() { return new AlwaysSpeculate(); }
char AlwaysSpeculate::ID = 0;

INITIALIZE_PASS_BEGIN(AlwaysSpeculate,
                      "shang-datapath-hoisting",
                      "Hoist the Datapath Instructions",
                      false, false)
  INITIALIZE_AG_DEPENDENCY(TargetTransformInfo)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
  INITIALIZE_PASS_DEPENDENCY(LoopInfo)
INITIALIZE_PASS_END(AlwaysSpeculate,
                    "shang-datapath-hoisting",
                    "Hoist the Datapath Instructions",
                    false, false)

bool AlwaysSpeculate::optimizePHI(PHINode *PN, DominatorTree &DT) {
  return false;
}

bool AlwaysSpeculate::speculateInst(Instruction *Inst, DominatorTree &DT) {
  if (PHINode *PN = dyn_cast<PHINode>(Inst))
    return optimizePHI(PN, DT);

  // Do not touch the following instructions.
  if (Inst->isTerminator() || isLoadStore(Inst) || Inst->mayThrow()
      || isCall(Inst, false))
    return false;

  BasicBlock *Dst = DT.getRoot(), *CurBB = Inst->getParent();

  typedef Instruction::op_iterator op_iterator;
  for (op_iterator I = Inst->op_begin(), E = Inst->op_end(); I != E; ++I) {
    if (Instruction *Src = dyn_cast<Instruction>(I)) {
      BasicBlock *SrcBB = Src->getParent();
      if (DT.dominates(Dst, SrcBB)) {
        Dst = SrcBB;
        continue;
      }

      assert(DT.dominates(SrcBB, Dst)
             && "Operands not in a path of the dominator tree!");
    }
  }

  if (Dst == CurBB) return false;

  DEBUG(dbgs() << "Moving " << *Inst << '\n');

  Inst->removeFromParent();

  Dst->getInstList().insert(Dst->getTerminator(), Inst);
  ++NumSpeculated;

  return true;
}

bool AlwaysSpeculate::speculateInstInFunction(Function &F, DominatorTree &DT) {
  bool changed = false;
  // Visit the BBs in topological order this can benefit some of the later
  // algorithms.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator rpo_iterator;
  for (rpo_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    BasicBlock *BB = *I;

    if (BB->getSinglePredecessor())
      FoldSingleEntryPHINodes(BB, this);

    for (BasicBlock::iterator BI = BB->begin(), BE = BB->end(); BI != BE; /*++BI*/)
      changed |= speculateInst(BI++, DT);
  }

  return changed;
}

static bool SimplifyCFGOnFunction(Function &F, const TargetTransformInfo &TTI,
                                  const DataLayout *TD) {
  bool Changed = false;
  bool LocalChange = true;
  while (LocalChange) {
    LocalChange = false;

    // Loop over all of the basic blocks and remove them if they are unneeded...
    //
    for (Function::iterator BBIt = F.begin(); BBIt != F.end(); ) {
      if (SimplifyCFG(BBIt++, TTI, TD)) {
        LocalChange = true;
        ++NumBBSimplified;
      }
    }
    Changed |= LocalChange;
  }
  return Changed;
}

bool AlwaysSpeculate::runOnFunction(Function &F) {
  DominatorTree &DT = getAnalysis<DominatorTree>();
  TargetTransformInfo &TTI = getAnalysis<TargetTransformInfo>();
  DataLayout *TD = getAnalysisIfAvailable<DataLayout>();

  bool changed = false;
  bool LocalChanged = true;
  while (LocalChanged) {
    changed |= (LocalChanged = speculateInstInFunction(F, DT));
    changed |= (LocalChanged = SimplifyCFGOnFunction(F, TTI, TD));
    // Recalculate the Dominator tree.
    DT.runOnFunction(F);
    verifyFunction(F);
  }

  return changed;
}
