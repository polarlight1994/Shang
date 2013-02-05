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
    Estimator(TimingEstimatorBase::CreateZeroDelayModel());

  typedef VASTModule::seqval_iterator iterator;
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *SVal = I;

    typedef VASTSeqValue::itertor fanin_iterator;
    for (fanin_iterator FI = SVal->begin(), FE = SVal->end(); FI != FE; ++FI) {
      // Estimate the delay for each fanin.
      VASTValue *Fanin = VASTValPtr(*FI).get();
      if (VASTOperandList::GetDatapathOperandList(Fanin) == 0) {
        if (VASTSeqValue *SVal = dyn_cast<VASTSeqValue>(Fanin)) {
          // Create an entry from SVal.
          TimingNetlist::delay_type &d = PathInfo[Fanin][SVal];
          d = std::max(TimingNetlist::delay_type(), d);
        }
        continue;
      }

      SrcInfoTy &SrcInfo = PathInfo[Fanin];
      if (!SrcInfo.empty()) continue;

      Estimator->estimateTimingOnTree(Fanin, SrcInfo);

      // Accumulate the delay of the fanin MUX.
    }
  }

  ExternalTimingAnalysis ETA(VM, *this);
  ETA.runExternalTimingAnalysis();

  dbgs() << "Timing Netlist: \n";
  print(dbgs());

  return false;
}

void TimingNetlist::print(raw_ostream &OS) const {
  for (const_path_iterator I = path_begin(), E = path_end(); I != E; ++I)
    printPathsTo(OS, *I);
}

void TimingNetlist::printPathsTo(raw_ostream &OS, VASTValue *Dst) const {
  PathInfoTy::const_iterator at = PathInfo.find(Dst);
  assert(at != PathInfo.end() && "DstReg not find!");
  printPathsTo(OS, *at);
}

void TimingNetlist::printPathsTo(raw_ostream &OS,
                                 const PathInfoTy::value_type &Path) const {
  VASTValue *Dst = Path.first;
  OS << "Dst: ";
  Dst->printAsOperand(OS, false);
  OS << " {\n";
  for (src_iterator I = Path.second.begin(), E = Path.second.end(); I != E; ++I)
  {
    OS.indent(2);
    I->first->printAsOperand(OS, false);
    OS << '(' << I->second << ")\n";
  }
  OS << "}\n";
}
