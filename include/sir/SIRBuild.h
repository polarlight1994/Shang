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
#include "sir/SIRAllocation.h"
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

  /// Functions to provide basic informations and elements
  unsigned getBitWidth(Value *V);
  unsigned getBitWidth(Type *Ty);
  Value *getAsOperand(Value *Operand, Instruction *ParentInst);

  /// Functions to visit all data-path instructions
  void visitSExtInst(SExtInst &I);
  void visitZExtInst(ZExtInst &I);
  void visitTruncInst(TruncInst &I);
  void visitSelectInst(SelectInst &I);
  void visitICmpInst(ICmpInst &I);
  void visitBinaryOperator(BinaryOperator &I);
  void visitIntrinsicInst(IntrinsicInst &I);
  void visitExtractValueInst(ExtractValueInst &I);
  void visitExtractElementInst(ExtractElementInst &I);
  void visitInsertElementInst(InsertElementInst &I);
  void visitShuffleVectorInst(ShuffleVectorInst &I);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  void visitGEPOperator(GEPOperator &O, GetElementPtrInst &I);

  /// Functions to create Shang-Inst
  Value *createShangInstPattern(ArrayRef<Value *> Ops, Type *RetTy,
    Value *InsertPosition,
    Intrinsic::ID FuncID, bool UsedAsArg);
  Value *createSNegativeInst(Value *U, bool isPositiveValue, bool isNegativeValue,
                             Type *RetTy, Value *InsertPosition, bool UsedAsArg);
  Value *createSOriginToComplementInst(Value *U, Type *RetTy, Value *InsertPosition,
                                       bool UsedAsArg);
  Value *createSComplementToOriginInst(Value *U, Type *RetTy, Value *InsertPosition,
                                       bool UsedAsArg);
  Value *createSBitExtractInst(Value *U, unsigned UB, unsigned LB, Type *RetTy,
                               Value *InsertPosition, bool UsedAsArg);
  Value *createSBitCatInst(ArrayRef<Value *> Ops, Type *RetTy,
                           Value *InsertPosition, bool UsedAsArg);
  Value *createSBitCatInst(Value *LHS, Value *RHS, Type *RetTy,
                           Value *InsertPosition, bool UsedAsArg);
  Value *createSBitRepeatInst(Value *U, unsigned RepeatTimes, Type *RetTy,
                              Value *InsertPosition, bool UsedAsArg);
  Value *createSSExtInst(Value *U, unsigned DstBitWidth, Type *RetTy,
                         Value *InsertPosition, bool UsedAsArg);
  Value *createSSExtInstOrSelf(Value *U, unsigned DstBitWidth, Type *RetTy,
                               Value *InsertPosition, bool UsedAsArg);
  Value *createSZExtInst(Value *U, unsigned DstBitWidth, Type *RetTy,
                         Value *InsertPosition, bool UsedAsArg);
  Value *createSZExtInstOrSelf(Value *U, unsigned DstBitWidth, Type *RetTy,
                               Value *InsertPosition, bool UsedAsArg);
  Value *createSTruncInst(Value *U, unsigned UB, unsigned LB, Type *RetTy,
                          Value *InsertPosition, bool UsedAsArg);
  Value *createSCastInst(Value *U, Type *RetTy, Value *InsertPosition, bool UsedAsArg);
  Value *createSSelInst(Value *Cnd, Value *TrueV, Value *FalseV, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);

  Value *createSNotInst(Value *U, Type *RetTy, Value *InsertPosition, bool UsedAsArg);
  Value *createSRAndInst(Value *U, Type *RetTy, Value *InsertPosition, bool UsedAsArg);
  Value *createSROrInst(Value *U, Type *RetTy, Value *InsertPosition, bool UsedAsArg);
  Value *createSRXorInst(Value *U, Type *RetTy, Value *InsertPosition, bool UsedAsArg);

  Value *createSNEInst(ArrayRef<Value *> Ops, Type *RetTy,
                       Value *InsertPosition, bool UsedAsArg);
  Value *createSEQInst(ArrayRef<Value *> Ops, Type *RetTy,
                       Value *InsertPosition, bool UsedAsArg);
  Value *createSEQInst(Value *LHS, Value *RHS, Type *RetTy,
                       Value *InsertPosition, bool UsedAsArg);
  Value *createSdpSGTInst(ArrayRef<Value *> Ops, Type *RetTy,
                          Value *InsertPosition, bool UsedAsArg);
  Value *createSdpUGTInst(ArrayRef<Value *> Ops, Type *RetTy,
                          Value *InsertPosition, bool UsedAsArg);
  Value *createSdpUGTInst(Value *LHS, Value *RHS, Type *RetTy,
                          Value *InsertPosition, bool UsedAsArg);

  Value *createSIcmpOrEqInst(ICmpInst::Predicate Predicate, ArrayRef<Value *> Ops,
                             Type *RetTy, Value *InsertPosition, bool UsedAsArg);
  Value *createSIcmpOrEqInst(ICmpInst::Predicate Predicate, Value *LHS, Value *RHS,
                             Type *RetTy, Value *InsertPosition, bool UsedAsArg);
  Value *createSICmpInst(ICmpInst::Predicate Predicate, ArrayRef<Value *> Ops,
                         Type *RetTy, Value *InsertPosition, bool UsedAsArg);
  Value *createSICmpInst(ICmpInst::Predicate Predicate, Value *LHS, Value *RHS,
                         Type *RetTy, Value *InsertPosition, bool UsedAsArg);

  Value *createSAddInst(ArrayRef<Value *> Ops, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);
  Value *createSSubInst(ArrayRef<Value *> Ops, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);
  Value *createSMulInst(ArrayRef<Value *> Ops, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);
  Value *createSUDivInst(ArrayRef<Value *> Ops, Type *RetTy,
                         Value *InsertPosition, bool UsedAsArg);
  Value *createSSDivInst(ArrayRef<Value *> Ops, Type *RetTy,
                         Value *InsertPosition, bool UsedAsArg);
  Value *createSAddInst(Value *LHS, Value *RHS, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);
  Value *createSAddInst(Value *LHS, Value *RHS, Value *Carry, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);
  Value *createSSubInst(Value *LHS, Value *RHS, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);
  Value *createSMulInst(Value *LHS, Value *RHS, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);

  Value *createSSRemInst(ArrayRef<Value *> Ops, Type *RetTy,
                         Value *InsertPosition, bool UsedAsArg);

  Value *createSAndInst(ArrayRef<Value *> Ops, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);
  Value *createSOrInst(ArrayRef<Value *> Ops, Type *RetTy,
                       Value *InsertPosition, bool UsedAsArg);
  Value *createSXorInst(ArrayRef<Value *> Ops, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);
  Value *createSAndInst(Value *LHS, Value *RHS, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);
  Value *createSOrInst(Value *LHS, Value *RHS, Type *RetTy,
                       Value *InsertPosition, bool UsedAsArg);
  Value *createSXorInst(Value *LHS, Value *RHS, Type *RetTy,
                        Value *InsertPosition, bool UsedAsArg);
  Value *createSOrEqualInst(Value *&LHS, Value *RHS, Type *RetTy,
                            Value *InsertPosition, bool UsedAsArg);

  Value *createSShiftInst(ArrayRef<Value *> Ops, Type *RetTy, Value *InsertPosition,
                          Intrinsic::ID FuncID, bool UsedAsArg);
  Value *createSShiftInst(Value *LHS, Value *RHS, Type *RetTy, Value *InsertPosition,
                          Intrinsic::ID FuncID, bool UsedAsArg);

  Value *createSGEPInst(GEPOperator *GEP, Type *RetTy,
                        Value *InsertPosition, bool UseAsArg);
  Value *createPtrToIntInst(Value *V, Type *IntTy,
                            Value *InsertPosition, bool UsedAsArg);
  Value *createIntToPtrInst(Value *V, Type *PtrTy,
                            Value *InsertPosition, bool UsedAsArg);
  Value *createBitCastInst(Value *V, Type *RetTy,
                           Value *InsertPosition, bool UsedAsArg);

  // Functions to help us create Shang-Inst.
  Value *getSignBit(Value *U, Value *InsertPosition);
  IntegerType *createIntegerType(unsigned BitWidth);
  Value *createIntegerValue(unsigned BitWidth, unsigned Val);
  Value *createIntegerValue(APInt Val);
};

// SIRCtrlRgnBuilder focus on the building of Control-path
// in SIR.
class SIRCtrlRgnBuilder : public InstVisitor<SIRCtrlRgnBuilder, void> {
  SIR *SM;
  DataLayout &TD;
  // During the building of control path, we need
  // to construct data-path instruction sometimes.
  SIRDatapathBuilder D_Builder;

public:
  SIRCtrlRgnBuilder(SIR *SM, DataLayout &TD)
    : SM(SM), TD(TD), D_Builder(SM, TD) {}

  /// Functions to provide basic information
  unsigned getBitWidth(Value *U);

  /// Functions to visit all control-path instructions
  void visitBranchInst(BranchInst &I);
  void visitSwitchInst(SwitchInst &I);
  void visitReturnInst(ReturnInst &I);

  /// Functions to build Control Logic

  // Function to represent the SeqVal stored in register
  Value *createRegAssignInst(Type *RetTy, Value *InsertPosition);

  // Function to build register
  SIRRegister *createRegister(StringRef Name, Type *ValueTy,
                              BasicBlock *ParentBB = 0, uint64_t InitVal = 0,
                              SIRRegister::SIRRegisterTypes T = SIRRegister::General);

  // Function to build Port
  SIRPort *createPort(SIRPort::SIRPortTypes T, StringRef Name, unsigned BitWidth);

  // Functions to build SubModule
  SIRSubModule *createSubModule();
  void createPortsForMemoryBank(SIRMemoryBank *SMB);
  SIRMemoryBank *createMemoryBank(unsigned BusNum, unsigned AddrSize,
                                  unsigned DataSize, bool RequireByteEnable,
                                  bool IsReadOnly, unsigned ReadLatency);
  void createMemoryTransaction(Value *Addr, Value *Data,
                               SIRMemoryBank *Bank, Instruction &I);

  // Functions to build slot
  SIRSlot *getOrCreateLandingSlot(BasicBlock *BB);
  SIRSlot *createSlot(BasicBlock *ParentBB, unsigned Schedule);
  SIRSlot *advanceToNextSlot(SIRSlot *CurSlot);
  SIRSlot *advanceToNextSlot(SIRSlot *CurSlot, unsigned NumSlots);

  // Functions to build the transition between BB.
  void createConditionalTransition(BasicBlock *DstBB, SIRSlot *SrcSlot, Value *Guard);
  void visitPHIsInSucc(SIRSlot *SrcSlot, SIRSlot *DstSlot,
                       Value *Guard, BasicBlock *SrcBB);

  // Functions to build the transition between Slot. 
  void createStateTransition(SIRSlot *SrcSlot, SIRSlot *DstSlot, Value *Cnd);

  // Functions to build the assign operation.
  void assignToReg(SIRSlot *S, Value *Guard, Value *Src, SIRRegister *Dst);

  // Forward some basic functions from the DataPathBuilder.
  IntegerType *createIntegerType(unsigned BitWidth);
  Value *createIntegerValue(unsigned BitWidth, unsigned Val);
  Value *createIntegerValue(APInt Val);
};

// SIRBuilder build the SIR by visiting all instructions
// in IR and transform it into Shang-Inst.
struct SIRBuilder : public InstVisitor<SIRBuilder, void> {
  SIR *SM;
  DataLayout &TD;
  SIRAllocation &SA;
  // The construction of SIR is divided into two parts:
  // (1) data-path: built by SIRDatapathBuilder
  // (2) control-path: built by SIRControlpathBuilder
  SIRDatapathBuilder D_Builder;
  SIRCtrlRgnBuilder C_Builder;

  SIRBuilder(SIR *SM, DataLayout &TD, SIRAllocation &SA)
    : SM(SM), TD(TD), SA(SA), D_Builder(SM, TD), C_Builder(SM, TD) {}

  // Build the basic interface for whole module.
  void buildInterface(Function *F);

  /// Functions to visit BB & Insts.
  void visitBasicBlock(BasicBlock &BB);
  void visitSExtInst(SExtInst &I);
  void visitZExtInst(ZExtInst &I);
  void visitTruncInst(TruncInst &I);
  void visitSelectInst(SelectInst &I);
  void visitICmpInst(ICmpInst &I);
  void visitBinaryOperator(BinaryOperator &I);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  void visitGEPOperator(GEPOperator &O, GetElementPtrInst &I);
  void visitStoreInst(StoreInst &I);
  void visitLoadInst(LoadInst &I);
  void visitBranchInst(BranchInst &I);
  void visitIntrinsicInst(IntrinsicInst &I);
  void visitExtractValueInst(ExtractValueInst &I);
  void visitExtractElementInst(ExtractElementInst &I);
  void visitInsertElementInst(InsertElementInst &I);
  void visitShuffleVectorInst(ShuffleVectorInst &I);
  void visitSwitchInst(SwitchInst &I);
  void visitReturnInst(ReturnInst &I);
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
};
}



#endif