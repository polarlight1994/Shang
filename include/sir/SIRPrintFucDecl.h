#include "SIR.h"

#include "llvm/IR/Value.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringExtras.h"


using namespace llvm;

//===----------------------------------------------------------------------===//
static std::string buildLiteral(uint64_t Value, unsigned bitwidth, bool isMinValue) {
  std::string ret;
  ret = utostr_32(bitwidth) + '\'';
  if (bitwidth == 1) ret += "b";
  else               ret += "h";
  // Mask the value that small than 4 bit to prevent printing something
  // like 1'hf out.
  if (bitwidth < 4) Value &= (1 << bitwidth) - 1;

  if(isMinValue) {
    ret += utohexstr(Value);
    return ret;
  }

  std::string ss = utohexstr(Value);
  unsigned int uselength = (bitwidth/4) + (((bitwidth&0x3) == 0) ? 0 : 1);
  if(uselength < ss.length())
    ss = ss.substr(ss.length() - uselength, uselength);
  ret += ss;

  return ret;
}

static std::string Mangle(const std::string &S) {
  std::string Result;

  for (unsigned i = 0, e = S.size(); i != e; ++i) {
    if (isalnum(S[i]) || S[i] == '_') {
      Result += S[i];
    } else {
      Result += '_';
    }
  }

  return Result;
}

static std::string BitRange(unsigned UB, unsigned LB = 0, bool printOneBit = false) {
  std::string ret;
  assert(UB && UB > LB && "Bad bit range!");
  --UB;
  if (UB != LB)
    ret = "[" + utostr_32(UB) + ":" + utostr_32(LB) + "]";
  else if(printOneBit)
    ret = "[" + utostr_32(LB) + "]";

  return ret;
}

static void printName(raw_ostream &OS, Instruction &I) {
  OS << Mangle(I.getName());
}





//===----------------------------------------------------------------------===//