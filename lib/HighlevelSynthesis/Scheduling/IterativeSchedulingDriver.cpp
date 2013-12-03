//===-- IterativeScheduling.cpp - the Iterative Scheduling Pass -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the IterativeScheduling pass.
// The IterativeScheduling pass repeat the process of "schedule then update
// estimation" multiple times to achieve timing closure.
//
//===----------------------------------------------------------------------===//
//

#include "VASTScheduling.h"

#include "vast/Dataflow.h"
#include "vast/Passes.h"
#include "vast/Strash.h"
#include "vast/VASTModule.h"
#include "vast/VASTModulePass.h"

#include "llvm/PassManagers.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/SourceMgr.h"
#define DEBUG_TYPE "shang-iterative-scheduling"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<unsigned> MaxIteration("shang-max-scheduling-iteration",
  cl::desc("Perform memory optimizations e.g. coalescing or banking"),
  cl::init(1));

static cl::opt<bool> DumpIntermediateNetlist("shang-dump-intermediate-netlist",
  cl::desc("Dump the netlists produced by each iteration"),
  cl::init(false));

namespace {

class IterativeScheduling : public VASTModulePass, public PMDataManager {
  const std::string OriginalRTLOutput, OriginalMCPOutput;
  const StringRef RTLFileName;
public:
  static char ID;
  IterativeScheduling() : VASTModulePass(ID), PMDataManager(),
    OriginalRTLOutput(getStrValueFromEngine("RTLOutput")),
    OriginalMCPOutput(getStrValueFromEngine("MCPDataBase")),
    RTLFileName(sys::path::filename(OriginalRTLOutput)) {
    initializeIterativeSchedulingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<DataflowAnnotation>();
    AU.addPreserved<DataflowAnnotation>();
  }

  void recoverOutputPath();
  void changeOutputPaths(unsigned i);

  bool runSingleIteration(Function &F) {
    bool Changed = false;

    // Collect inherited analysis from Module level pass manager.
    populateInheritedAnalysis(TPM->activeStack);

    for (unsigned Index = 0; Index < getNumContainedPasses(); ++Index) {
      FunctionPass *FP = getContainedPass(Index);
      bool LocalChanged = false;

      dumpPassInfo(FP, EXECUTION_MSG, ON_FUNCTION_MSG, F.getName());
      dumpRequiredSet(FP);

      initializeAnalysisImpl(FP);

      {
        PassManagerPrettyStackEntry X(FP, F);
        TimeRegion PassTimer(getPassTimer(FP));

        LocalChanged |= FP->runOnFunction(F);
      }

      Changed |= LocalChanged;
      if (LocalChanged)
        dumpPassInfo(FP, MODIFICATION_MSG, ON_FUNCTION_MSG, F.getName());
      dumpPreservedSet(FP);

      verifyPreservedAnalysis(FP);
      removeNotPreservedAnalysis(FP);
      recordAvailableAnalysis(FP);
      removeDeadPasses(FP, F.getName(), ON_FUNCTION_MSG);
    }

    return Changed;
  }

  bool runOnVASTModule(VASTModule &VM) {
    if (MaxIteration == 0) return false;

    Function &F = VM.getLLVMFunction();

    for (unsigned i = 0, e = MaxIteration - 1; i < e; ++i) {
      // Redirect the output path for the intermediate netlist.
      if (DumpIntermediateNetlist) changeOutputPaths(i);

      runSingleIteration(F);
      rebuildModule();
    }

    // Fix the output pass if necessary.
    if (DumpIntermediateNetlist) recoverOutputPath();

    // Last iteration, we will stop at scheduling.
    while (!PassVector.empty() &&
           PassVector.back()->getPassID() != &VASTScheduling::ID) {
      delete PassVector.pop_back_val();
    }

    runSingleIteration(F);

    return true;
  }

  void assignPassManager(PMStack &PMS, PassManagerType T) {
    FunctionPass::assignPassManager(PMS, T);

    // Inherit the existing analyses
    populateInheritedAnalysis(PMS);

    // Setup the pass managers stack.
    PMDataManager *PMD = PMS.top();
    PMTopLevelManager *TPM = PMD->getTopLevelManager();
    TPM->addIndirectPassManager(this);
    PMS.push(this);

    schedulePass(createVASTSchedulingPass());
    // Do not need to add other subpasses if we are not iterate at all.
    if (MaxIteration <= 1)
      return;

    // Add the passes for each single iteration.
    schedulePass(createLUTMappingPass());
    schedulePass(new DataflowAnnotation(true));

    // Finish the whole HLS process if we want to dump the intermediate netlist
    // for each iteration.
    if (DumpIntermediateNetlist) {
      //schedulePass(createRegisterSharingPass());
      schedulePass(createSelectorPipeliningPass());
      schedulePass(createRTLCodeGenPass());
      schedulePass(createTimingScriptGenPass());
    }
  }

  VASTModulePass *getContainedPass(unsigned N) {
    assert ( N < PassVector.size() && "Pass number out of range!");
    VASTModulePass *FP = static_cast<VASTModulePass *>(PassVector[N]);
    return FP;
  }

  /// cleanup - After running all passes, clean up pass manager cache.
  void cleanup() {
    for (unsigned Index = 0; Index < getNumContainedPasses(); ++Index) {
      FunctionPass *FP = getContainedPass(Index);
      AnalysisResolver *AR = FP->getResolver();
      assert(AR && "Analysis Resolver is not set");
      AR->clearAnalysisImpls();
    }
  }

  void schedulePass(Pass *P);

  virtual PMDataManager *getAsPMDataManager() { return this; }
  virtual Pass *getAsPass() { return this; }

  virtual const char *getPassName() const {
    return "Iterative Scheduling Driver";
  }

  // Pretend we are a basic block pass manager to prevent the normal passes
  // from using this pass as their manager.
  PassManagerType getPassManagerType() const {
    return PMT_BasicBlockPassManager;
  }
};
}
//===----------------------------------------------------------------------===//
char IterativeScheduling::ID = 0;

INITIALIZE_PASS_BEGIN(IterativeScheduling, "shang-iterative-scheduling",
                      "Preform iterative scheduling",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataflowAnnotation)
  INITIALIZE_PASS_DEPENDENCY(PreSchedBinding)
  INITIALIZE_PASS_DEPENDENCY(LoopInfo)
  INITIALIZE_PASS_DEPENDENCY(BranchProbabilityInfo)
INITIALIZE_PASS_END(IterativeScheduling, "shang-iterative-scheduling",
                    "Preform iterative scheduling",
                    false, true)

Pass *llvm::createIterativeSchedulingPass() {
  return new IterativeScheduling();
}

void IterativeScheduling::schedulePass(Pass *P) {
  // If P is an analysis pass and it is available then do not
  // generate the analysis again. Stale analysis info should not be
  // available at this point.
  const PassInfo *PI =
    PassRegistry::getPassRegistry()->getPassInfo(P->getPassID());
  if (PI && PI->isAnalysis() && findAnalysisPass(P->getPassID(), true)) {
    delete P;
    return;
  }

  AnalysisUsage *AnUsage = TPM->findAnalysisUsage(P);
  PassManagerType CurPMT = P->getPotentialPassManagerType();

  bool checkAnalysis = true;
  while (checkAnalysis) {
    checkAnalysis = false;

    const AnalysisUsage::VectorType &RequiredSet = AnUsage->getRequiredSet();
    typedef AnalysisUsage::VectorType::const_iterator iterator;
    for (iterator I = RequiredSet.begin(), E = RequiredSet.end(); I != E; ++I) {
      Pass *AnalysisPass = findAnalysisPass(*I, true);
      if (AnalysisPass) continue;

      const PassInfo *PI = PassRegistry::getPassRegistry()->getPassInfo(*I);

      assert(PI && "Expected required passes to be initialized");
      AnalysisPass = PI->createPass();
      PassManagerType AnalysisPMT = AnalysisPass->getPotentialPassManagerType();

      if (CurPMT == AnalysisPMT)
        // Schedule analysis pass that is managed by the same pass manager.
        schedulePass(AnalysisPass);
      else if (CurPMT > AnalysisPMT) {
        // Schedule analysis pass that is managed by a new manager.
        schedulePass(AnalysisPass);
        // Recheck analysis passes to ensure that required analyses that
        // are already checked are still available.
        checkAnalysis = true;
      }
      else
        // Do not schedule this analysis. Lower level analsyis
        // passes are run on the fly.
        // delete AnalysisPass;
        llvm_unreachable("On the fly analysis is not supported!");
    }
  }

  // Now all required passes are available.
  if (ImmutablePass *IP = P->getAsImmutablePass()) {
    // P is a immutable pass and it will be managed by this
    // top level manager. Set up analysis resolver to connect them.
    PMDataManager &ParentDM = getResolver()->getPMDataManager();
    AnalysisResolver *AR = new AnalysisResolver(ParentDM);
    P->setResolver(AR);
    initializeAnalysisImpl(P);
    TPM->addImmutablePass(IP);
    recordAvailableAnalysis(IP);
    return;
  }

  // Add the requested pass to the current manager.
  add(P);
}

void IterativeScheduling::changeOutputPaths(unsigned i) {
  SmallString<256> NewPath = sys::path::parent_path(OriginalRTLOutput);
  sys::path::append(NewPath, utostr_32(i));
  bool Existed;
  sys::fs::create_directories(StringRef(NewPath), Existed);
  (void)Existed;
  sys::path::append(NewPath, RTLFileName);

  std::string Script;
  raw_string_ostream SS(Script);
  SS << "RTLOutput = [[" << NewPath << "]]\n";
  sys::path::replace_extension(NewPath, ".sql");
  SS << "MCPDataBase = [[" << NewPath << "]]\n";

  SMDiagnostic Err;
  if (!runScriptStr(SS.str(), Err))
    report_fatal_error("Cannot set output paths for intermediate netlist!");
}

void IterativeScheduling::recoverOutputPath() {
  std::string Script;
  raw_string_ostream SS(Script);
  SS << "RTLOutput = [[" << OriginalRTLOutput << "]]\n"
        "MCPDataBase = [[" << OriginalMCPOutput << "]]\n";

  SMDiagnostic Err;
  if (!runScriptStr(SS.str(), Err))
    report_fatal_error("Cannot recover output paths!");
}
