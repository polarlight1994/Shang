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
#include "shang/VASTControlPathNodes.h"
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
    if (Expr->hasName()) Expr->unnameExpr();

    if (!Expr->isInlinable()) {
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

  typedef VASTModule::const_slot_iterator slot_iterator;

  for (slot_iterator SI = VM.slot_begin(), SE = VM.slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    typedef VASTSlot::const_op_iterator op_iterator;

    // Print the logic of slot ready and active.
    VASTOperandList::visitTopOrder(S->getActive(), Visited, Namer(ExprSize));

    // Print the logic of the datapath used by the SeqOps.
    for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
      VASTSeqOp *L = *I;

      typedef VASTOperandList::op_iterator op_iterator;
      for (op_iterator OI = L->op_begin(), OE = L->op_end(); OI != OE; ++OI) {
        VASTValue *V = OI->unwrap().get();
        VASTOperandList::visitTopOrder(V, Visited, Namer(ExprSize));
      }
    }
  }

  // Also print the driver of the wire outputs.
  typedef VASTModule::const_port_iterator port_iterator;
  for (port_iterator I = VM.ports_begin(), E = VM.ports_end(); I != E; ++I) {
    VASTPort *P = *I;

    if (P->isInput() || P->isRegister()) continue;
    VASTWire *W = cast<VASTWire>(P->getValue());
    VASTOperandList::visitTopOrder(W, Visited, Namer(ExprSize));
  }
  return false;
}
