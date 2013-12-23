//===----- ShangTTI.cpp - Basic TTI for the Shang HLS framework -*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the Shang TargetTransformInfo, which prevent the llvm from
// preforming suboptimal transformation for HLS.
//
//===----------------------------------------------------------------------===//

#include "vast/Passes.h"

#define DEBUG_TYPE "shang-tti"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Target/TargetLowering.h"
#include <utility>

using namespace llvm;

using namespace llvm;

namespace {

struct ShangTTI : public ImmutablePass, public TargetTransformInfo {
  ShangTTI() : ImmutablePass(ID) {
    initializeShangTTIPass(*PassRegistry::getPassRegistry());
  }

  virtual void initializePass() {
    pushTTIStack(this);
  }

  virtual void finalizePass() {
    popTTIStack();
  }

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    TargetTransformInfo::getAnalysisUsage(AU);
  }

  /// Pass identification.
  static char ID;

  /// Provide necessary pointer adjustments for the two base classes.
  virtual void *getAdjustedAnalysisPointer(const void *ID) {
    if (ID == &TargetTransformInfo::ID)
      return (TargetTransformInfo*)this;
    return this;
  }

  /// \name Scalar TTI Implementations
  /// @{

  virtual bool isLegalAddImmediate(int64_t imm) const;
  virtual bool isLegalICmpImmediate(int64_t imm) const;
  virtual bool isLegalAddressingMode(Type *Ty, GlobalValue *BaseGV,
                                     int64_t BaseOffset, bool HasBaseReg,
                                     int64_t Scale) const;
  virtual bool isTruncateFree(Type *Ty1, Type *Ty2) const;
  virtual bool isTypeLegal(Type *Ty) const;
  virtual unsigned getJumpBufAlignment() const;
  virtual unsigned getJumpBufSize() const;
  virtual bool shouldBuildLookupTables() const;

  /// @}

  /// \name Vector TTI Implementations
  /// @{

  virtual unsigned getNumberOfRegisters(bool Vector) const;
  virtual unsigned getMaximumUnrollFactor() const;
  virtual unsigned getRegisterBitWidth(bool Vector) const;
  virtual unsigned getArithmeticInstrCost(unsigned Opcode, Type *Ty) const;
  virtual unsigned getShuffleCost(ShuffleKind Kind, Type *Tp,
                                  int Index, Type *SubTp) const;
  virtual unsigned getCastInstrCost(unsigned Opcode, Type *Dst,
                                    Type *Src) const;
  virtual unsigned getCFInstrCost(unsigned Opcode) const;
  virtual unsigned getCmpSelInstrCost(unsigned Opcode, Type *ValTy,
                                      Type *CondTy) const;
  virtual unsigned getVectorInstrCost(unsigned Opcode, Type *Val,
                                      unsigned Index) const;
  virtual unsigned getMemoryOpCost(unsigned Opcode, Type *Src,
                                   unsigned Alignment,
                                   unsigned AddressSpace) const;
  virtual unsigned getIntrinsicInstrCost(Intrinsic::ID, Type *RetTy,
                                         ArrayRef<Type*> Tys) const;
  virtual unsigned getNumberOfParts(Type *Tp) const;

  /// @}
};

}

INITIALIZE_AG_PASS(ShangTTI, TargetTransformInfo, "basictti",
                   "Target independent code generator's TTI", true, true, false)
char ShangTTI::ID = 0;

Pass *
vast::createShangTargetTransformInfoPass() {
  return new ShangTTI();
}


bool ShangTTI::isLegalAddImmediate(int64_t imm) const {
  return true;
}

bool ShangTTI::isLegalICmpImmediate(int64_t imm) const {
  return true;
}

bool ShangTTI::isLegalAddressingMode(Type *Ty, GlobalValue *BaseGV,
                                     int64_t BaseOffset, bool HasBaseReg,
                                     int64_t Scale) const {
  return true;
}

bool ShangTTI::isTruncateFree(Type *Ty1, Type *Ty2) const {
  return true;
}

bool ShangTTI::isTypeLegal(Type *Ty) const {
  return Ty->isIntegerTy() || Ty->isPointerTy();
}

unsigned ShangTTI::getJumpBufAlignment() const {
  return 0;
}

unsigned ShangTTI::getJumpBufSize() const {
  return 0;
}

bool ShangTTI::shouldBuildLookupTables() const {
  return false;
}

//===----------------------------------------------------------------------===//
//
// Calls used by the vectorizers.
//
//===----------------------------------------------------------------------===//
unsigned ShangTTI::getNumberOfRegisters(bool Vector) const {
  return 1;
}

unsigned ShangTTI::getRegisterBitWidth(bool Vector) const {
  return 32;
}

unsigned ShangTTI::getMaximumUnrollFactor() const {
  return 1;
}

unsigned ShangTTI::getArithmeticInstrCost(unsigned Opcode, Type *Ty) const {
  switch (Opcode) {
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor: return 0;
  case Instruction::Add:
  case Instruction::Sub: return 1;
  case Instruction::Shl:
  case Instruction::AShr:
  case Instruction::LShr: return 2;
  case Instruction::Mul:  return 3;
  case Instruction::SDiv:
  case Instruction::UDiv:
  case Instruction::SRem:
  case Instruction::URem: return 128;
  }
  
  return 1;
}

unsigned ShangTTI::getShuffleCost(ShuffleKind Kind, Type *Tp, int Index,
                                  Type *SubTp) const {
  return 1;
}

unsigned ShangTTI::getCastInstrCost(unsigned Opcode, Type *Dst,
                                    Type *Src) const {
  return 0;
 }

unsigned ShangTTI::getCFInstrCost(unsigned Opcode) const {
  // Branches are assumed to be predicted.
  return 0;
}

unsigned ShangTTI::getCmpSelInstrCost(unsigned Opcode, Type *ValTy,
                                      Type *CondTy) const {
  return 0;
}

unsigned ShangTTI::getVectorInstrCost(unsigned Opcode, Type *Val,
                                      unsigned Index) const {
  return 1;
}

unsigned ShangTTI::getMemoryOpCost(unsigned Opcode, Type *Src,
                                   unsigned Alignment,
                                   unsigned AddressSpace) const {
  return 8;
}

unsigned ShangTTI::getIntrinsicInstrCost(Intrinsic::ID, Type *RetTy,
                                         ArrayRef<Type *> Tys) const {
  return 0;
}

unsigned ShangTTI::getNumberOfParts(Type *Tp) const {
  return 1;
}
