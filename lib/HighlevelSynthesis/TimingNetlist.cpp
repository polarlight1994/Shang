//=--- TimingNetlist.cpp - The Netlist for Delay Estimation -------*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the interface timing netlist.
//
//===----------------------------------------------------------------------===//

#include "TimingNetlist.h"
#include "TimingEstimator.h"

#include "shang/VASTModule.h"
#include "shang/Passes.h"
#include "shang/FUInfo.h"

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/SetOperations.h"
#define DEBUG_TYPE "shang-timing-netlist"
#include "llvm/Support/Debug.h"

using namespace llvm;

void TimingNetlist::annotateDelay(VASTSeqValue *Src, VASTValue *Dst,
                                  delay_type delay) {
  PathInfoTy::iterator path_end_at = PathInfo.find(Dst);
  assert(path_end_at != PathInfo.end() && "Path not exist!");
  SrcInfoTy::iterator path_start_from = path_end_at->second.find(Src);
  assert(path_start_from != path_end_at->second.end() && "Path not exist!");
  path_start_from->second = delay / VFUs::Period;
}

TimingNetlist::delay_type
TimingNetlist::getDelay(VASTSeqValue *Src, VASTValue *Dst) const {
  PathInfoTy::const_iterator path_end_at = PathInfo.find(Dst);
  assert(path_end_at != PathInfo.end() && "Path not exist!");
  SrcInfoTy::const_iterator path_start_from = path_end_at->second.find(Src);
  assert(path_start_from != path_end_at->second.end() && "Path not exist!");
  return path_start_from->second;
}

void TimingNetlist::createDelayEntry(VASTValue *Dst, VASTSeqValue *Src) {
  assert(Src && Dst && "Bad pointer!");

  PathInfo[Dst][Src] = 0;
}

void TimingNetlist::createPathFromSrc(VASTValue *Dst, VASTValue *Src) {
  assert(Dst != Src && "Unexpected cycle!");

  // Forward the Src terminator of the path from SrcReg.
  PathInfoTy::iterator at = PathInfo.find(Src);

  // No need to worry about this path.
  if (at == PathInfo.end()) return;


  // Otherwise forward the source nodes reachable to SrcReg to DstReg.
  set_union(PathInfo[Dst], at->second);
}

TimingNetlist::TimingNetlist() : VASTModulePass(ID) {
  initializeTimingNetlistPass(*PassRegistry::getPassRegistry());
}

char TimingNetlist::ID = 0;
char &llvm::TimingNetlistID = TimingNetlist::ID;

INITIALIZE_PASS(TimingNetlist, "shang-timing-netlist",
                "Preform Timing Estimation on the RTL Netlist",
                false, true)

Pass *llvm::createTimingNetlistPass() {
  return new TimingNetlist();
}

void TimingNetlist::releaseMemory() {
  PathInfo.clear();
}

void TimingNetlist::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.setPreservesAll();
}

//===----------------------------------------------------------------------===//
bool TimingNetlist::runOnVASTModule(VASTModule &VM) {
  // Create an estimator.
  OwningPtr<TimingEstimatorBase>
    Estimator(TimingEstimatorBase::CreateBlackBoxModel());

  typedef VASTModule::seqval_iterator iterator;
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *SeqVal = I;
    typedef VASTSeqValue::itertor fanin_iterator;
    for (fanin_iterator FI = SeqVal->begin(), FE = SeqVal->end(); FI != FE; ++FI)
    {
      VASTValPtr Fanin = (*FI);
      Estimator->estimateTimingOnTree(Fanin.get());
    }
  }

  return false;
}
