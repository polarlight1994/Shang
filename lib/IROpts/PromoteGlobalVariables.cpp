//===- PromoteGlobalVariables.cpp - Promote Global Variables ----*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement th ePromoteGlobalVariables, which replace the access to
// scalar global variables to the access to a shadow stack variable, and flush
// the content to the global variables when the function return.
//
//===----------------------------------------------------------------------===//
#include "vast/Passes.h"

#include "llvm/Pass.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"

#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-promote-globals-to-stack"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(NumPromotedGV, "Number of GlobalVariable Replaced by Stack Variable");

namespace {
struct GlobalToStack : public ModulePass {
  static char ID;
  DataLayout *TD;

  GlobalToStack() : ModulePass(ID), TD(0) {
    initializeGlobalToStackPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M);
  bool replaceScalarGlobalVariable(GlobalVariable *GV,
                                   BasicBlock *Entry,
                                   BasicBlock::iterator InsertPos,
                                   ArrayRef<ReturnInst*> Rets);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<DataLayout>();
  }
};
}

char GlobalToStack::ID = 0;

INITIALIZE_PASS(GlobalToStack, "global2stack",
                "Create Shadow Register for global variables",
                false, false)

Pass *vast::createGlobalToStackPass() {
  return new GlobalToStack();
}

bool GlobalToStack::runOnModule(Module &M) {
  TD = &getAnalysis<DataLayout>();
  bool changed = false;

  // Verify the goto expansion is run before this pass and find the top function.
  Function *TopFunction = 0;
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
    Function *F = I;

    if (!F->use_empty() || F->isDeclaration())  continue;

    // Ignore the dead functions.
    if (!F->hasExternalLinkage()) continue;

    assert(TopFunction == 0 && "More than 1 top functions!");
    TopFunction = F;
  }

  // Collect the return instructions.
  std::vector<ReturnInst*> Rets;
  typedef Function::iterator bb_iterator;
  for (bb_iterator I = TopFunction->begin(), E = TopFunction->end();I != E;++I)
    if (ReturnInst *R = dyn_cast<ReturnInst>(I->getTerminator()))
      Rets.push_back(R);

  typedef BasicBlock::iterator iterator;
  BasicBlock *Entry = &TopFunction->getEntryBlock();
  iterator EntryInsertPoint = Entry->begin();

  typedef Module::global_iterator global_iterator;
  for (global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I) {
    GlobalVariable *GV = I;

    // Ignore the dead GlobalVariables.
    if (GV->use_empty()) continue;

    if (replaceScalarGlobalVariable(GV, Entry, EntryInsertPoint, Rets)) {
      changed = true;
      continue;
    }
    
    // TODO: Also replace the small arrays.
  }

  return changed;
}

static Instruction *rewriteConstExpr(ConstantExpr *Expr, Value *From,
                                     BasicBlock *Entry,
                                     BasicBlock::iterator InsertPos) {
  Instruction *Inst = Expr->getAsInstruction();
  Entry->getInstList().insert(InsertPos, Inst);
  while (!Expr->use_empty()) {
    User *U = Expr->use_back();
    if (ConstantExpr *SubExpr = dyn_cast<ConstantExpr>(U))
      U = rewriteConstExpr(SubExpr, SubExpr, Entry, InsertPos);
    else if (GlobalAlias *GA = dyn_cast<GlobalAlias>(U)) {
      Constant *Aliasee = GA->getAliasee();
      if (ConstantExpr *SubExpr = dyn_cast<ConstantExpr>(Aliasee))
        U = rewriteConstExpr(SubExpr, GA, Entry, InsertPos);
      else
        llvm_unreachable("Unexpected user!");
    }

    U->replaceUsesOfWith(From, Inst);
  }

  Expr->destroyConstant();
  return Inst;
}

bool GlobalToStack::replaceScalarGlobalVariable(GlobalVariable *GV,
                                                BasicBlock *Entry,
                                                BasicBlock::iterator InsertPos,
                                                ArrayRef<ReturnInst*> Rets) {
  Type *Ty = GV->getType()->getElementType();

  if (isa<ArrayType>(Ty) || isa<StructType>(Ty))
    return false;

  AllocaInst *Shadow = new AllocaInst(Ty, GV->getName() + ".shadow", InsertPos);

  while (!GV->use_empty()) {
    User *U = GV->use_back();
    Instruction *I = 0;
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(U)) {
      I = rewriteConstExpr(CE, CE, Entry, InsertPos);
    } else
      I = cast<Instruction>(U);

    I->replaceUsesOfWith(GV, Shadow);
  }

  // Load the value to the shadow register in the entry of the BB.
  LoadInst *L = new LoadInst(GV, GV->getName() + ".restore", InsertPos);
  new StoreInst(L, Shadow, InsertPos);

  // Store the value of the shadow back to the global variable.
  for (unsigned i = 0; i < Rets.size(); ++i) {
    ReturnInst *R = Rets[i];

    LoadInst *L = new LoadInst(Shadow, Shadow->getName() + ".save", R);
    new StoreInst(L, GV, R);
  }

  ++NumPromotedGV;
  return true;
}
