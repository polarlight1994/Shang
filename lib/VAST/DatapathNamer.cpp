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
#include "vast/Strash.h"

#include "vast/VASTDatapathNodes.h"
#include "vast/VASTSeqValue.h"
#include "vast/VASTModule.h"
#include "vast/VASTModulePass.h"
#include "vast/Passes.h"

#include "llvm/ADT/StringSet.h"
#define DEBUG_TYPE "vast-datapath-namer"
#include "llvm/Support/Debug.h"

using namespace llvm;

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

  bool assignName(CachedStrashTable &Strash, VASTModule &VM);
  
  void nameExpr(VASTExpr *Expr, CachedStrashTable &Strash) {
    unsigned StrashID = Strash.getOrCreateStrashID(Expr);
    Expr->assignNameID(StrashID);
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
  VM.resetSelectorName();

  CachedStrashTable &Strash = getAnalysis<CachedStrashTable>();

  while (assignName(Strash, VM))
    Strash.releaseMemory();

  return false;
}

bool DatapathNamer::assignName(CachedStrashTable &Strash, VASTModule &VM) {
  typedef VASTModule::expr_iterator expr_iterator;
  for (expr_iterator I = VM.expr_begin(), E = VM.expr_end(); I != E; ++I)
    nameExpr(I, Strash);

  bool SelectorNameChanged = false;
  typedef std::pair<unsigned, unsigned> FIPair;
  std::map<FIPair, VASTSelector*> IdenticalSelectors;

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;

    if (!Sel->isSelectorSynthesized())
      continue;

    // Do not rename the selector like memory ports, output port, etc.
    if (!isa<VASTRegister>(Sel->getParent()))
      continue;

    unsigned FIIdx = Strash.getOrCreateStrashID(Sel->getFanin());
    unsigned GuardIdx = Strash.getOrCreateStrashID(Sel->getGuard());
    VASTSelector *&IdenticalSel = IdenticalSelectors[FIPair(FIIdx, GuardIdx)];

    if (IdenticalSel) {
      if (IdenticalSel->getName() != Sel->getName())  {
        SelectorNameChanged |= true;
        Sel->setName(IdenticalSel->getName());
      }

      continue;
    }

    IdenticalSel = Sel;
  }

  return SelectorNameChanged;
}
