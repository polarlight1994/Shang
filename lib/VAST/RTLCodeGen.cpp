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

#include "LangSteam.h"
#include "shang/VASTModulePass.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"
#include "shang/Utilities.h"

#include "llvm/IR/Module.h"
#include "llvm/Target/Mangler.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vtm-rtl-codegen"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<bool>
EnalbeDumpIR("vtm-dump-ir", cl::desc("Dump the IR to the RTL code."),
             cl::init(false));

namespace {
class RTLCodeGen : public VASTModulePass {
  vlang_raw_ostream Out;
  DataLayout *TD;

public:
  /// @name FunctionPass interface
  //{
  static char ID;
  RTLCodeGen(raw_ostream &O);
  RTLCodeGen() : VASTModulePass(ID) {
    assert( 0 && "Cannot construct the class without the raw_stream!");
  }

  ~RTLCodeGen(){}

  bool doInitialization(Module &M);

  bool runOnVASTModule(VASTModule &VM);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequiredID(ControlLogicSynthesisID);
    AU.setPreservesAll();
  }
};
}

//===----------------------------------------------------------------------===//
char RTLCodeGen::ID = 0;

Pass *llvm::createRTLCodeGenPass(raw_ostream &O) {
  return new RTLCodeGen(O);
}

INITIALIZE_PASS(RTLCodeGen, "shang-verilog-writer",
                "Write the RTL verilog code to output file.",
                false, true)

RTLCodeGen::RTLCodeGen(raw_ostream &O) : VASTModulePass(ID), Out(O) {
  initializeRTLCodeGenPass(*PassRegistry::getPassRegistry());
  initializeControlLogicSynthesisPass(*PassRegistry::getPassRegistry());
}

bool RTLCodeGen::doInitialization(Module &Mod) {
  TD = getAnalysisIfAvailable<DataLayout>();
  SMDiagnostic Err;
  const char *GlobalScriptPath[] = { "Misc", "RTLGlobalScript" };
  std::string GlobalScript = getStrValueFromEngine(GlobalScriptPath);
  if (!runScriptOnGlobalVariables(Mod, TD, GlobalScript, Err))
    report_fatal_error("VerilogASTWriter: Cannot run globalvariable script:\n"
                       + Err.getMessage());

  const char *GlobalCodePath[] = { "RTLGlobalCode" };
  std::string GlobalCode = getStrValueFromEngine(GlobalCodePath);
  Out << GlobalCode << '\n';

  return false;
}

bool RTLCodeGen::runOnVASTModule(VASTModule &VM) {
  if (EnalbeDumpIR) {
    Out << "`ifdef wtf_is_this\n" << "Function for RTL Codegen:\n";
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

  Out << "\n\n";
  Out << "// Always Block\n";
  Out.always_ff_begin();

  Out.else_begin();

  Out.always_ff_end();

  Out.module_end();
  Out.flush();

  return false;
}
