//===-------------------- SIRBuild.cpp - IR2SIR -----------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of the SIRBuild pass, which construct the SIR from LLVM IR
//
//===----------------------------------------------------------------------===//
#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/Passes.h"

#include "llvm/InstVisitor.h"
#include "llvm/PassSupport.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

bool SIRInit::runOnFunction(Function &F) {
  SM = new SIR(&F);

  // Initialize SIR from IR by tranform llvm-inst to Shang-inst.
  SIRBuilder Builder();

  Builder.buildInterface();

  // Visit the basic block in topological order.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    Builder.visitBasicBlock(*I);

  return false;
}

void SIRInit::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}