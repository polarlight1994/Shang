//=- ObjectBasedAliasAnalysis.cpp - Underlying Objects Based Alias Analyasis-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the Underlying Objects Based implementation of the
// Alias Analysis interface.
//
//===----------------------------------------------------------------------===//
#include "vast/Passes.h"

#include "llvm/Pass.h"
#include "llvm/IR/Constants.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/AliasAnalysis.h"
#define DEBUG_TYPE "object-based-aa"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct ObjectBasedAliasAnalyais : public ImmutablePass, public AliasAnalysis {
  static char ID;

  ObjectBasedAliasAnalyais() : ImmutablePass(ID) {
    initializeObjectBasedAliasAnalyaisPass(*PassRegistry::getPassRegistry());
  }

  void initializePass() {
    InitializeAliasAnalysis(this);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AliasAnalysis::getAnalysisUsage(AU);
  }

  /// getAdjustedAnalysisPointer - This method is used when a pass implements
  /// an analysis interface through multiple inheritance.  If needed, it
  /// should override this to adjust the this pointer as needed for the
  /// specified pass info.
  void *getAdjustedAnalysisPointer(const void *ID) {
    if (ID == &AliasAnalysis::ID)
      return (AliasAnalysis*)this;
    return this;
  }

  AliasResult alias(const Location &LocA, const Location &LocB);
};
}

char ObjectBasedAliasAnalyais::ID = 0;

Pass *vast::createObjectBasedAliasAnalyaisPass() {
  return new ObjectBasedAliasAnalyais();
}

INITIALIZE_AG_PASS(ObjectBasedAliasAnalyais, AliasAnalysis, "objectaa",
                   "Underlying Object Based Alias Analysis",
                   false, true, false)

bool isNullOrUndef(const Value *V) {
  return isa<ConstantPointerNull>(V) || isa<UndefValue>(V);
}

AliasAnalysis::AliasResult
ObjectBasedAliasAnalyais::alias(const Location &LocA, const Location &LocB) {

  SmallVector<Value*, 8> AObjects, BObjects;

  GetUnderlyingObjects(const_cast<Value*>(LocA.Ptr), AObjects, getDataLayout());
  GetUnderlyingObjects(const_cast<Value*>(LocB.Ptr), BObjects, getDataLayout());

  // LocA and LocB does not alias each other if all their underlying objects not
  // alias.
  typedef SmallVector<Value*, 8>::const_iterator iterator;
  for (iterator AI = AObjects.begin(), AE = AObjects.end(); AI != AE; ++AI) {
    Value *APtr = *AI;
    // Ignore the Nulls and UndefValues, memory access behaviors though such
    // objects are known as undefined and can assume no alias.
    if (isNullOrUndef(APtr)) continue;

    Location A(APtr, UnknownSize);
    for (iterator BI = BObjects.begin(), BE = BObjects.end(); BI != BE; ++BI) {
      Value *BPtr = *BI;
      // Ignore the Nulls and UndefValues, memory access behaviors though such
      // objects are known as undefined and can assume no alias.
      if (isNullOrUndef(BPtr)) continue;

      Location B(BPtr, UnknownSize);
      AliasResult Result = AliasAnalysis::alias(A, B);
      // Bail out if any of the underlying object alias.
      if (Result != AliasAnalysis::NoAlias)
        return AliasAnalysis::alias(LocA, LocB);
    }
  }

  return AliasAnalysis::NoAlias;
}
