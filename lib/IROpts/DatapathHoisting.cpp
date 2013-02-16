//===- DatapathCodeMotion.cpp - Move the data-path operations ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the VTM implementation of the DatapathCodeMotion pass.
//
//===----------------------------------------------------------------------===//
#include "shang/Passes.h"
#include "shang/Utilities.h"

#include "llvm/Pass.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Function.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/CFG.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-data-path-hoisting"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumDatapathHoisted, "Number of datapath operations hoisted");
STATISTIC(NumBBSimplified, "Number of blocks simplified");

namespace {
struct DatapathHoisting : public FunctionPass {
  static char ID;
  DatapathHoisting() : FunctionPass(ID) {
    initializeDatapathHoistingPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F);
  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<TargetTransformInfo>();
    AU.addRequired<DominatorTree>();
  }
};
}

Pass *llvm::createDatapathHoistingPass() { return new DatapathHoisting(); }
char DatapathHoisting::ID = 0;

INITIALIZE_PASS_BEGIN(DatapathHoisting,
                      "shang-datapath-hoisting",
                      "Hoist the Datapath Instructions",
                      false, false)
  INITIALIZE_AG_DEPENDENCY(TargetTransformInfo)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
INITIALIZE_PASS_END(DatapathHoisting,
                    "shang-datapath-hoisting",
                    "Hoist the Datapath Instructions",
                    false, false)

static bool hoistInst(Instruction *Inst, DominatorTree &DT) {
  // Do not touch the following instructions.
  if (Inst->isTerminator() || isLoadStore(Inst) || isa<PHINode>(Inst)
      || Inst->mayThrow() || isCall(Inst))
    return false;

  BasicBlock *Dst = DT.getRoot(), *CurMBB = Inst->getParent();

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

  if (Dst == CurMBB) return false;

  DEBUG(dbgs() << "Moving " << *Inst << '\n');

  Inst->removeFromParent();

  Dst->getInstList().insert(Dst->getTerminator(), Inst);
  ++NumDatapathHoisted;

  return true;
}

static bool hoistInstInFunction(Function &F, DominatorTree &DT) {
  bool changed = false;
  // Visit the BBs in topological order this can benefit some of the later
  // algorithms.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator rpo_iterator;
  for (rpo_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    BasicBlock *BB = *I;
    for (BasicBlock::iterator BI = BB->begin(), BE = BB->end(); BI != BE; /*++BI*/)
      changed |= hoistInst(BI++, DT);
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
bool DatapathHoisting::runOnFunction(Function &F) {
  DominatorTree &DT = getAnalysis<DominatorTree>();
  TargetTransformInfo &TTI = getAnalysis<TargetTransformInfo>();
  DataLayout *TD = getAnalysisIfAvailable<DataLayout>();

  bool changed = false;
  bool LocalChanged = true;
  while (LocalChanged) {
    changed |= (LocalChanged = hoistInstInFunction(F, DT));
    changed |= (LocalChanged = SimplifyCFGOnFunction(F, TTI, TD));
    // Recalculate the Dominator tree.
    DT.runOnFunction(F);
  }

  return changed;
}
