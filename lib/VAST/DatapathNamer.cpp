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
#include "shang/VASTDatapathNodes.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTModule.h"
#include "shang/VASTModulePass.h"
#include "shang/Passes.h"

#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vast-datapath-namer"
#include "llvm/Support/Debug.h"

#include <sstream>

using namespace llvm;

static cl::opt<unsigned>
ExprInlineThreshold("vtm-expr-inline-thredhold",
                    cl::desc("Inline the expression which has less than N "
                             "operand  (16 by default)"),
                    cl::init(2));

namespace {

struct Namer {
  std::map<VASTExpr*, unsigned> &ExprSize;

  Namer(std::map<VASTExpr*, unsigned> &ExprSize) : ExprSize(ExprSize) {}

  void nameExpr(VASTExpr *Expr) const {
    // The size of named expression is 1.
    ExprSize[Expr] = 1;
    // Dirty hack: Do not name the MUX, they should be print with a wire.
    if (Expr->getOpcode() != VASTExpr::dpMux) Expr->nameExpr();
  }

  void operator()(VASTNode *N) const {
    VASTExpr *Expr = dyn_cast<VASTExpr>(N);

    if (Expr == 0) return;

    // Remove the naming, we will recalculate them.
    Expr->nameExpr(false);

    // Even dpAssign is inlinable, we should assign a name to it.
    // Otherwise we may lost the UB and LB information when we print it.
    // Another solution is name the operand of the dpAssign.
    if (!Expr->isInlinable() || Expr->getOpcode() == VASTExpr::dpAssign) {
      nameExpr(Expr);
      return;
    }

    unsigned Size = 0;

    // Visit all the operand to accumulate the expression size.
    typedef VASTExpr::op_iterator op_iterator;
    for (op_iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
      if (VASTExpr *SubExpr = dyn_cast<VASTExpr>(*I)) {
        std::map<VASTExpr*, unsigned>::const_iterator at = ExprSize.find(SubExpr);
        assert(at != ExprSize.end() && "SubExpr not visited?");
        Size += at->second;
        continue;
      }

      Size += 1;
    }

    if (Size >= ExprInlineThreshold) nameExpr(Expr);
    else                             ExprSize[Expr] = Size;
  }
};

struct DatapathNamer : public VASTModulePass {
  static char ID;

  DatapathNamer() : VASTModulePass(ID) {
    initializeDatapathNamerPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.setPreservesAll();
  }

  bool runOnVASTModule(VASTModule &VM);
};
}

INITIALIZE_PASS(DatapathNamer, "datapath-namer",
                "Assign name to the datapath node", false, true)
char DatapathNamer::ID = 0;
char &llvm::DatapathNamerID = DatapathNamer::ID;

bool DatapathNamer::runOnVASTModule(VASTModule &VM) {
  std::set<VASTOperandList*> Visited;
  std::map<VASTExpr*, unsigned> ExprSize;
  Namer N(ExprSize);

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
        VASTOperandList::visitTopOrder(V, Visited, N);
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
      VASTOperandList::visitTopOrder(FIVal, Visited, N);
      VASTValue *FICnd = FI->Cnd.unwrap().get();
      VASTOperandList::visitTopOrder(FICnd, Visited, N);
    }

    VASTValue *SelEnable = Sel->getEnable().get();
    VASTOperandList::visitTopOrder(SelEnable, Visited, N);
  }

  return false;
}
