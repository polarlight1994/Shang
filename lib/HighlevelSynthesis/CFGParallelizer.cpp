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

void VASTScheduling::buildControlFlowEdges() {
  Function &F = G->getFunction();
  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;

    ArrayRef<VASTSchedUnit*> SUs(IR2SUMap[BB]);
    buildControlFlowEdge(BB, SUs);
  }
}
