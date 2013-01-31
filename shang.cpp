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
#include "shang/LuaScript.h"

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

#include <memory>

// This is the only header we need to include for LuaBind to work
#include "luabind/luabind.hpp"


// Include the lua headers (the extern "C" is a requirement because we're
// using C++ and lua has been compiled as C code)
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

using namespace llvm;

// General options for sync.  Other pass-specific options are specified
// within the corresponding sync passes, and target-specific options
// and back-end code generation options are specified with the target machine.
//
static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input lua script>"), cl::init("-"));

static void LoopOptimizerEndExtensionFn(const PassManagerBuilder &Builder,
                                        PassManagerBase &PM) {
  PM.add(createMemoryAccessAlignerPass());
  PM.add(createScalarEvolutionAliasAnalysisPass());
  PM.add(createTrivialLoopUnrollPass());
  PM.add(createMemoryAccessAlignerPass());
  PM.add(createInstructionCombiningPass());
}


static void addHighlevelSynthesisPasses(PassManager &PM) {
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

  PM.add(createLoopStrengthReducePass());
  PM.add(createGCLoweringPass());

  // Make sure that no unreachable blocks are instruction selected.
  PM.add(createUnreachableBlockEliminationPass());

  PM.add(createCFGSimplificationPass());

  // Do not pass the TLI to CodeGenPrepare pass, so it won't sink the address
  // computation. We can handle sinking by ourself.
  PM.add(createCodeGenPreparePass(0));

  //PM.add(createStackProtectorPass(getTargetLowering()));

  // Name the instructions.
  PM.add(createInstructionNamerPass());


  // All passes which modify the LLVM IR are now complete; run the verifier
  // to ensure that the IR is valid.
  PM.add(createVerifierPass());

  // Run the SCEVAA pass to compute more accurate alias information.
  PM.add(createScalarEvolutionAliasAnalysisPass());
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

  LuaScript *S = &scriptEngin();
  S->init();

  // Run the lua script.
  if (!S->runScriptFile(InputFilename, Err)){
    Err.print(argv[0], errs());
    return 1;
  }

  S->updateStatus();

  // Load the module to be compiled...
  std::auto_ptr<Module> M;

  M.reset(ParseIRFile(S->getValue<std::string>("InputFile"),
                      Err, Context));
  if (M.get() == 0) {
    Err.print(argv[0], errs());
    return 1;
  }
  Module &mod = *M.get();

  // TODO: Build the right triple.
  Triple TheTriple(mod.getTargetTriple());

  // Build up all of the passes that we want to do to the module.
  PassManagerBuilder Builder;
  Builder.DisableUnrollLoops = true;
  Builder.LibraryInfo = new TargetLibraryInfo();
  Builder.LibraryInfo->disableAllFunctions();
  Builder.OptLevel = 3;
  Builder.SizeLevel = 2;
  Builder.DisableSimplifyLibCalls = true;
  Builder.Inliner = createHLSInlinerPass();
  Builder.addExtension(PassManagerBuilder::EP_LoopOptimizerEnd,
                       LoopOptimizerEndExtensionFn);
  PassManager Passes;
  Passes.add(new DataLayout(S->getDataLayout()));

  Passes.add(createVerifierPass());

  // This is the final bitcode, internalize it to expose more optimization
  // opportunities. Note that we should internalize it before SW/HW partition,
  // otherwise we may lost some information that help the later internalize.
  const char *ExportList[] = { "main" };
  Passes.add(createInternalizePass(ExportList));

  // Perform Software/Hardware partition.
  Passes.add(createFunctionFilterPass(S->getOutputStream("SoftwareIROutput"),
                                      S->getTopHWFunctions()));
  Passes.add(createGlobalDCEPass());
  // Optimize the hardware part.
  //Builder.populateFunctionPassManager(*FPasses);
  Builder.populateModulePassManager(Passes);
  Builder.populateLTOPassManager(Passes,
                                 /*Internalize*/false,
                                 /*RunInliner*/true);

  //PM.add(createPrintModulePass(&dbgs()));
   
  // We do not use the stream that passing into addPassesToEmitFile.

  addHighlevelSynthesisPasses(Passes);

  // Analyse the slack between registers.
  //Passes.add(createCombPathDelayAnalysisPass());
  Passes.add(createRTLCodeGenPass(S->getOutputStream("RTLOutput")));

  //// Run some scripting passes.
  //for (LuaScript::scriptpass_it I = S->passes_begin(), E = S->passes_end();
  //     I != E; ++I) {
  //  const luabind::object &o = *I;
  //  Pass *P = createScriptingPass(
  //    luabind::object_cast<std::string>(I.key()).c_str(),
  //    luabind::object_cast<std::string>(o["FunctionScript"]).c_str(),
  //    luabind::object_cast<std::string>(o["GlobalScript"]).c_str());
  //  Passes.add(P);
  //}

  // Run the passes.
  Passes.run(mod);

  // If no error occur, keep the files.
  S->keepAllFiles();

  return 0;
}
