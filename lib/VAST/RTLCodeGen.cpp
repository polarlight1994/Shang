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
EnalbeDumpIR("shang-dump-ir", cl::desc("Dump the IR to the RTL code."),
             cl::init(false));

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
    AU.addRequiredID(SelectorSynthesisID);
    AU.addRequiredID(DatapathNamerID);
    AU.addRequired<HLSAllocation>();
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
  INITIALIZE_PASS_DEPENDENCY(SelectorSynthesis)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
  INITIALIZE_AG_DEPENDENCY(HLSAllocation)
INITIALIZE_PASS_END(RTLCodeGen, "shang-verilog-writer",
                    "Write the RTL verilog code to output file.",
                    false, true)

RTLCodeGen::RTLCodeGen() : VASTModulePass(ID), Out() {
  std::string RTLOutputPath = getStrValueFromEngine("RTLOutput");
  std::string Error;
  raw_fd_ostream *Output = new raw_fd_ostream(RTLOutputPath.c_str(), Error);
  Out.setStream(*Output, true);

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

    if (Allocation.getMemoryBankNum(*GV) == 0)
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

  Out.module_end();
  Out.flush();

  return false;
}
