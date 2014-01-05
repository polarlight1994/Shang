//=- DatapathNamer.cpp --- Assign Name to the Datapath Nodes ---------------==//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass assign name to the datapath name so that we can match the node in
// datapath in the constraints script.
//
//===----------------------------------------------------------------------===//
#include "vast/Strash.h"

#include "vast/VASTDatapathNodes.h"
#include "vast/VASTSeqValue.h"
#include "vast/VASTModule.h"
#include "vast/VASTModulePass.h"
#include "vast/Passes.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vast-datapath-namer"
#include "llvm/Support/Debug.h"

using namespace llvm;
static cl::opt<bool>
StrashNaming("vast-strash-naming",
             cl::desc("Name the expressions based on strash."),
             cl::init(true));

namespace {
struct DatapathNamer : public VASTModulePass {
  static char ID;

  DatapathNamer() : VASTModulePass(ID) {
    initializeDatapathNamerPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredTransitive<CachedStrashTable>();
    AU.setPreservesAll();
  }

  bool runOnVASTModule(VASTModule &VM);

  void assignName(CachedStrashTable &Strash, VASTModule &VM);
};
}

INITIALIZE_PASS_BEGIN(DatapathNamer, "datapath-namer",
                      "Assign name to the datapath node", false, true)
  INITIALIZE_PASS_DEPENDENCY(CachedStrashTable);
INITIALIZE_PASS_END(DatapathNamer, "datapath-namer",
                    "Assign name to the datapath node", false, true)
char DatapathNamer::ID = 0;
char &vast::DatapathNamerID = DatapathNamer::ID;

bool DatapathNamer::runOnVASTModule(VASTModule &VM) {
  assignName(getAnalysis<CachedStrashTable>(), VM);

  return false;
}

void DatapathNamer::assignName(CachedStrashTable &Strash, VASTModule &VM) {
  unsigned CurID = 0;
  typedef VASTModule::expr_iterator expr_iterator;
  for (expr_iterator I = VM.expr_begin(), E = VM.expr_end(); I != E; ++I) {
    VASTExpr *Expr = I;

    // Name the expressions according to structural hashing ID if the user ask
    // to do so.
    if (StrashNaming) {
      unsigned StrashID = Strash.getOrCreateStrashID(Expr);
      Expr->assignNameID(StrashID);
      continue;
    }

    Expr->assignNameID(++CurID);
  }
}
