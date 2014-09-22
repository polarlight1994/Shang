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
// SIRDatapathBuilder focus on the building of Data-path
// in SIR.
class SIRDatapathBuilder : public InstVisitor<SIRDatapathBuilder, void> {
  SIR *SM;
  DataLayout &TD;

public:
  SIRDatapathBuilder(SIR *SM, DataLayout &TD) : SM(SM), TD(TD) {}

  // Functions to provide basic information of instruction
  unsigned getBitWidth(Value *U);

  // Functions to visit all data-path instructions
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

  // Functions to create Shang-Inst
  Value *createShangInstPattern(ArrayRef<Value *> Ops, Type *RetTy,
                                Instruction *Insertbefore,
                                Intrinsic::ID FuncID, bool UsedAsArg);
  Value *createSBitExtractInst(Value *U, unsigned UB, unsigned LB,
                               Instruction *Insertbefore, bool UsedAsArg);
  Value *createSBitCatInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                           bool UsedAsArg);
  Value *createSBitRepeatInst(Value *U, unsigned RepeatTimes,
                              Instruction *Insertbefore, bool UsedAsArg);
  Value *createSSExtInst(Value *U, unsigned DstBitWidth,
                         Instruction *Insertbefore, bool UsedAsArg);
  Value *createSSExtInstOrSelf(Value *U, unsigned DstBitWidth,
                               Instruction *Insertbefore, bool UsedAsArg);
  Value *createSZExtInst(Value *U, unsigned DstBitWidth,
                         Instruction *Insertbefore, bool UsedAsArg);
  Value *createSZExtInstOrSelf(Value *U, unsigned DstBitWidth,
                               Instruction *Insertbefore, bool UsedAsArg);
  Value *createSTruncInst(Value *U, unsigned UB, unsigned LB,
                          Instruction *Insertbefore, bool UsedAsArg);
  Value *createSCastInst(Value *U, Instruction *Insertbefore, bool UsedAsArg);
  Value *createSSelInst(Value *Cnd, Value *TrueV, Value *FalseV,
                        Instruction *Insertbefore, bool UsedAsArg);

  Value *createSNotInst(Value *U, Instruction *Insertbefore, bool UsedAsArg);
  Value *createSRAndInst(Value *U, Instruction *Insertbefore, bool UsedAsArg);
  Value *createSROrInst(Value *U, Instruction *Insertbefore, bool UsedAsArg);
  Value *createSRXorInst(Value *U, Instruction *Insertbefore, bool UsedAsArg);

  Value *createSNEInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                       bool UsedAsArg);
  Value *createSEQInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                       bool UsedAsArg);
  Value *createSEQInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                       bool UsedAsArg);
  Value *createSdpSGTInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                          bool UsedAsArg);
  Value *createSdpUGTInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                          bool UsedAsArg);

  Value *createSIcmpOrEqInst(ICmpInst::Predicate Predicate, ArrayRef<Value *> Ops,
                             Instruction *Insertbefore, bool UsedAsArg);
  Value *createSIcmpOrEqInst(ICmpInst::Predicate Predicate, Value *LHS, Value *RHS,
                             Instruction *Insertbefore, bool UsedAsArg);
  Value *createSICmpInst(ICmpInst::Predicate Predicate, ArrayRef<Value *> Ops,
                         Instruction *Insertbefore, bool UsedAsArg);
  Value *createSICmpInst(ICmpInst::Predicate Predicate, Value *LHS, Value *RHS,
                         Instruction *Insertbefore, bool UsedAsArg);

  Value *createSAddInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                        bool UsedAsArg);
  Value *createSSubInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                        bool UsedAsArg);
  Value *createSMulInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                        bool UsedAsArg);
  Value *createSUDivInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                         bool UsedAsArg);
  Value *createSSDivInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                         bool UsedAsArg);
  Value *createSAddInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                        bool UsedAsArg);
  Value *createSMulInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                        bool UsedAsArg);

  Value *createSSRemInst(ArrayRef<Value *> Ops, BinaryOperator &I);

  Value *createSAndInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                        bool UsedAsArg);
  Value *createSOrInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                       bool UsedAsArg);
  Value *createSXorInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                        bool UsedAsArg);
  Value *createSAndInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                        bool UsedAsArg);
  Value *createSOrInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                       bool UsedAsArg);
  Value *createSXorInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                        bool UsedAsArg);
  Value *createSOrEqualInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                            bool UsedAsArg);

  Value *createSShiftInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                          Intrinsic::ID FuncID, bool UsedAsArg);
  Value *createSShiftInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                          Intrinsic::ID FuncID, bool UsedAsArg);

  // Functions to help us create Shang-Inst.
  Value *getSignBit(Value *U, Instruction *InsertBefore);
  Value *createSConstantInt(uint16_t Value, IntegerType *T, bool Signed);
};

// SIRBuilder build the SIR by visiting all instructions
// in IR and transform it into Shang-Inst.
struct SIRBuilder : public InstVisitor<SIRBuilder, void> {
  SIR *SM;
  DataLayout &TD;
  // The construction of SIR is divided into two parts:
  // (1) data-path: built by SIRDatapathBuilder
  // (2) control-path: built by SIRControlpathBuilder
  SIRDatapathBuilder Builder;

  SIRBuilder(SIR *SM, DataLayout &TD)
    : SM(SM), TD(TD), Builder(SM, TD) {}

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
  // Disable the control-path instruction for now.
  //void visitStoreInst(StoreInst &I);
  //void visitLoadInst(LoadInst &I);
  //void visitBranchInst(BranchInst &I);
  //void visitUnreachableInst(UnreachableInst &I);
  //void visitReturnInst(ReturnInst &I);
  //void visitSwitchInst(SwitchInst &I);

};

// SIRInit is used to initialize a SIR from IR.
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
}



#endif