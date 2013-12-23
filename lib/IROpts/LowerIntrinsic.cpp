//===---- LowerIntrinsic.cpp - Lower the Intrinsics Functions ---*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the LowerIntrinsic pass.
//
//===----------------------------------------------------------------------===//
#include "vast/Passes.h"

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/InstIterator.h"
#define DEBUG_TYPE "shang-ntrinsic-lowering"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct LowerIntrinsic : public FunctionPass {
  static char ID;
  LowerIntrinsic() : FunctionPass(ID) {
    initializeLowerIntrinsicPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F);
};
}

char LowerIntrinsic::ID = 0;

INITIALIZE_PASS(LowerIntrinsic, "shang-lower-intrinsic",
                "Lower the unsupported intrinsics",
                false, false)

Pass *vast::createLowerIntrinsicPass() {
  return new LowerIntrinsic();
}

bool LowerIntrinsic::runOnFunction(Function &F) {
  std::vector<IntrinsicInst*> Worklist;

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    IntrinsicInst *Intr = dyn_cast<IntrinsicInst>(&*I);

    if (Intr == 0) continue;

    switch (Intr->getIntrinsicID()) {
    default: break;
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
      Worklist.push_back(Intr);
      break;
    }
  }

  if (Worklist.empty()) return false;

  while (!Worklist.empty()) {
    IntrinsicInst *I = Worklist.back();
    Worklist.pop_back();

    I->eraseFromParent();
  }

  return true;
}
