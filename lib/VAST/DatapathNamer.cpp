//=- DatapathNamer.cpp --- Assign Name to the Datapath Nodes ---------------==//
//
//                      The Shang HLS frameowrk                               //
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
#include "shang/Strash.h"

#include "shang/VASTDatapathNodes.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTModule.h"
#include "shang/VASTModulePass.h"
#include "shang/Passes.h"

#include "llvm/ADT/StringSet.h"
#define DEBUG_TYPE "vast-datapath-namer"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct DatapathNamer : public VASTModulePass {
  static char ID;
  StringSet<> Names;

  DatapathNamer() : VASTModulePass(ID) {
    initializeDatapathNamerPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredTransitive<CachedStrashTable>();
    AU.setPreservesAll();
  }

  bool runOnVASTModule(VASTModule &VM);

  void releaseMemory() {
    Names.clear();
  }
};
}

INITIALIZE_PASS_BEGIN(DatapathNamer, "datapath-namer",
                      "Assign name to the datapath node", false, true)
  INITIALIZE_PASS_DEPENDENCY(CachedStrashTable);
INITIALIZE_PASS_END(DatapathNamer, "datapath-namer",
                    "Assign name to the datapath node", false, true)
char DatapathNamer::ID = 0;
char &llvm::DatapathNamerID = DatapathNamer::ID;

bool DatapathNamer::runOnVASTModule(VASTModule &VM) {
  CachedStrashTable &Strash = getAnalysis<CachedStrashTable>();
  VM.nameDatapath(Names, &Strash);
  return false;
}
