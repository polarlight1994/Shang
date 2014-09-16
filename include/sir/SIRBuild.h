//===-------------------- SIRBuild.h - IR2SIR -------------------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Declaration of the SIRBuild pass, which construct the SIR from LLVM IR
//
//===----------------------------------------------------------------------===//
#include "sir/SIR.h"
#include "sir/Passes.h"

#include "llvm/InstVisitor.h"
#include "llvm/PassSupport.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/CallSite.h"


#include "llvm/Support/Debug.h"

#ifndef SIR_BUILD_H
#define SIR_BUILD_H

namespace llvm {
// This class build the SIR by visiting all instructions
// in IR and transform it into Shang-Inst.
struct SIRBuilder : public InstVisitor<SIRBuilder, void> {
  SIR *SM;
  SIRDatapathBuilder Builder;

  SIRBuilder(SIR *SM) : SM(SM), Builder(SM) {}

  // Build the basic interface for whole module.
  void buildInterface(Function *F);

  // Functions to visit BB & Insts.
  void visitBasicBlock(BasicBlock &BB);
  void visitSExtInst(SExtInst &I);
  void visitZExtInst(ZExtInst &I);
  void visitTruncInst(TruncInst &I);
  void visitIntToPtrInst(IntToPtrInst &I);
  void visitPtrToIntInst(PtrToIntInst &I);
  void visitBitCastInst(BitCastInst &I);
  void visitSelectInst(SelectInst &I);
  void visitICmpInst(ICmpInst &I);
  void visitBinaryOperator(BinaryOperator &I);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  void visitGEPOperator(GEPOperator &O, GetElementPtrInst &I);
  void visitStoreInst(StoreInst &I);
  void visitLoadInst(LoadInst &I);
  void visitBranchInst(BranchInst &I);
  void visitUnreachableInst(UnreachableInst &I);
  void visitReturnInst(ReturnInst &I);
  void visitSwitchInst(SwitchInst &I);

};

// This class is used to initialize a SIR from IR.
// To be noted that, the real construction of SIR
// is implemented in SIRBuilder which is called in
// SIRInit.
struct SIRInit : public FunctionPass {
  SIR *SM;
  static char ID;

  SIRInit() : FunctionPass(ID), SM(0) {
    initializeSIRInitPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F);
  void getAnalysisUsage(AnalysisUsage &AU) const;

  operator SIR*() const { return SM; }
  SIR* operator->() const { return SM;}
};

INITIALIZE_PASS_BEGIN(SIRInit,
                      "shang-ir-init", "SIR Init",
                      false, true)
INITIALIZE_PASS_END(SIRInit,
                    "shang-ir-init", "SIR Init",
                    false, true)

char SIRInit::ID = 0;
}

#endif