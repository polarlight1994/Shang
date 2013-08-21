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

  bool assignName(CachedStrashTable &Strash, VASTModule &VM);

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
namespace {
struct Namer {
  CachedStrashTable &Strash;
  StringSet<> &Names;
  Namer(CachedStrashTable &Strash, StringSet<> &Names)
    : Strash(Strash), Names(Names) {}

  void nameExpr(VASTExpr *Expr) {
    unsigned StrashID = Strash.getOrCreateStrashID(Expr);
    StringSet<>::MapEntryTy &Entry
      = Names.GetOrCreateValue("t" + utostr_32(StrashID) + "t");
    Expr->nameExpr(Entry.getKeyData());
  }

  void operator()(VASTNode *N) {
    VASTExpr *Expr = dyn_cast<VASTExpr>(N);

    if (Expr == 0) return;

    nameExpr(Expr);
  }
};
}

bool DatapathNamer::runOnVASTModule(VASTModule &VM) {
  VM.resetSelectorName();

  CachedStrashTable &Strash = getAnalysis<CachedStrashTable>();

  while (assignName(Strash, VM))
    Strash.releaseMemory();

  return false;
}

bool DatapathNamer::assignName(CachedStrashTable & Strash, VASTModule &VM) {
  Namer N(Strash, Names);
  std::set<VASTExpr*> Visited;

  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator SI = VM.slot_begin(), SE = VM.slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    typedef VASTSlot::const_op_iterator op_iterator;

    // Print the logic of the datapath used by the SeqOps.
    for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
      VASTSeqOp *L = *I;

      typedef VASTOperandList::op_iterator op_iterator;
      for (op_iterator OI = L->op_begin(), OE = L->op_end(); OI != OE; ++OI) {
        VASTValue *V = OI->unwrap().get();
        if (VASTExpr *Expr = dyn_cast<VASTExpr>(V))
          Expr->visitConeTopOrder(Visited, N);
      }
    }
  }

  bool SelectorNameChanged = false;
  typedef std::pair<unsigned, unsigned> FIPair;
  std::map<FIPair, VASTSelector*> IdenticalSelectors;

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;
    if (!Sel->isSelectorSynthesized()) continue;

    if (VASTExpr *Expr = Sel->getGuard().getAsLValue<VASTExpr>())
      Expr->visitConeTopOrder(Visited, N);
    if (VASTExpr *Expr = Sel->getFanin().getAsLValue<VASTExpr>())
      Expr->visitConeTopOrder(Visited, N);

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
