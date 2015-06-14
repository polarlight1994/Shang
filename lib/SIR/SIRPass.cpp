//===------------------------- SIRPass.cpp ----------------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of the SIRPass pass, which is the base of all passed operated 
// on the SIR module.
//
//===----------------------------------------------------------------------===//
#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

#include "llvm/InstVisitor.h"
#include "llvm/PassSupport.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Analysis/ValueTracking.h"
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
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "shang-ir-pass"

using namespace llvm;


void SIRPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredTransitive<SIRAllocation>();
  AU.addPreserved<SIRAllocation>();
  AU.setPreservesCFG();
}

bool SIRPass::runOnFunction(Function &F) {
  SIRAllocation &SA = getAnalysis<SIRAllocation>();

  bool changed = runOnSIR(*SA);

  return changed;
}

// Initialize all passed required by a SIRPass.
SIRPass::SIRPass(char &ID) : FunctionPass(ID) {
  initializeSIRInitPass(*PassRegistry::getPassRegistry());
}



