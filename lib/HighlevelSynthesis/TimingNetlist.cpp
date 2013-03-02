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
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "shang-timing-netlist"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<enum TimingEstimatorBase::ModelType>
TimingModel("timing-model", cl::Hidden,
            cl::desc("The Timing Model of the Delay Estimator"),
            cl::values(
  clEnumValN(TimingEstimatorBase::Bitlevel, "bit-level",
            "Bit-level delay estimation with linear approximation"),
  clEnumValN(TimingEstimatorBase::BlackBox, "blackbox",
            "Only accumulate the critical path delay of each FU"),
  clEnumValN(TimingEstimatorBase::ZeroDelay, "zero-delay",
            "Assume all datapath delay is zero"),
  clEnumValEnd));

void TNLDelay::print(raw_ostream &OS) const {
  OS << "MSB: " << getMSBLL() << " LSB: " << getLSBLL() << " Delay(ns): "
     << getDelay();
}

void TNLDelay::dump() const {
  print(dbgs());
}

TimingNetlist::delay_type
TimingNetlist::getDelay(VASTValue *Src, VASTValue *Dst) const {
  // TODO:
  //if (VASTSeqValue *SVal = dyn_cast<VASTSeqValue>(Dst)) {
  //  for each fanin fi of Dst,
  //    get the CRITICAL path delay from Src to fi
  //    max reduction

  //  return CRITICAL delay to all fanins + Mux delay?
  //}

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
  return S2T.addLLParallel(T2D);
}

TimingNetlist::TimingNetlist() : VASTModulePass(ID),
  Estimator(new BitlevelDelayEsitmator(PathInfo, TimingModel))
{
  initializeTimingNetlistPass(*PassRegistry::getPassRegistry());
}

TimingNetlist::~TimingNetlist() {
  delete Estimator;
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
void TimingNetlist::buildTimingPathToReg(VASTValue *Thu, VASTSeqValue *Dst,
                                         delay_type MUXDelay) {
  if (VASTOperandList::GetDatapathOperandList(Thu) == 0) {
    if (isa<VASTSeqValue>(Thu)) {
      TimingNetlist::delay_type &d = PathInfo[Dst][Thu];
      d = TNLDelay::max(MUXDelay, d);
    }

    return;
  }

  // We need to handle the wires here.
  if (isa<VASTWire>(Thu))
    Estimator->estimateTimingOnTree(Thu);

  // If this expression if not driven by any register, there is not timing path.
  if (src_empty(Thu)) return;

  // Accumulate the delay of the fanin MUX.
  TimingNetlist::delay_type &d = PathInfo[Dst][Thu];
  d = TNLDelay::max(MUXDelay, d);
  Estimator->accumulateDelayFrom(Thu, Dst);
}

TNLDelay TimingNetlist::getMuxDelay(unsigned Fanins, unsigned Bitwidth) const {
  VFUMux *Mux = getFUDesc<VFUMux>();

  unsigned MUXLL = 0;

  if (TimingModel != TimingEstimatorBase::ZeroDelay)
    MUXLL = Mux->getMuxLogicLevels(Fanins, Bitwidth);

  return delay_type(MUXLL, MUXLL);
}

bool TimingNetlist::runOnVASTModule(VASTModule &VM) {
  // Build the timing path for datapath nodes.
  typedef DatapathContainer::expr_iterator expr_iterator;
  for (expr_iterator I = VM->expr_begin(), E = VM->expr_end(); I != E; ++I)
    if (!I->use_empty()) Estimator->estimateTimingOnTree(I);
  
  // Build the timing path for registers.
  typedef VASTModule::seqval_iterator iterator;
  for (iterator I = VM.seqval_begin(), E = VM.seqval_end(); I != E; ++I) {
    VASTSeqValue *SVal = I;

    // Calculate the delay of the Fanin MUX.
    delay_type MUXDelay = getMuxDelay(SVal->size(), SVal->getBitWidth());

    if (SVal->isSelectorSynthesized()) {
      typedef VASTSeqValue::fanin_iterator fanin_iterator;
      for (fanin_iterator I = SVal->fanin_begin(), E = SVal->fanin_end();
           I != E; ++I){
        const VASTSeqValue::Fanin *FI = *I;

        buildTimingPathToReg(FI->Pred.unwrap().get(), SVal, MUXDelay);
        buildTimingPathToReg(FI->FI.unwrap().get(), SVal, MUXDelay);
      }
      continue;
    } // else

    typedef VASTSeqValue::iterator fanin_iterator;
    for (fanin_iterator FI = SVal->begin(), FE = SVal->end(); FI != FE; ++FI) {
      VASTLatch U = *FI;
      // Estimate the delay for each fanin.
      buildTimingPathToReg(VASTValPtr(U).get(), SVal, MUXDelay);
      // And the predicate expression.
      buildTimingPathToReg(VASTValPtr(U.getPred()).get(), SVal, MUXDelay);
      if (VASTValPtr SlotActive = U.getSlotActive())
        buildTimingPathToReg(SlotActive.get(), SVal, MUXDelay);
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

void TimingNetlist::dumpPathsTo(VASTValue *Dst) const {
  printPathsTo(dbgs(), Dst);
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
    OS << '(' << I->second << ")\n";
  }
  OS << "}\n";
}
