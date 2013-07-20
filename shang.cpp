//===-- sync.cpp - Implement the C Synthesis Code Generator ---------------===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This is the C Synthesis code generator driver. It provides a convenient
// command-line interface for generating RTL Verilog and various interfacing
// code, given LLVM bitcode.
//
//===----------------------------------------------------------------------===//
#include "shang/Passes.h"
#include "shang/Utilities.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Config/config.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Assembly/PrintModulePass.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/Target/TargetLibraryInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"

#include <memory>
#include <map>

using namespace llvm;
namespace llvm {
bool loadConfig(const std::string &Path);
std::string getDataLayoutFromEngine();
}

// General options for sync.  Other pass-specific options are specified
// within the corresponding sync passes, and target-specific options
// and back-end code generation options are specified with the target machine.
//
static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input lua script>"), cl::init("-"));

static cl::opt<bool> EnableGotoExpansion("shang-enable-goto-expansion",
  cl::desc("Perform goto expansion to generate a function that include all code"),
  cl::init(true));

static cl::opt<bool> EnableMemoryOptimization("shang-enable-memory-optimization",
  cl::desc("Perform memory optimizations e.g. coalescing or banking"),
  cl::init(true));

static cl::opt<bool>
EnableMemoryPartition("shang-enable-memory-partition",
  cl::desc("Perform memory partition"),
  cl::init(true));

static cl::opt<bool> EnableRegisterSharing("shang-enable-register-sharing",
  cl::desc("Perform register sharing"),
  cl::init(false));

static cl::opt<bool>
EnablePreScheduleLUTMapping("shang-enable-pre-schedule-lut-mapping",
  cl::desc("Perform lut mapping before scheduling"),
  cl::init(true));

static cl::opt<bool> EnableMUXPipelining("shang-enable-mux-pipelining",
  cl::desc("Perform MUX pipelining"),
  cl::init(false));

static cl::opt<bool> DumpIRBeforeHLS("shang-enable-dump-ir-before-hls",
  cl::desc("Print the IR before HLS"),
  cl::init(false));

static void addHLSPreparePasses(PassManager &PM) {
  // Basic AliasAnalysis support.
  // Add TypeBasedAliasAnalysis before BasicAliasAnalysis so that
  // BasicAliasAnalysis wins if they disagree. This is intended to help
  // support "obvious" type-punning idioms.
  PM.add(createTypeBasedAliasAnalysisPass());
  PM.add(createBasicAliasAnalysisPass());

  // Try to lower memory access to accessing local memory, and annotate the
  // unhandled stack allocation alias with global variable, schedule this pass
  // before standard target orient IR passes which create ugly instructions
  // and these intructions are not able to be handle by the BlockRAMFormation
  // pass.
  //PM.add(createBlockRAMFormation(*getIntrinsicInfo()));
  // Schedule the DeadArgEliminationPass to clean up the module.
  PM.add(createDeadArgEliminationPass());

  PM.add(createGCLoweringPass());

  // Make sure that no unreachable blocks are instruction selected.
  PM.add(createUnreachableBlockEliminationPass());

  PM.add(createCFGSimplificationPass());

  // Do not pass the TLI to CodeGenPrepare pass, so it won't sink the address
  // computation. We can handle sinking by ourself.
  PM.add(createCodeGenPreparePass(0));
}

void addIROptimizationPasses(PassManager &HLSPasses) {
  PassManagerBuilder Builder;
  Builder.DisableUnrollLoops = true;
  Builder.LibraryInfo = new TargetLibraryInfo();
  Builder.LibraryInfo->disableAllFunctions();
  Builder.OptLevel = 3;
  Builder.SizeLevel = 2;
  Builder.DisableSimplifyLibCalls = true;
  Builder.Inliner = createHLSInlinerPass();

  HLSPasses.add(createVerifierPass());
  // Optimize the hardware part.
  //Builder.populateFunctionPassManager(*FPasses);
  Builder.populateModulePassManager(HLSPasses);
  Builder.populateLTOPassManager(HLSPasses,
                                 /*Internalize*/false,
                                 /*RunInliner*/true);
}

// main - Entry point for the sync compiler.
//
int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal();

  PrettyStackTraceProgram X(argc, argv);

  // Enable debug stream buffering.
  EnableDebugBuffering = true;

  LLVMContext &Context = getGlobalContext();
  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  cl::ParseCommandLineOptions(argc, argv, "llvm system compiler\n");

  SMDiagnostic Err;

  std::string error;

  if (loadConfig(InputFilename))
    report_fatal_error("Cannot load synthesis configuration file!");

  // Load the module to be compiled...
  std::auto_ptr<Module> M;

  M.reset(ParseIRFile(getStrValueFromEngine("InputFile"), Err, Context));

  if (M.get() == 0) {
    Err.print(argv[0], errs());
    return 1;
  }

  std::string DataLayoutStr = getDataLayoutFromEngine();

  Module &mod = *M.get();

  // Stage 1, perform software/hardware partition.
  {
    // Make sure the PassManager is deleted before the tool_output_file otherwise
    // we may delete the raw_fd_ostream before the other streams that using it.
    PassManager PreHLSPasses;
    PreHLSPasses.add(new DataLayout(DataLayoutStr));

    PreHLSPasses.add(createVerifierPass());

    // This is the final bitcode, internalize it to expose more optimization
    // opportunities. Note that we should internalize it before SW/HW partition,
    // otherwise we may lost some information that help the later internalize.
    const char *ExportList[] = { "main" };
    PreHLSPasses.add(createInternalizePass(ExportList));

    // Perform Software/Hardware partition.
    PreHLSPasses.add(createFunctionFilterPass());
    PreHLSPasses.add(createGlobalDCEPass());

    PreHLSPasses.run(mod);
  }

  if (mod.empty()) {
    report_fatal_error("Module become empty after Software/Hardware paritioning!");
    return 0;
  }

  // Stage 2, perform high-level synthesis related IR optimizations.
  {
    PassManager HLSIRPasses;
    HLSIRPasses.add(new DataLayout(DataLayoutStr));
    HLSIRPasses.add(createShangTargetTransformInfoPass());
    addIROptimizationPasses(HLSIRPasses);
    addHLSPreparePasses(HLSIRPasses);
    HLSIRPasses.run(mod);
  }

  const char *MainSynthesisInfoPath[2] = { "Functions", "main" };
  bool isMainSynthesis = !getStrValueFromEngine(MainSynthesisInfoPath).empty();
  // Stage 3, perform high-level synthesis.
  // Build up all of the passes that we want to do to the module.
  {
    // Make sure the PassManager is deleted before the tool_output_file otherwise
    // we may delete the raw_fd_ostream before the other streams that using it.
    PassManager HLSPasses;
    HLSPasses.add(new DataLayout(DataLayoutStr));
    HLSPasses.add(createShangTargetTransformInfoPass());
    HLSPasses.add(createBasicAliasAnalysisPass());
    HLSPasses.add(createTypeBasedAliasAnalysisPass());
    HLSPasses.add(createObjectBasedAliasAnalyaisPass());
    // Name the instructions to make the LLVM IR easier for debugging.
    HLSPasses.add(createInstructionNamerPass());

    HLSPasses.add(createLowerIntrinsicPass());

    // Try to optimize the computation.
    HLSPasses.add(createInstructionCombiningPass());

    if (EnableGotoExpansion) {
      // Sink the the datapath instruction to avoid unnecessary PHIs.
      HLSPasses.add(createSinkingPass());
      // Perform goto expansion.
      HLSPasses.add(createDemoteRegisterToMemoryPass());
      HLSPasses.add(createGotoExpansionPass());
      HLSPasses.add(createGlobalDCEPass());
      HLSPasses.add(createVerifierPass());
      HLSPasses.add(createGlobalToStackPass());
      HLSPasses.add(createPromoteMemoryToRegisterPass());
      HLSPasses.add(createCFGSimplificationPass());
      HLSPasses.add(createCorrelatedValuePropagationPass());
      HLSPasses.add(createGlobalOptimizerPass());
      HLSPasses.add(createSROAPass());
      HLSPasses.add(createDatapathHoistingPass());
      HLSPasses.add(createGVNPass());
      HLSPasses.add(createInstructionCombiningPass());
      HLSPasses.add(createDeadStoreEliminationPass());
    }

    HLSPasses.add(createDatapathHoistingPass());

    // Unroll the loop to expose more coalescing opportunities.
    if (EnableMemoryOptimization) {
      HLSPasses.add(createMemoryAccessAlignerPass());
      HLSPasses.add(createScalarEvolutionAliasAnalysisPass());
      HLSPasses.add(createTrivialLoopUnrollPass());
      HLSPasses.add(createInstructionCombiningPass());
      HLSPasses.add(createCFGSimplificationPass());
      HLSPasses.add(createDeadInstEliminationPass());
      HLSPasses.add(createGlobalOptimizerPass());
      HLSPasses.add(createSROAPass());
      HLSPasses.add(createInstructionCombiningPass());
      HLSPasses.add(createMemoryAccessAlignerPass());
      // Run the SCEVAA pass to compute more accurate alias information.
      HLSPasses.add(createScalarEvolutionAliasAnalysisPass());
      HLSPasses.add(createMemoryAccessCoalescingPass());
      // Verifier the IR produced by the Coalescer.
      HLSPasses.add(createVerifierPass());
      HLSPasses.add(createGVNPass());
      HLSPasses.add(createInstructionCombiningPass());
      HLSPasses.add(createDeadStoreEliminationPass());
    }

    HLSPasses.add(createLowerGetElementPtrPass());

    // Try to optimize the computation.
    HLSPasses.add(createInstructionCombiningPass());
    // Move the datapath instructions as soon as possible.
    HLSPasses.add(createDatapathHoistingPass());

    if (DumpIRBeforeHLS)
      HLSPasses.add(createPrintModulePass(&dbgs()));

    // Replace the stack alloca variables by global variables.
    HLSPasses.add(createLowerAllocaPass());

    // Name the instructions.
    HLSPasses.add(createInstructionNamerPass());

    // Allocate the BlockRAMs.
    if (isMainSynthesis && EnableMemoryPartition)
      HLSPasses.add(createMemoryPartitionPass());
    else
      HLSPasses.add(createSimpleBlockRAMAllocationPass());

    if (EnablePreScheduleLUTMapping) HLSPasses.add(createLUTMappingPass());

    // Perform the scheduling.
    HLSPasses.add(createScalarEvolutionAliasAnalysisPass());
    HLSPasses.add(createIterativeSchedulingPass());
    // Scheduling will restruct the datapath. Optimize the datapath again
    // after scheduling.
    HLSPasses.add(createLUTMappingPass());

    if (EnableRegisterSharing) HLSPasses.add(createRegisterSharingPass());
    if (EnableMUXPipelining) HLSPasses.add(createSelectorPipeliningPass());

    // Analyse the slack between registers.
    HLSPasses.add(createRTLCodeGenPass());
    HLSPasses.add(createTimingScriptGenPass());

    // Run the passes.
    HLSPasses.run(mod);
  }

  return 0;
}
