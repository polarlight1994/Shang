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
TimingNetlist::getDelay(VASTSeqValue *Src, VASTValue *Dst) const {
  const_path_iterator path_end_at = PathInfo.find(Dst);
  assert(path_end_at != PathInfo.end() && "Path not exist!");
  src_iterator path_start_from = path_end_at->second.find(Src);
  assert(path_start_from != path_end_at->second.end() && "Path not exist!");
  return path_start_from->second;
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
bool TimingNetlist::runOnVASTModule(VASTModule &VM) {
  // Create an estimator.
  TimingEstimatorBase *Estimator = new ZeroDelayEstimator(PathInfo);

  typedef VASTModule::seqval_iterator iterator;
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *SVal = I;

    typedef VASTSeqValue::iterator fanin_iterator;
    for (fanin_iterator FI = SVal->begin(), FE = SVal->end(); FI != FE; ++FI) {
      // Estimate the delay for each fanin.
      VASTValue *Fanin = VASTValPtr(*FI).get();
      if (VASTOperandList::GetDatapathOperandList(Fanin) == 0) {
        if (VASTSeqValue *SVal = dyn_cast<VASTSeqValue>(Fanin)) {
          // Create an entry from SVal.
          TimingNetlist::delay_type &d = PathInfo[Fanin][SVal];
          d = TNLDelay::max(TimingNetlist::delay_type(), d);
        }
        continue;
      }

      Estimator->estimateTimingOnTree(Fanin);

      // Accumulate the delay of the fanin MUX.
    }
  }

  //ExternalTimingAnalysis ETA(VM, *this);
  //ETA.runExternalTimingAnalysis();

  DEBUG(dbgs() << "Timing Netlist: \n";
        print(dbgs()););

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
