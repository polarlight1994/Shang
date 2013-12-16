//===- CFGParallelizer.cpp - Parallelize Basic Blocks in CFG ----*- C++ -*-===//
//
//                      The VAST HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the CFGParallelizer. The CFGParallelizer parallelize BBs
// in CFG based on the BB level dependence.
//
//===----------------------------------------------------------------------===//

#include "VASTScheduling.h"

#include "llvm/Analysis/PostDominators.h"
#include "llvm/Support/CFG.h"
#define DEBUG_TYPE "shang-linear-order-builder"
#include "llvm/Support/Debug.h"

using namespace llvm;

static void buildControlFlowEdge(BasicBlock *BB, ArrayRef<VASTSchedUnit*> &SUs) {
  VASTSchedUnit *Entry = 0;
  for (unsigned i = 0; i < SUs.size(); ++i)
    if (SUs[i]->isBBEntry())
      Entry = SUs[i];

  for (unsigned i = 0; i < SUs.size(); ++i) {
    if (SUs[i]->isBBEntry())
      continue;

    assert(isa<TerminatorInst>(SUs[i]->getInst())
      && "Unexpected instruction type!");
    assert(SUs[i]->getTargetBlock() == BB && "Wrong target BB!");
    Entry->addDep(SUs[i], VASTDep::CreateCndDep());
  }
}

namespace {
struct CFGDepGraph {
  typedef std::set<BasicBlock*> DepSet;
  typedef std::map<BasicBlock*, DepSet> DepMatrix;
  DepMatrix Matrix;
  VASTSchedGraph &G;
  PostDominatorTree &PDT;

  CFGDepGraph(VASTSchedGraph &G, PostDominatorTree &PDT) : G(G), PDT(PDT) {}

  // Build the dependence graph between basic blocks.
  void buildDep(BasicBlock *BB);

  void buildDepForPHIIncomings(PHINode *PN, BasicBlock *BB);

  void parallelizeCFG();

  void analyzeParallelism();

  void analyzeParallelism(BasicBlock *BB);

  bool hasDepEdge(BasicBlock *Src, BasicBlock *Dst) const {
    DepMatrix::const_iterator I = Matrix.find(Dst);

    if (I == Matrix.end())
      return false;

    return I->second.count(Src);
  }

  bool isCFGEdgeParallizable(BasicBlock *Src, BasicBlock *Dst) const;
};
}

void CFGDepGraph::buildDepForPHIIncomings(PHINode *PN, BasicBlock *BB) {
  for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
    Instruction *Inst = dyn_cast<Instruction>(PN->getIncomingValue(i));

    if (Inst == NULL)
      continue;

    Matrix[BB].insert(Inst->getParent());
  }
}

void CFGDepGraph::buildDep(BasicBlock *BB) {
  ArrayRef<VASTSchedUnit*> SUs(G.getSUInBB(BB));

  for (unsigned i = 1, e = SUs.size(); i != e; ++i) {
    VASTSchedUnit *U = SUs[i];

    typedef VASTSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator I = U->dep_begin(), E = U->dep_end(); I != E; ++I) {
      VASTSchedUnit *Src = *I;

      BasicBlock *SrcBB = Src->getParent();
      if (SrcBB == BB)
        continue;

      Matrix[BB].insert(SrcBB);
    }

    // Build the dependencies for the incoming value of PHINodes.
    if (U->isPHI())
      buildDepForPHIIncomings(dyn_cast_or_null<PHINode>(U->getInst()), BB);
  }
}

void CFGDepGraph::analyzeParallelism(BasicBlock *BB) {
  // Do not mess up with the BB with multi predecessors for now.
  for (pred_iterator I = pred_begin(BB), E = pred_end(BB); I != E; ++I) {
    BasicBlock *Pred = *I;

    if (hasDepEdge(Pred, BB) || !PDT.properlyDominates(BB, Pred))
      continue;

    dbgs() <<  Pred->getName() << " -> " << BB->getName() << '\n';
  }
}

void CFGDepGraph::analyzeParallelism() {
  Function &F = G.getFunction();

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    buildDep(BB);
  }

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    analyzeParallelism(BB);
  }
}

void VASTScheduling::buildControlFlowEdges() {
  Function &F = G->getFunction();

  //CFGDepGraph(*G, getAnalysis<PostDominatorTree>()).analyzeParallelism();

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;

    ArrayRef<VASTSchedUnit*> SUs(IR2SUMap[BB]);
    buildControlFlowEdge(BB, SUs);
  }
}
