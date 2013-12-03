//===-- Utilities.h - Utilities Functions for Verilog Backend ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements some utilities functions for Verilog Backend.
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
// Get the bit slice in range (UB, LB].
/// GetBits - Retrieve bits between [LB, UB).
inline uint64_t getBitSlice64(uint64_t x, unsigned UB, unsigned LB = 0) {
  assert(UB - LB <= 64 && UB <= 64 && "Cannot extract bit slice!");
  // If the bit slice contains the whole 64-bit variable, simply return it.
  if (UB == 64 && LB == 0) return x;

  return (x >> LB) & ((uint64_t(1) << (UB - LB)) - 1);
}

inline bool isAllZeros64(uint64_t V, unsigned BitWidth) {
  return getBitSlice64(V, BitWidth) == getBitSlice64(UINT64_C(0), BitWidth);
}

inline bool isAllOnes64(uint64_t V, unsigned BitWidth) {
  return getBitSlice64(V, BitWidth) == getBitSlice64(~UINT64_C(0), BitWidth);

}

inline std::string ShangMangle(const std::string &S) {
  std::string Result;

  for (unsigned i = 0, e = S.size(); i != e; ++i)
    if (isalnum(S[i]) || S[i] == '_') {
      Result += S[i];
    } else {
      Result += '_';
      Result += 'A'+(S[i]&15);
      Result += 'A'+((S[i]>>4)&15);
      Result += '_';
    }
    return Result;
}

// PrintEscapedString - Print each character of the specified string, escaping
// it if it is not printable or if it is an escape char.
inline void PrintEscapedString(const char *Str, unsigned Length,
                               raw_ostream &Out) {
    for (unsigned i = 0; i != Length; ++i) {
      unsigned char C = Str[i];
      if (isprint(C) && C != '\\' && C != '"')
        Out << C;
      else if (C == '\\')
        Out << "\\\\";
      else if (C == '\"')
        Out << "\\\"";
      else if (C == '\t')
        Out << "\\t";
      else
        Out << "\\x" << hexdigit(C >> 4) << hexdigit(C & 0x0F);
    }
}

// PrintEscapedString - Print each character of the specified string, escaping
// it if it is not printable or if it is an escape char.
inline void PrintEscapedString(const std::string &Str, raw_ostream &Out) {
  PrintEscapedString(Str.c_str(), Str.size(), Out);
}

class Module;
class DataLayout;
class SMDiagnostic;
// Allow other pass to run script against the GlobalVariables.
bool runScriptOnGlobalVariables(Module &M, DataLayout *TD,
                                const std::string &Script,
                                SMDiagnostic Err);

bool runScriptOnGlobalVariables(ArrayRef<GlobalVariable*> GVs, DataLayout *TD,
                                const std::string &Script,
                                SMDiagnostic Err);
class VASTModule;
void bindFunctionToScriptEngine(DataLayout &TD, VASTModule *Module);

class VASTModule;
// Bind VASTModule to script engine.
void bindToScriptEngine(const char *name, VASTModule *M);
bool runScriptFile(const std::string &ScriptPath, SMDiagnostic &Err);
bool runScriptStr(const std::string &ScriptStr, SMDiagnostic &Err);
//
unsigned getIntValueFromEngine(ArrayRef<const char*> Path);
float getFloatValueFromEngine(ArrayRef<const char*> Path);
std::string getStrValueFromEngine(ArrayRef<const char*> Path);
std::string getStrValueFromEngine(const char *VariableName);

class SCEV;
class ScalarEvolution;
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
