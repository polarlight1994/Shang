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
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Compiler.h"

namespace llvm {
inline unsigned getByteEnable(unsigned SizeInBytes) {
  return (0x1 << SizeInBytes) - 1;
}
// Get the bit slice in range (UB, LB].
/// GetBits - Retrieve bits between [LB, UB).
inline uint64_t getBitSlice64(uint64_t x, unsigned UB, unsigned LB = 0) {
  assert(UB - LB <= 64 && UB <= 64 && "Cannot extract bit slice!");
  // If the bit slice contains the whole 64-bit variable, simply return it.
  if (UB == 64 && LB == 0) return x;

  return (x >> LB) & ((uint64_t(1) << (UB - LB)) - 1);
}

/// SignExtend64 - Sign extend B-bit number x to 64-bit int.
/// Usage int64_t r = SignExtend64<5>(x);
inline int64_t SignExtend64(uint64_t x, unsigned SizeInBits) {
  return int64_t(x << (64 - SizeInBits)) >> (64 - SizeInBits);
}

inline bool isAllZeros64(uint64_t V, unsigned BitWidth) {
  return getBitSlice64(V, BitWidth) == getBitSlice64(UINT64_C(0), BitWidth);
}

inline bool isAllOnes64(uint64_t V, unsigned BitWidth) {
  return getBitSlice64(V, BitWidth) == getBitSlice64(~UINT64_C(0), BitWidth);

}

inline std::string VBEMangle(const std::string &S) {
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

template <typename X, typename Y>
inline X pair_first(std::pair<X, Y> P) { return P.first; }

template <typename X, typename Y>
inline Y pair_second(std::pair<X, Y> P) { return P.second; }

class Module;
class TargetData;
class SMDiagnostic;
// Allow other pass to run script against the GlobalVariables.
bool runScriptOnGlobalVariables(Module &M, TargetData *TD,
                                const std::string &Script,
                                SMDiagnostic Err);
class MachineFunction;
class VASTModule;
void bindFunctionInfoToScriptEngine(MachineFunction &MF, TargetData &TD,
                                    VASTModule *Module);

class VASTModule;
// Bind VASTModule to script engine.
void bindToScriptEngine(const char *name, VASTModule *M);
bool runScriptFile(const std::string &ScriptPath, SMDiagnostic &Err);
bool runScriptStr(const std::string &ScriptStr, SMDiagnostic &Err);
//
unsigned getIntValueFromEngine(ArrayRef<const char*> Path);
std::string getStrValueFromEngine(ArrayRef<const char*> Path);

class MachineMemOperand;
class ScalarEvolution;
class SCEV;
// Alias Analysis.
AliasAnalysis::AliasResult
MachineMemOperandAlias(MachineMemOperand* V1, MachineMemOperand *V2,
                       AliasAnalysis *AA, ScalarEvolution *SE);

const SCEV *getMachineMemOperandSCEV(MachineMemOperand *V, ScalarEvolution *SE);

const SCEV *getAddressDeltaSCEV(MachineMemOperand *LHS, MachineMemOperand *RHS,
                                ScalarEvolution *SE);

int64_t getAddressDelta(MachineMemOperand *LHS, MachineMemOperand *RHS,
                        ScalarEvolution *SE);

static inline
bool isMachineMemOperandAlias(MachineMemOperand* V1, MachineMemOperand *V2,
                              AliasAnalysis *AA, ScalarEvolution *SE) {
  return MachineMemOperandAlias(V1, V2, AA, SE) != AliasAnalysis::NoAlias;
}

std::pair<const Value*, int64_t>
extractPointerAndOffset(const Value *V, int64_t Offset);

// Loop dependency Analysis.
int getLoopDepDist(bool SrcBeforeDest, int Distance = 0);

int getLoopDepDist(const SCEV *SSAddr, const SCEV *SDAddr,
                   bool SrcLoad, unsigned ElemSizeInByte, ScalarEvolution *SE);

class MachineBasicBlock;
class TargetInstrInfo;
void fixTerminators(MachineBasicBlock *MBB);
void fixTerminators(MachineFunction &MF);

class MachineDominatorTree;
class MachineInstr;
class MachineRegisterInfo;
bool hoistDatapathOp(MachineInstr *MI, MachineDominatorTree  *DT,
                     MachineRegisterInfo *MRI);

bool hoistDatapathOpInMBB(MachineBasicBlock *MBB, MachineDominatorTree *DT,
                          MachineRegisterInfo *MRI);

bool hoistDatapathOpInSuccs(MachineBasicBlock *MBB, MachineDominatorTree *DT,
                            MachineRegisterInfo *MRI);
}

#endif
