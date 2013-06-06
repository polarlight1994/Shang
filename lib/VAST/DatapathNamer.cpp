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
  CachedStrashTable *Strash;
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
    Strash = 0;
  }

  void nameExpr(VASTExpr *Expr) {
    unsigned StrashID = Strash->getOrCreateStrashID(Expr);
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

INITIALIZE_PASS_BEGIN(DatapathNamer, "datapath-namer",
                      "Assign name to the datapath node", false, true)
  INITIALIZE_PASS_DEPENDENCY(CachedStrashTable);
INITIALIZE_PASS_END(DatapathNamer, "datapath-namer",
                    "Assign name to the datapath node", false, true)
char DatapathNamer::ID = 0;
char &llvm::DatapathNamerID = DatapathNamer::ID;

bool DatapathNamer::runOnVASTModule(VASTModule &VM) {
  Strash = &getAnalysis<CachedStrashTable>();
  std::set<VASTOperandList*> Visited;

  typedef VASTModule::const_slot_iterator slot_iterator;

  for (slot_iterator SI = VM.slot_begin(), SE = VM.slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    typedef VASTSlot::const_op_iterator op_iterator;

    // Print the logic of the datapath used by the SeqOps.
    for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
      VASTSeqOp *L = *I;

      typedef VASTOperandList::op_iterator op_iterator;
      for (op_iterator OI = L->op_begin(), OE = L->op_end(); OI != OE; ++OI) {
        VASTValue *V = OI->unwrap().get();
        VASTOperandList::visitTopOrder(V, Visited, *this);
      }
    }
  }

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;
    if (!Sel->isSelectorSynthesized()) continue;

    typedef VASTSelector::fanin_iterator fanin_iterator;
    for (fanin_iterator I = Sel->fanin_begin(), E = Sel->fanin_end();
         I != E; ++I){
      const VASTSelector::Fanin *FI = *I;
      VASTValue *FIVal = FI->FI.unwrap().get();
      VASTOperandList::visitTopOrder(FIVal, Visited, *this);
      VASTValue *FICnd = FI->Cnd.unwrap().get();
      VASTOperandList::visitTopOrder(FICnd, Visited, *this);
    }

    VASTValue *SelEnable = Sel->getEnable().get();
    VASTOperandList::visitTopOrder(SelEnable, Visited, *this);
  }

  return false;
}
