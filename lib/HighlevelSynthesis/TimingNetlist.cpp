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
#include "ExternalTimingAnalysis.h"

#include "shang/VASTModule.h"
#include "shang/Passes.h"
#include "shang/FUInfo.h"

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/SetOperations.h"
#define DEBUG_TYPE "shang-timing-netlist"
#include "llvm/Support/Debug.h"

using namespace llvm;

TimingNetlist::delay_type
TimingNetlist::getDelay(VASTValue *Src, VASTValue *Dst) const {
  const_path_iterator path_end_at = PathInfo.find(Dst);
  assert(path_end_at != PathInfo.end() && "Path not exist!");
  src_iterator path_start_from = path_end_at->second.find(Src);
  assert(path_start_from != path_end_at->second.end() && "Path not exist!");
  return path_start_from->second;
}

TimingNetlist::delay_type
TimingNetlist::getDelay(VASTValue *Src, VASTValue *Thu, VASTValue *Dst) const {
  if (Thu == 0) return getDelay(Src, Dst);

  delay_type S2T = getDelay(Src, Thu), T2D = getDelay(Thu, Dst);
  return S2T + T2D;
}

TimingNetlist::TimingNetlist() : VASTModulePass(ID) {
  initializeTimingNetlistPass(*PassRegistry::getPassRegistry());
}

char TimingNetlist::ID = 0;

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
void TimingNetlist::buildTimingPath(VASTValue *Thu, VASTSeqValue *Dst,
                                    delay_type MUXDelay) {
  if (VASTOperandList::GetDatapathOperandList(Thu) == 0) {
    if (isa<VASTSeqValue>(Thu)) {
      TimingNetlist::delay_type &d = PathInfo[Dst][Thu];
      d = TNLDelay::max(MUXDelay, d);
    }

    return;
  }

  Estimator->estimateTimingOnTree(Thu);

  // If this expression if not driven by any register, there is not timing path.
  if (src_empty(Thu)) return;

  // Accumulate the delay of the fanin MUX.
  TimingNetlist::delay_type &d = PathInfo[Dst][Thu];
  d = TNLDelay::max(MUXDelay, d);
  Estimator->accumulateDelayFrom(Thu, Dst);
}

bool TimingNetlist::runOnVASTModule(VASTModule &VM) {
  // Create an estimator.
  Estimator = new BlackBoxTimingEstimator(PathInfo);

  typedef VASTModule::seqval_iterator iterator;
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *SVal = I;

    // Calculate the delay of the Fanin MUX.
    delay_type MUXDelay = delay_type(0.0f);

    typedef VASTSeqValue::iterator fanin_iterator;
    for (fanin_iterator FI = SVal->begin(), FE = SVal->end(); FI != FE; ++FI) {
      VASTSeqUse U = *FI;
      // Estimate the delay for each fanin.
      buildTimingPath(VASTValPtr(U).get(), SVal, MUXDelay);
      // And the predicate expression.
      buildTimingPath(VASTValPtr(U.getPred()).get(), SVal, MUXDelay);
      if (VASTValPtr SlotActive = U.getSlotActive())
        buildTimingPath(SlotActive.get(), SVal, MUXDelay);
    }
  }

  //ExternalTimingAnalysis ETA(VM, *this);
  //ETA.runExternalTimingAnalysis();

  DEBUG(dbgs() << "Timing Netlist: \n";
        print(dbgs()););

  delete Estimator;
  return false;
}

void TimingNetlist::print(raw_ostream &OS) const {
  for (const_path_iterator I = path_begin(), E = path_end(); I != E; ++I)
    printPathsTo(OS, *I);
}

void TimingNetlist::printPathsTo(raw_ostream &OS, VASTValue *Dst) const {
  const_path_iterator at = PathInfo.find(Dst);
  assert(at != PathInfo.end() && "DstReg not find!");
  printPathsTo(OS, *at);
}

void TimingNetlist::printPathsTo(raw_ostream &OS, const PathTy &Path) const {
  VASTValue *Dst = Path.first;
  OS << "Dst: ";
  Dst->printAsOperand(OS, false);
  OS << " {\n";
  for (src_iterator I = Path.second.begin(), E = Path.second.end(); I != E; ++I)
  {
    OS.indent(2);
    I->first->printAsOperand(OS, false);
    OS << '(' << I->second.delay << ")\n";
  }
  OS << "}\n";
}
