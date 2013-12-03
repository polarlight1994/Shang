//===- Writer.cpp - VTM machine instructions to RTL verilog  ----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VerilogASTWriter pass, which write VTM machine instructions
// in form of RTL verilog code.
//
//===----------------------------------------------------------------------===//

#include "Allocation.h"
#include "LangSteam.h"

#include "shang/VASTModulePass.h"
#include "shang/VASTModule.h"
#include "shang/Utilities.h"
#include "shang/Passes.h"
#include "shang/STGDistances.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vtm-rtl-codegen"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<bool>
EnalbeDumpIR("vast-dump-ir", cl::desc("Dump the IR to the RTL code."),
             cl::init(false));
static cl::opt<bool>
EnalbeDumpTrace("vast-dump-trace", cl::desc("Dump the execution trace."),
                cl::init(true));

namespace {
struct RTLCodeGen : public VASTModulePass {
  vlang_raw_ostream Out;

  /// @name FunctionPass interface
  //{
  static char ID;
  RTLCodeGen();

  ~RTLCodeGen(){}

  void generateCodeForTopModule(Module *M, VASTModule &VM);
  bool runOnVASTModule(VASTModule &VM);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.addRequiredID(TimingDrivenSelectorSynthesisID);
    AU.addRequiredID(DatapathNamerID);
    AU.addRequired<STGDistances>();
    AU.setPreservesAll();
  }
};
}

//===----------------------------------------------------------------------===//
char RTLCodeGen::ID = 0;

Pass *llvm::createRTLCodeGenPass() {
  return new RTLCodeGen();
}

INITIALIZE_PASS_BEGIN(RTLCodeGen, "shang-verilog-writer",
                      "Write the RTL verilog code to output file.",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(TimingDrivenSelectorSynthesis)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
  INITIALIZE_PASS_DEPENDENCY(STGDistances)
INITIALIZE_PASS_END(RTLCodeGen, "shang-verilog-writer",
                    "Write the RTL verilog code to output file.",
                    false, true)

RTLCodeGen::RTLCodeGen() : VASTModulePass(ID), Out() {
  initializeRTLCodeGenPass(*PassRegistry::getPassRegistry());
}

void RTLCodeGen::generateCodeForTopModule(Module *M, VASTModule &VM) {
  DataLayout *TD = getAnalysisIfAvailable<DataLayout>();
  HLSAllocation &Allocation = getAnalysis<HLSAllocation>();

  SMDiagnostic Err;
  const char *GlobalScriptPath[] = { "Misc", "RTLGlobalScript" };
  std::string GlobalScript = getStrValueFromEngine(GlobalScriptPath);
  SmallVector<GlobalVariable*, 32> GVs;

  for (Module::global_iterator I = M->global_begin(), E = M->global_end();
       I != E; ++I) {
    GlobalVariable *GV = I;

    if (Allocation.getMemoryBank(*GV) == NULL)
      GVs.push_back(I);
  }

  if (!runScriptOnGlobalVariables(GVs, TD, GlobalScript, Err))
    report_fatal_error("RTLCodeGen: Cannot run globalvariable script:\n"
                       + Err.getMessage());

  // Read the result from the scripting engine.
  const char *GlobalCodePath[] = { "RTLGlobalCode" };
  std::string GlobalCode = getStrValueFromEngine(GlobalCodePath);
  Out << GlobalCode << '\n';

  bindFunctionToScriptEngine(*TD, &VM);
  const char *TopModuleScriptPath[] = { "Misc", "RTLTopModuleScript" };
  std::string TopModuleScript = getStrValueFromEngine(TopModuleScriptPath);
  if (!runScriptStr(TopModuleScript, Err))
    report_fatal_error("RTLCodeGen: Cannot run top module script:\n"
                       + Err.getMessage());
}

bool RTLCodeGen::runOnVASTModule(VASTModule &VM) {
  Function &F = VM.getLLVMFunction();
  std::string RTLOutputPath = getStrValueFromEngine("RTLOutput");
  std::string Error;
  raw_fd_ostream Output(RTLOutputPath.c_str(), Error);
  Out.setStream(Output);

  generateCodeForTopModule(F.getParent(), VM);

  if (EnalbeDumpIR) {
    Out << "`ifdef wtf_is_this\n" << "Function for RTL Codegen:\n";
    F.print(Out);
    VM.print(Out);
    Out << "`endif\n";
  }

  // Write buffers to output
  VM.printModuleDecl(Out);
  Out.module_begin();
  Out << "\n\n";
  // Reg and wire
  Out << "// Reg and wire decl\n";
  VM.printSignalDecl(Out);
  Out << "\n\n";
  // Datapath
  Out << "// Datapath\n";
  VM.printDatapath(Out);

  // Sequential logic of the registers.
  VM.printSubmodules(Out);
  VM.printRegisterBlocks(Out);

#ifndef DISABLE_SELFVERIFICATION
  STGDistances &STGDist = getAnalysis<STGDistances>();
  // Verify the register assignment.
  Out << "// synthesis translate_off\n";

  if (EnalbeDumpTrace)
    Out << "integer trace_database; // the\n";

  // Open the trace database
  if (EnalbeDumpTrace) {
    Out << "initial ";
    Out.enter_block();
    Out << "trace_database = $fopen(\"trace_database.sql\");\n";
    VASTSelector::initTraceDataBase(Out, "trace_database");
    Out.exit_block();
  }

  Out.always_ff_begin(false);

  typedef VASTModule::selector_iterator iterator;
  for (iterator I = VM.selector_begin(), E = VM.selector_end(); I != E; ++I) {
    Out << "// Verification code for Selector: " << I->getName() << '\n';
    I->printVerificationCode(Out, &STGDist,
                             EnalbeDumpTrace ? "trace_database" : 0);
    Out << '\n';
  }

  Out.always_ff_end(false);
  Out << "// synthesis translate_on\n\n";
#endif

  Out.module_end();
  Out.flush();

  Out.setStream(nulls());
  return false;
}
