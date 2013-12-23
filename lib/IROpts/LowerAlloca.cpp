//=====- LowerAlloca.cpp - Move Stack Memory to Global RAM ----*- C++ -*-=====//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the VTM implementation of TargetFrameInfo class.
//
//===----------------------------------------------------------------------===//

#include "vast/Passes.h"

#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "shang-alloca-2-gv"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/Statistic.h"

using namespace llvm;

STATISTIC(NumAllocaReplaced, "Number of allocas replaced by globalvariable");

namespace {
struct LowerAlloca : public ModulePass {
  static char ID;

  LowerAlloca() : ModulePass(ID) {
    initializeLowerAllocaPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M);
  bool runOnFunction(Function &F, Module &M);

  GlobalVariable *createGVFramStack(AllocaInst *AI, Module &M);
};
}

char LowerAlloca::ID = 0;

INITIALIZE_PASS(LowerAlloca, "shang-lower-alloca",
                "Move Stack Variable to Global Variable",
                false, true)

Pass *vast::createLowerAllocaPass() {
  return new LowerAlloca();
}

GlobalVariable *LowerAlloca::createGVFramStack(AllocaInst *AI, Module &M) {
  Type *AllocatedType = AI->getAllocatedType();
  // Create the global alias.
  GlobalVariable *GV =
    new GlobalVariable(M, AllocatedType, false, GlobalValue::InternalLinkage,
                       Constant::getNullValue(AllocatedType),
                       AI->getName() + utohexstr(intptr_t(AI)) + "_s2g",
                       0, GlobalVariable::NotThreadLocal, 0);
  GV->setAlignment(std::max(8u, AI->getAlignment()));
  // Replace the alloca by the global variable.
  // Please note that this operation make the function become no reentrantable.
  AI->replaceAllUsesWith(GV);
  AI->eraseFromParent();
  ++NumAllocaReplaced;

  return GV;
}

bool LowerAlloca::runOnFunction(Function &F, Module &M) {
  bool changed = false;

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; /*++I*/) {
    if (AllocaInst *AI = dyn_cast<AllocaInst>(&*I++)) {
      createGVFramStack(AI, M);
      changed = true;
    }
  }

  return changed;
}

bool LowerAlloca::runOnModule(Module &M) {
  bool changed = false;

  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
    changed |= runOnFunction(*I, M);

  return changed;
}
