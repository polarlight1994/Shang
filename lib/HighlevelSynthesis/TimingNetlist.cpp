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

#include "shang/VASTMemoryPort.h"
#include "shang/VASTModule.h"
#include "shang/Passes.h"
#include "shang/FUInfo.h"

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "shang-timing-netlist"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<enum TimingNetlist::ModelType>
TimingModel("timing-model", cl::Hidden,
            cl::desc("The Timing Model of the Delay Estimator"),
            cl::values(
  clEnumValN(TimingNetlist::External, "external",
            "Perform delay estimation with synthesis tool"),
  clEnumValN(TimingNetlist::BlackBox, "blackbox",
            "Only accumulate the critical path delay of each FU"),
  clEnumValN(TimingNetlist::ZeroDelay, "zero-delay",
            "Assume all datapath delay is zero"),
  clEnumValEnd));

TimingNetlist::delay_type
TimingNetlist::getDelay(VASTValue *Src, VASTSelector *Dst) const {
  const_fanin_iterator at = FaninInfo.find(Dst);
  assert(at != FaninInfo.end() && "Path not exist!");

  src_iterator path_start_from = at->second.find(Src);
  assert(path_start_from != at->second.end() && "Path not exist!");
  return path_start_from->second;
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
TimingNetlist::getDelay(VASTValue *Src, VASTValue *Thu,
                        VASTSelector *Dst) const {
  if (Thu == 0) return getDelay(Src, Dst);

  delay_type S2T = getDelay(Src, Thu), T2D = getDelay(Thu, Dst);
  return S2T + T2D;
}

TimingNetlist::delay_type
TimingNetlist::accumulateSelDelay(VASTSelector *Sel, VASTSeqValue *V,
                                  VASTValue *Thu, delay_type delay) {
  if (Sel)  delay += getDelay(Thu, Sel);

  return delay;
}

void TimingNetlist::extractDelay(VASTSelector *Sel, VASTValue *Src,
                                 RegDelaySet &Set) {
  const_path_iterator cone_rooted_on = PathInfo.find(Src);
  if (cone_rooted_on == PathInfo.end()) {
    // Handle the trivial path.
    if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(Src)) {
      delay_type &OldDelay = Set[SeqVal];
      OldDelay = std::max(accumulateSelDelay(Sel, SeqVal, Src, delay_type(0.0f)),
                          OldDelay);
    }

    return;
  }

  // Extract the delay from the leaves of the cone.
  const SrcDelayInfo &Srcs = cone_rooted_on->second;
  for (const_src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I){
    if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(I->first)) {
      delay_type &OldDelay = Set[SeqVal];
      OldDelay = std::max(accumulateSelDelay(Sel, SeqVal, Src, I->second),
                          OldDelay);
    }
  }
}

void
TimingNetlist::annotateDelay(VASTValue *Src, VASTValue *Dst, delay_type delay) {
  delay_type &old_delay = PathInfo[Dst][Src];
  old_delay = std::max(old_delay, delay);
}

void
TimingNetlist::annotateDelay(VASTValue *Src, VASTSelector *Dst, delay_type delay)
{
  delay_type &old_delay = FaninInfo[Dst][Src];
  old_delay = std::max(old_delay, delay);
}

TimingNetlist::TimingNetlist() : VASTModulePass(ID) {
  initializeTimingNetlistPass(*PassRegistry::getPassRegistry());
}

char TimingNetlist::ID = 0;

INITIALIZE_PASS_BEGIN(TimingNetlist, "shang-timing-netlist",
                      "Preform Timing Estimation on the RTL Netlist",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
INITIALIZE_PASS_END(TimingNetlist, "shang-timing-netlist",
                    "Preform Timing Estimation on the RTL Netlist",
                    false, true)

Pass *llvm::createTimingNetlistPass() {
  return new TimingNetlist();
}

void TimingNetlist::releaseMemory() {
  PathInfo.clear();
  FaninInfo.clear();
}

void TimingNetlist::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  // Perform the control logic synthesis because we need to write the netlist.
  if (TimingModel == TimingNetlist::External) {
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addRequiredID(DatapathNamerID);
  }

  AU.setPreservesAll();
}

//===----------------------------------------------------------------------===//
void TimingNetlist::buildTimingPathTo(VASTValue *Thu, VASTSelector *Dst,
                                      delay_type MUXDelay) {
  if (!isa<VASTExpr>(Thu)) {
    if (isa<VASTSeqValue>(Thu)) {
      TimingNetlist::delay_type &OldDelay = FaninInfo[Dst][Thu];
      OldDelay = std::max(MUXDelay, OldDelay);
    }

    return;
  }

  path_iterator at = PathInfo.find(Thu);
  if (at == PathInfo.end()) return;

  TimingNetlist::delay_type &OldDelay = FaninInfo[Dst][Thu];
  OldDelay =std::max(OldDelay, MUXDelay);
  
  SrcDelayInfo &Srcs = at->second;

  // If this expression if not driven by any register, there is not timing path.
  for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    VASTValue *Src = I->first;
    TimingNetlist::delay_type NewDelay = I->second;
    // Accumulate the delay of the fanin MUX.
    NewDelay += MUXDelay;

    TimingNetlist::delay_type &OldDelay = FaninInfo[Dst][Src];
    OldDelay =std::max(OldDelay, NewDelay);
  }  
}

TimingNetlist::delay_type
TimingNetlist::getSelectorDelayImpl(unsigned NumFannins, VASTSelector *Sel) const {
  float MUXDelay = 0.0f;

  if (TimingModel != TimingNetlist::ZeroDelay) {
    VFUMux *Mux = getFUDesc<VFUMux>();
    MUXDelay = Mux->getMuxLatency(NumFannins);
    if (Sel) {
      // Also accumulate the delay of the block RAM.
      if (VASTMemoryBus *Mem = dyn_cast<VASTMemoryBus>(Sel->getParent())) {
        if (!Mem->requireByteEnable()) {
          VFUBRAM *RAM = getFUDesc<VFUBRAM>();
          MUXDelay += RAM->Latency;
        }
      }
    }
  }

  return delay_type(MUXDelay);
}

bool TimingNetlist::runOnVASTModule(VASTModule &VM) {
  if (TimingModel == TimingNetlist::External) {
    if (!performExternalAnalysis(VM))
      report_fatal_error("External timing analysis fail!");

    return false;
  }
  
  BitlevelDelayEsitmator Estimator(PathInfo, TimingModel);

  // Build the timing path for datapath nodes.
  typedef DatapathContainer::expr_iterator expr_iterator;
  for (expr_iterator I = VM->expr_begin(), E = VM->expr_end(); I != E; ++I)
    if (!I->use_empty()) Estimator.estimateTimingOnCone(I);
  
  // Build the timing path for registers.
  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    VASTSelector *Sel = I;
    // Calculate the delay of the Fanin MUX.
    delay_type MUXDelay = getSelectorDelayImpl(Sel->size(), Sel);
    
    if (Sel->isSelectorSynthesized()) {
      typedef VASTSelector::fanin_iterator fanin_iterator;
      for (fanin_iterator I = Sel->fanin_begin(), E = Sel->fanin_end();
           I != E; ++I){
        const VASTSelector::Fanin *FI = *I;

        buildTimingPathTo(FI->Cnd.unwrap().get(), Sel, MUXDelay);
        buildTimingPathTo(FI->FI.unwrap().get(), Sel, MUXDelay);
      }

      buildTimingPathTo(Sel->getEnable().get(), Sel, MUXDelay);

      // Dirty HACK: Also run on the Latching operation of the registers, so that
      // we can build the timing path for the SlotActive wires.
      // continue;
    } // else

    typedef VASTSelector::iterator fanin_iterator;
    for (fanin_iterator FI = Sel->begin(), FE = Sel->end(); FI != FE; ++FI) {
      VASTLatch U = *FI;
      // Estimate the delay for each fanin.
      buildTimingPathTo(VASTValPtr(U).get(), Sel, MUXDelay);
      // And the predicate expression.
      buildTimingPathTo(VASTValPtr(U.getGuard()).get(), Sel, MUXDelay);
      if (VASTValPtr SlotActive = U.getSlotActive())
        buildTimingPathTo(SlotActive.get(), Sel, MUXDelay);
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
