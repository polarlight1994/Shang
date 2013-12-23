//===-------------- Utilities.h - Utilities Functions -----------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements some utilities functions
//
//===----------------------------------------------------------------------===//
#ifndef VTM_UTILITIES_H
#define VTM_UTILITIES_H
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Compiler.h"

namespace llvm {
class SCEV;
class ScalarEvolution;
}

namespace vast {
using namespace llvm;

// Loop dependency Analysis.
int getLoopDepDist(bool SrcBeforeDest, int Distance = 0);

int getLoopDepDist(const SCEV *SSAddr, const SCEV *SDAddr,
                   bool SrcLoad, unsigned ElemSizeInByte, ScalarEvolution *SE);

inline bool isCall(const Instruction *Inst, bool IgnoreExternalCall = true) {
  const CallInst *CI = dyn_cast<CallInst>(Inst);

  if (CI == 0) return false;

  // Ignore the trivial intrinsics.
  if (const IntrinsicInst *Intr = dyn_cast<IntrinsicInst>(CI)) {
    switch (Intr->getIntrinsicID()) {
    default: break;
    case Intrinsic::uadd_with_overflow:
    case Intrinsic::lifetime_end:
    case Intrinsic::lifetime_start:
    case Intrinsic::bswap: return false;
    }
  }

  // Ignore the call to external function, they are ignored in the VAST and do
  // not have a corresponding VASTSeqInst.
  if (CI->getCalledFunction()->isDeclaration() && IgnoreExternalCall)
    return false;

  return true;
}

inline bool isLoadStore(const Instruction *Inst) {
  return isa<LoadInst>(Inst) || isa<StoreInst>(Inst);
}

inline Value *getPointerOperand(Instruction *I) {
  if (LoadInst *L = dyn_cast<LoadInst>(I))
    return L->getPointerOperand();

  if (StoreInst *S = dyn_cast<StoreInst>(I))
    return S->getPointerOperand();

  return 0;
}

inline bool isChainingCandidate(const Value *V) {
  // Do not perform (single-cycle) chaining across load/store or call, which
  // requires special functional units.
  // Also do not chain with PHI either, otherwise we will break the SSA form.
  // At last, do not chain with instruction that do not define any value!
  if (const Instruction *Inst = dyn_cast_or_null<Instruction>(V))
    return !(isLoadStore(Inst) || isCall(Inst, false)
             || isa<PHINode>(Inst) || Inst->isTerminator()
             || Inst->getOpcode() == Instruction::UDiv
             || Inst->getOpcode() == Instruction::SDiv
             || Inst->getOpcode() == Instruction::URem
             || Inst->getOpcode() == Instruction::SRem);

  return false;
}
}

#endif
