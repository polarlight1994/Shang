//===------------------------IRAnalysis.cpp--------------------------------===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  Place the BBs in topological order, this can benefit some
//  of the later algorithms.
//
//===----------------------------------------------------------------------===//

#include "shang/Passes.h"

#include "llvm/Pass.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/CFG.h"
#include "llvm/ADT/PostOrderIterator.h"

using namespace llvm;

namespace {
struct BasicBlockTopOrder : public FunctionPass {
  static char ID;

  BasicBlockTopOrder() : FunctionPass(ID) {
    initializeBasicBlockTopOrderPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    // FIXME: This pass break BlockPlacement.
    AU.setPreservesAll();
  }
};
}

INITIALIZE_PASS(BasicBlockTopOrder, "basicblock-top-order",
                "Place the BasicBlocks in topological order",
                true, true)

char BasicBlockTopOrder::ID = 0;
char &llvm::BasicBlockTopOrderID = BasicBlockTopOrder::ID;

bool BasicBlockTopOrder::runOnFunction(Function &F){
  Function::BasicBlockListType &Blocks = F.getBasicBlockList();
  // Place the BBs in topological order this can benefit some of the later
  // algorithms.
  for (po_iterator<BasicBlock*> I = po_begin(&F.getEntryBlock()),
       E = po_end(&F.getEntryBlock()); I != E; ++I) {
    BasicBlock *BB = *I;
    Blocks.splice(Blocks.begin(), Blocks, BB);
  }

  return false;
}
