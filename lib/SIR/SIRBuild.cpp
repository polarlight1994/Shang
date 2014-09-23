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

//------------------------------------------------------------------------//
char SIRInit::ID = 0;

Pass *llvm::createSIRInitPass() {
  return new SIRInit();
}
//------------------------------------------------------------------------//

INITIALIZE_PASS_BEGIN(SIRInit,
                      "shang-ir-init", "SIR Init",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(SIRInit,
                    "shang-ir-init", "SIR Init",
                    false, true)

bool SIRInit::runOnFunction(Function &F) {
  DataLayout &TD = getAnalysis<DataLayout>();

  SM = new SIR(&F);

  // Initialize SIR from IR by transform llvm-inst to Shang-inst.
  SIRBuilder Builder(SM, TD);

  // Build the general interface(Ports) of the module.
  Builder.buildInterface(&F);

  // Visit the basic block in topological order.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    Builder.visitBasicBlock(*(*I));

  return false;
}

void SIRInit::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DataLayout>();
  AU.setPreservesAll();
}

void SIRBuilder::buildInterface(Function *F) {
  // Add basic ports.
  SM->createPort(SIRPort::Clk, "clk", 1);
  SM->createPort(SIRPort::Rst, "rstN", 1);
  SM->createPort(SIRPort::Start, "start", 1);

  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
       I != E; ++I) {
    Argument *Arg = I;
    StringRef Name = Arg->getName();
    unsigned BitWidth = TD.getTypeSizeInBits(Arg->getType());

    SM->createPort(SIRPort::ArgPort, Name, BitWidth);
  }
  
  Type *RetTy = F->getReturnType();
  if (!RetTy->isVoidTy()) {
    assert(RetTy->isIntegerTy() && "Only support return integer now!");
    unsigned BitWidth = TD.getTypeSizeInBits(RetTy);

    SM->createPort(SIRPort::RetPort, "return_value", BitWidth);
  }
}

void SIRBuilder::visitBasicBlock(BasicBlock &BB) {
  typedef BasicBlock::iterator iterator;
  for (iterator I = BB.begin(), E = BB.end(); I != E; ++I) 
    visit(I);
}

//-----------------------------------------------------------------------//
// All Data-path instructions should be built by SIRDatapathBuilder
//-----------------------------------------------------------------------//

void SIRBuilder::visitTruncInst(TruncInst &I) {
  Builder.visit(I);
}

void SIRBuilder::visitZExtInst(ZExtInst &I) {
  Builder.visit(I);
}

void SIRBuilder::visitSExtInst(SExtInst &I) {
  Builder.visit(I);
}

void SIRBuilder::visitPtrToIntInst(PtrToIntInst &I) {
  Builder.visit(I);
}

void SIRBuilder::visitIntToPtrInst(IntToPtrInst &I) {
  Builder.visit(I);
}

void SIRBuilder::visitBitCastInst(BitCastInst &I) {
  Builder.visit(I);
}

void SIRBuilder::visitSelectInst(SelectInst &I) {
  Builder.visit(I);
}

void SIRBuilder::visitICmpInst(ICmpInst &I) {
  Builder.visit(I);
}

void SIRBuilder::visitBinaryOperator(BinaryOperator &I) {
  Builder.visit(I);
}

void SIRBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  Builder.visit(I);
}

//-----------------------------------------------------------------------//
// All Control-path instructions should be built by SIRControlpathBuilder
//-----------------------------------------------------------------------//

void SIRBuilder::visitReturnInst(ReturnInst &I) {
  if (I.getNumOperands()) {
    SIRRegister *Reg = cast<SIROutPort>(SM->getRetPort())->getRegister();
    // Warning: the guard should the slot value, we set it as true for now.
    Reg->addAssignment(I.getReturnValue(), SM->createIntegerValue(1, 1));

    // Index the register with return instruction.
    SM->IndexSeqInst2Reg(&I, Reg);
  }
}


/// Functions to provide basic information of instruction

unsigned SIRDatapathBuilder::getBitWidth(Value *U) {
  Type *Ty = U->getType();
  unsigned BitWidth = TD.getTypeSizeInBits(Ty);
  return BitWidth;
}



/// Functions to visit all data-path instructions

void SIRDatapathBuilder::visitTruncInst(TruncInst &I) {
  createSTruncInst(I.getOperand(0), getBitWidth(&I), 0,
                   &I, false);
}

void SIRDatapathBuilder::visitZExtInst(ZExtInst &I) {
  unsigned NumBits = getBitWidth(&I);
  createSZExtInst(I.getOperand(0), NumBits, &I, false);
}

void SIRDatapathBuilder::visitSExtInst(SExtInst &I) {
  unsigned NumBits = getBitWidth(&I);
  createSSExtInst(I.getOperand(0), NumBits, &I, false);
}

void SIRDatapathBuilder::visitPtrToIntInst(PtrToIntInst &I) {
  createSCastInst(I.getPointerOperand(), &I, false);
}

void SIRDatapathBuilder::visitIntToPtrInst(IntToPtrInst &I) {
  createSCastInst(I.getOperand(0), &I, false);
}

void SIRDatapathBuilder::visitBitCastInst(BitCastInst &I) {
  createSCastInst(I.getOperand(0), &I, false);
}

void SIRDatapathBuilder::visitSelectInst(SelectInst &I) {
  createSSelInst(I.getOperand(0), I.getOperand(1),
                 I.getOperand(2), &I, false);
}

void SIRDatapathBuilder::visitICmpInst(ICmpInst &I) {
  Value *Ops[] = {I.getOperand(0), I.getOperand(1)};
  createSICmpInst(I.getPredicate(), Ops, &I, false);
}

void SIRDatapathBuilder::visitBinaryOperator(BinaryOperator &I) {
  Value *Ops[] = {I.getOperand(0), I.getOperand(1)};

  switch (I.getOpcode()) {
  case Instruction::Add: createSAddInst(Ops, &I, false); return;
  case Instruction::Sub: createSSubInst(Ops, &I, false); return;
  case Instruction::Mul: createSMulInst(Ops, &I, false); return;

  case Instruction::Shl: createSShiftInst(Ops, &I, Intrinsic::shang_shl, false); return;
  case Instruction::AShr: createSShiftInst(Ops, &I, Intrinsic::shang_ashr, false); return;
  case Instruction::LShr: createSShiftInst(Ops, &I, Intrinsic::shang_lshr, false); return;

  case Instruction::UDiv: createSUDivInst(Ops, &I, false); return;
  case Instruction::SDiv: createSSDivInst(Ops, &I, false); return;

  case Instruction::And: createSAndInst(Ops, &I, false); return;
  case Instruction::Or:  createSOrInst(Ops, &I, false); return;
  case Instruction::Xor: createSXorInst(Ops, &I, false); return;

  default: llvm_unreachable("Unexpected opcode"); break;
  }

  return;
}

void SIRDatapathBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  return visitGEPOperator(cast<GEPOperator>(I), I);
}

void SIRDatapathBuilder::visitGEPOperator(GEPOperator &O, GetElementPtrInst &I) {
  //assert(O.getPointerOperand()->getType() == O.getType() && "Type not match!");
  Value *Ptr = O.getPointerOperand();

  // Cast the Ptr into int type so we can do the math operation below.
  Value *PtrVal = new PtrToIntInst(Ptr, IntegerType::getInt32Ty(I.getContext()),
    "SIRPtrToInt", &I);

  //Value *Test = cast<>
  // FIXME: All the pointer arithmetic are perform under the precision of
  // PtrSize, do we need to perform the arithmetic at the max available integer
  // width and truncate the result?
  unsigned PtrSize = getBitWidth(O.getPointerOperand());
  // Note that the pointer operand may be a vector of pointers. Take the scalar
  // element which holds a pointer.
  Type *Ty = O.getPointerOperandType()->getScalarType();

  typedef GEPOperator::op_iterator op_iterator;
  for (op_iterator OI = O.idx_begin(), E = O.op_end(); OI != E; ++OI) {
    const Value *Idx = *OI;
    if (StructType *StTy = dyn_cast<StructType>(Ty)) {
      unsigned Field = cast<ConstantInt>(Idx)->getZExtValue();
      if (Field) {
        // N = N + Offset
        uint64_t Offset
          = TD.getStructLayout(StTy)->getElementOffset(Field);
        IntegerType *IntegerTy = IntegerType::get(O.getContext(), PtrSize);
        PtrVal = createSAddInst(PtrVal, createSConstantInt(Offset, IntegerTy, false),
          &I, true);
      }

      Ty = StTy->getElementType(Field);
    } else {
      Ty = cast<SequentialType>(Ty)->getElementType();

      // If this is a constant subscript, handle it quickly.
      if (const ConstantInt *CI = dyn_cast<ConstantInt>(Idx)) {
        if (CI->isZero()) continue;
        uint64_t Offs = TD.getTypeAllocSize(Ty)
          * cast<ConstantInt>(CI)->getSExtValue();

        IntegerType *InterTy = IntegerType::get(O.getContext(), PtrSize);
        PtrVal = createSAddInst(PtrVal, createSConstantInt(Offs, InterTy, false),
          &I, true);
        continue;
      }

      // N = N + Idx * ElementSize;
      APInt ElementSize = APInt(PtrSize, TD.getTypeAllocSize(Ty));
      Value *IdxN = const_cast<Value*>(Idx);

      // If the index is smaller or larger than intptr_t, truncate or extend
      // it.
      IdxN = createSBitExtractInst(IdxN, PtrSize, 0, &I, true);

      // If this is a multiply by a power of two, turn it into a shl
      // immediately.  This is a very common case.
      if (ElementSize != 1) {
        if (ElementSize.isPowerOf2()) {
          unsigned Amt = ElementSize.logBase2();
          IntegerType *IntegerTy = IntegerType::get(O.getContext(), PtrSize);
          IdxN = createSShiftInst(IdxN, createSConstantInt(Amt, IntegerTy, false),
            &I, Intrinsic::shang_shl, true);
        } else {
          IntegerType *IntegerTy = IntegerType::get(O.getContext(), PtrSize);
          Value *Scale = createSConstantInt(ElementSize.getSExtValue(),
            IntegerTy, false);
          IdxN = createSMulInst(IdxN, Scale, &I, true);
        }
      }

      PtrVal = createSAddInst(PtrVal, IdxN, &I, true);
    }
  }

  Value *PtrResult = new IntToPtrInst(PtrVal, I.getType(), "SIRIntToPtr", &I);
  I.replaceAllUsesWith(PtrResult);
}

/// Functions to create Shang-Inst

Value *SIRDatapathBuilder::createShangInstPattern(ArrayRef<Value *> Ops, Type *RetTy,
                                                  Instruction *Insertbefore, Intrinsic::ID FuncID,
                                                  bool UsedAsArg) {
  // Insert a correctly-typed definition now.
  std::vector<Type *> FuncTy;
  // The return type
  FuncTy.push_back(RetTy);
  // The operand type
  for (unsigned i = 0; i < Ops.size(); ++i) {
    FuncTy.push_back(Ops[i]->getType());
  }

  Module *M = Insertbefore->getParent()->getParent()->getParent();
  Value *Func = Intrinsic::getDeclaration(M, FuncID, FuncTy);

  Instruction *NewInst = CallInst::Create(Func, Ops, Func->getName(), Insertbefore);
  // If the inst is not used as an argument of other functions,
  // then it is used to replace the inst in IR
  if (!UsedAsArg) Insertbefore->replaceAllUsesWith(NewInst);

  return NewInst;
}

Value *SIRDatapathBuilder::createSBitCatInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                             bool UsedAsArg) {
    // Not finished //

    // Compute the RetTy
    unsigned RetBitWidth = 0;
    for (unsigned i = 0; i < Ops.size(); ++i) {
      RetBitWidth += getBitWidth(Ops[i]);
    }
    IntegerType *RetTy = IntegerType::get(Insertbefore->getContext(), RetBitWidth);

    return createShangInstPattern(Ops, RetTy, Insertbefore, Intrinsic::shang_bit_cat, UsedAsArg);
}

Value *SIRDatapathBuilder::createSBitExtractInst(Value *U, unsigned UB, unsigned LB,
                                                 Instruction *Insertbefore, bool UsedAsArg) {
    IntegerType *T = IntegerType::get(U->getContext(), 8);
    Value *Ops[] = {U, createSConstantInt(UB, T, false), createSConstantInt(LB, T, false)};

    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(Insertbefore->getContext(), UB - LB);

    return createShangInstPattern(Ops, RetTy, Insertbefore, Intrinsic::shang_bit_extract, UsedAsArg);
}

Value *SIRDatapathBuilder::createSCastInst(Value *U, Instruction *Insertbefore, bool UsedAsArg) {
  if (!UsedAsArg) {
    assert((!U || getBitWidth(U) == getBitWidth(Insertbefore))
      && "Cast between types with different size found!");
    Insertbefore->replaceAllUsesWith(U);
  }
  return U;
}

Value *SIRDatapathBuilder::createSTruncInst(Value *U, unsigned UB, unsigned LB,
                                            Instruction *Insertbefore, bool UsedAsArg) {
    // Truncate the value by bit-extract expression.
    return createSBitExtractInst(U, UB, LB, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSZExtInst(Value *U, unsigned DstBitWidth, Instruction *Insertbefore,
                                           bool UsedAsArg) {
    unsigned SrcBitWidth = getBitWidth(U);
    assert(DstBitWidth > SrcBitWidth && "Unexpected DstBitWidth!");
    unsigned NumExtendBits = DstBitWidth - SrcBitWidth;
    IntegerType *T = IntegerType::get(Insertbefore->getContext(), 1);
    Value *Zero = createSConstantInt(0, T, false);

    Value *ExtendBits = createSBitRepeatInst(Zero, NumExtendBits, Insertbefore, true);
    Value *Ops[] = { ExtendBits, U };
    return createSBitCatInst(Ops, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSZExtInstOrSelf(Value *U, unsigned DstBitWidth, Instruction *Insertbefore,
                                                 bool UsedAsArg) {
    if (getBitWidth(U) < DstBitWidth)
      return createSZExtInst(U, DstBitWidth, Insertbefore, UsedAsArg);
    else if (!UsedAsArg) Insertbefore->replaceAllUsesWith(U);

    return U;
}

Value *SIRDatapathBuilder::createSSExtInst(Value *U, unsigned DstBitWidth, Instruction *Insertbefore,
                                           bool UsedAsArg) {
    unsigned SrcBitWidth = getBitWidth(U);
    assert(DstBitWidth > SrcBitWidth && "Unexpected DstBitWidth!");
    unsigned NumExtendBits = DstBitWidth - SrcBitWidth;
    Value *SignBit = getSignBit(U, Insertbefore);

    Value *ExtendBits = createSBitRepeatInst(SignBit, NumExtendBits, Insertbefore, true);
    Value *Ops[] = { ExtendBits, U };
    return createSBitCatInst(Ops, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSExtInstOrSelf(Value *U, unsigned DstBitWidth, Instruction *Insertbefore,
                                                 bool UsedAsArg) {
    if (getBitWidth(U) < DstBitWidth)
      return createSSExtInst(U, DstBitWidth, Insertbefore, UsedAsArg);
    else if (!UsedAsArg) Insertbefore->replaceAllUsesWith(U);

    return U;
}

Value *SIRDatapathBuilder::createSRAndInst(Value *U, Instruction *Insertbefore,
                                           bool UsedAsArg) {
    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(Insertbefore->getContext(), 1);

    return createShangInstPattern(U, RetTy, Insertbefore, Intrinsic::shang_rand, UsedAsArg);
}

Value *SIRDatapathBuilder::createSRXorInst(Value *U, Instruction *Insertbefore,
                                           bool UsedAsArg) {
    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(Insertbefore->getContext(), 1);

    return createShangInstPattern(U, RetTy, Insertbefore, Intrinsic::shang_rxor, UsedAsArg);
}

Value *SIRDatapathBuilder::createSROrInst(Value *U, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    // A | B .. | Z = ~(~A & ~B ... & ~Z).
    Value *NotU = createSNotInst(U, Insertbefore, true);
    return createSNotInst(createSRAndInst(NotU, Insertbefore, true),
      Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSNEInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                         bool UsedAsArg) {
    // Get the bitwise difference by Xor.
    Value *BitWissDiff = createSXorInst(Ops, Insertbefore, true);
    // If there is any bitwise difference, then LHS and RHS is not equal.
    return createSROrInst(BitWissDiff, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSEQInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                         bool UsedAsArg) {
    return createSNotInst(createSNEInst(Ops, Insertbefore, true),
      Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSEQInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                                         bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS };
    return createSEQInst(Ops, Insertbefore, true);
}

Value *SIRDatapathBuilder::createSdpSGTInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                            bool UsedAsArg) {
    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(Insertbefore->getContext(), 1);

    return createShangInstPattern(Ops, RetTy, Insertbefore, Intrinsic::shang_sgt, UsedAsArg);
}

Value *SIRDatapathBuilder::createSdpUGTInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                            bool UsedAsArg) {
    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(Insertbefore->getContext(), 1);

    return createShangInstPattern(Ops, RetTy, Insertbefore, Intrinsic::shang_ugt, UsedAsArg);
}

Value *SIRDatapathBuilder::createSBitRepeatInst(Value *U, unsigned RepeatTimes, Instruction *Insertbefore,
                                                bool UsedAsArg) {
    if (RepeatTimes == 1) {
      if (!UsedAsArg) Insertbefore->replaceAllUsesWith(U);
      return U;
    }

    // Compute the RetTy
    IntegerType *RetTy = IntegerType::get(Insertbefore->getContext(),
      getBitWidth(U) * RepeatTimes);
    Value *Ops[] = { U, createSConstantInt(RepeatTimes, IntegerType::getInt32Ty(Insertbefore->getContext()), false)};
    return createShangInstPattern(Ops, RetTy, Insertbefore, Intrinsic::shang_bit_repeat, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSelInst(Value *Cnd, Value *TrueV, Value *FalseV,
                                          Instruction *Insertbefore, bool UsedAsArg) {
    assert(getBitWidth(Cnd) == 1 && "Bad condition width!");

    unsigned Bitwidth = getBitWidth(TrueV);
    assert(getBitWidth(FalseV) == Bitwidth && "Bad bitwidth!");

    if (ConstantInt *C = dyn_cast<ConstantInt>(Cnd)) {
      Value *Result = C->getValue().getBoolValue() ? TrueV : FalseV; 
      if (!UsedAsArg) Insertbefore->replaceAllUsesWith(Result);
      return Result;
    }

    Value *NewCnd = createSBitRepeatInst(Cnd, Bitwidth, Insertbefore, true);
    return createSOrInst(createSAndInst(NewCnd, TrueV, Insertbefore, true),
      createSAndInst(createSNotInst(NewCnd, Insertbefore, true),
      FalseV, Insertbefore, true),
      Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSIcmpOrEqInst(ICmpInst::Predicate Predicate, ArrayRef<Value *> Ops,
                                               Instruction *Insertbefore, bool UsedAsArg) {
    Value *ICmpResult = createSICmpInst(Predicate, Ops, Insertbefore, true);
    Value *EQResult = createSEQInst(Ops, Insertbefore, true);
    Value *NewOps[] = {EQResult, ICmpResult};
    return createSOrInst(NewOps, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSIcmpOrEqInst(ICmpInst::Predicate Predicate, Value *LHS, Value *RHS,
                                               Instruction *Insertbefore, bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS };
    return createSIcmpOrEqInst(Predicate, Ops, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSICmpInst(ICmpInst::Predicate Predicate, ArrayRef<Value *> Ops,
                                           Instruction *Insertbefore, bool UsedAsArg) {
    assert(Ops.size() == 2 && "There must be two operands!");
    assert(getBitWidth(Ops[0]) == getBitWidth(Ops[1])
      && "Bad icmp bitwidth!");
    SmallVector<Value *, 2> NewOps;
    for (int i = 0; i < Ops.size(); i++)
      NewOps.push_back(Ops[i]);

    switch (Predicate) {
    case CmpInst::ICMP_NE:
      return createSNEInst(NewOps, Insertbefore, UsedAsArg);
    case CmpInst::ICMP_EQ:
      return createSEQInst(NewOps, Insertbefore, UsedAsArg);

    case CmpInst::ICMP_SLT:
      std::swap(NewOps[0], NewOps[1]);
      // Fall though.
    case CmpInst::ICMP_SGT:
      return createSdpSGTInst(NewOps, Insertbefore, UsedAsArg);

    case CmpInst::ICMP_ULT:
      std::swap(NewOps[0], NewOps[1]);
      // Fall though.
    case CmpInst::ICMP_UGT:
      return createSdpUGTInst(NewOps, Insertbefore, UsedAsArg);

    case CmpInst::ICMP_SLE:
      std::swap(NewOps[0], NewOps[1]);
      // Fall though.
    case CmpInst::ICMP_SGE:
      return createSIcmpOrEqInst(CmpInst::ICMP_SGT, NewOps, Insertbefore, UsedAsArg);
      //return buildICmpOrEqExpr(VASTExpr::dpSGT, LHS, RHS);

    case CmpInst::ICMP_ULE:
      std::swap(NewOps[0], NewOps[1]);
      // Fall though.
    case CmpInst::ICMP_UGE:
      return createSIcmpOrEqInst(CmpInst::ICMP_UGT, NewOps, Insertbefore, UsedAsArg);

    default: break;
    }

    llvm_unreachable("Unexpected ICmp predicate!");
    return 0;
}

Value *SIRDatapathBuilder::createSNotInst(Value *U, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    return createShangInstPattern(U, U->getType(), Insertbefore, Intrinsic::shang_not, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAddInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    return createShangInstPattern(Ops, Ops[0]->getType(), Insertbefore, Intrinsic::shang_add, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAddInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS };
    return createSAddInst(Ops, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSubInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    IntegerType *T = IntegerType::get(Insertbefore->getContext(), 1);
    Value *NewOps[] = { Ops[0],
      createSNotInst(Ops[1], Insertbefore, true),
      createSConstantInt(1, T, false) };
    return createSAddInst(NewOps, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSMulInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    // Handle the trivial case trivially.
    if (Ops.size() == 1) {
      if (!UsedAsArg) Insertbefore->replaceAllUsesWith(Ops[0]);
      return Ops[0];
    }

    // Compute the RetTy
    unsigned RetBitWidth = 1;
    for (unsigned i = 0; i < Ops.size(); ++i) {
      RetBitWidth *= getBitWidth(Ops[i]);
    }
    IntegerType *RetTy = IntegerType::get(Insertbefore->getContext(), RetBitWidth);

    return createShangInstPattern(Ops, RetTy, Insertbefore, Intrinsic::shang_mul, UsedAsArg);
}

Value *SIRDatapathBuilder::createSMulInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS};
    return createSMulInst(Ops, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSUDivInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                           bool UsedAsArg) {
    // Compute the RetTy
    unsigned RetBitWidth = getBitWidth(Ops[0]);
    for (unsigned i = 1; i < Ops.size(); ++i) {
      RetBitWidth /= getBitWidth(Ops[i]);
    }
    IntegerType *RetTy = IntegerType::get(Insertbefore->getContext(), RetBitWidth);

    return createShangInstPattern(Ops, RetTy, Insertbefore, Intrinsic::shang_udiv, UsedAsArg);
}

Value *SIRDatapathBuilder::createSSDivInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                           bool UsedAsArg) {
    // Compute the RetTy
    unsigned RetBitWidth = getBitWidth(Ops[0]);
    for (unsigned i = 1; i < Ops.size(); ++i) {
      RetBitWidth /= getBitWidth(Ops[i]);
    }
    IntegerType *RetTy = IntegerType::get(Insertbefore->getContext(), RetBitWidth);

    return createShangInstPattern(Ops, RetTy, Insertbefore, Intrinsic::shang_sdiv, UsedAsArg);
}

Value *SIRDatapathBuilder::createSShiftInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                            Intrinsic::ID FuncID, bool UsedAsArg) {
    assert(Ops.size() == 2 && "The shift inst must have two operands!");
    Value *LHS = Ops[0]; Value *RHS = Ops[1];
    // Limit the shift amount so keep the behavior of the hardware the same as
    // the corresponding software.
    unsigned RHSMaxSize = Log2_32_Ceil(getBitWidth(LHS));
    if (getBitWidth(RHS) > RHSMaxSize) 
      RHS = createSBitExtractInst(Ops[1], RHSMaxSize, 0, Insertbefore, true);  
    Value *NewOps[] = {LHS, RHS};

    return createShangInstPattern(NewOps, LHS->getType(), Insertbefore, FuncID, UsedAsArg);
}

Value *SIRDatapathBuilder::createSShiftInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                                            Intrinsic::ID FuncID, bool UsedAsArg) {
    Value *Ops[] = { LHS, RHS };
    return createSShiftInst(Ops, Insertbefore, FuncID, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAndInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    // Handle the trivial case trivially.
    if (Ops.size() == 1) {
      if (!UsedAsArg) Insertbefore->replaceAllUsesWith(Ops[0]);
      return Ops[0];
    }

    return createShangInstPattern(Ops, Ops[0]->getType(), Insertbefore, Intrinsic::shang_and, UsedAsArg);
}

Value *SIRDatapathBuilder::createSAndInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    Value *Ops[] = {LHS, RHS};
    //assert(TD.getTypeSizeInBits(LHS->getType()) == TD.getTypeSizeInBits(RHS->getType())
    //       && "BitWidth not match!");
    return createSAndInst(Ops, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                         bool UsedAsArg) {
    // Handle the trivial case trivially.
    if (Ops.size() == 1) {
      if (!UsedAsArg) Insertbefore->replaceAllUsesWith(Ops[0]);
      return Ops[0];
    }

    SmallVector<Value *, 8> NotInsts;
    // Build the operands of Or operation into not inst.
    for (unsigned i = 0; i < Ops.size(); ++i) 
      NotInsts.push_back(createSNotInst(Ops[i], Insertbefore, true));

    // Build Or operation with the And Inverter Graph (AIG).
    Value *AndInst = createSAndInst(NotInsts, Insertbefore, true);
    return createSNotInst(AndInst, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                                         bool UsedAsArg) {
    Value *Ops[] = {LHS, RHS};
    return createSOrInst(Ops, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSXorInst(ArrayRef<Value *> Ops, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    assert (Ops.size() == 2 && "There should be more than one operand!!");

    // Build the Xor Expr with the And Inverter Graph (AIG).
    Value *OrInst = createSOrInst(Ops, Insertbefore, true);
    Value *AndInst = createSAndInst(Ops, Insertbefore, true);
    Value *NotInst = createSNotInst(AndInst, Insertbefore, true);

    Value *NewOps[] = {OrInst, NotInst};
    return createSAndInst(NewOps, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSXorInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                                          bool UsedAsArg) {
    Value *Ops[] = {LHS, RHS};
    return createSXorInst(Ops, Insertbefore, UsedAsArg);
}

Value *SIRDatapathBuilder::createSOrEqualInst(Value *LHS, Value *RHS, Instruction *Insertbefore,
                                              bool UsedAsArg) {
    if (LHS == NULL) {
      if (!UsedAsArg) Insertbefore->replaceAllUsesWith(RHS);
      return (LHS = RHS);
    }

    return (LHS = createSOrInst(LHS, RHS, Insertbefore, UsedAsArg));
}

/// Functions to help us create Shang-Inst.
Value *SIRDatapathBuilder::getSignBit(Value *U, Instruction *InsertBefore) {
  unsigned BitWidth = getBitWidth(U);
  return createSBitExtractInst(U, BitWidth, BitWidth - 1, InsertBefore, true);
}

Value *SIRDatapathBuilder::createSConstantInt(uint16_t Value, IntegerType *T, bool Signed) {
  return ConstantInt::get(T, Value, Signed);
}