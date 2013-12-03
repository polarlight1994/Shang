//=- FunctionFilter.cpp --- This Pass filter out the SW part of the module -==//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass perform the software/hardware sysinfo by simply move the SW part
// to another llvm module.
//
//===----------------------------------------------------------------------===//


#include "vast/Passes.h"
#include "vast/Utilities.h"

#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Assembly/AssemblyAnnotationWriter.h"

#include "llvm/Support/CallSite.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Casting.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/ADT/DepthFirstIterator.h"
#define DEBUG_TYPE "function-filter"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
struct FunctionFilter : public ModulePass {
  static char ID;
  // The output stream for software part.
  raw_fd_ostream *SwOut;
  std::map<Function*, std::string> TopFunctions;

  FunctionFilter(): ModulePass(ID), SwOut(0) {
    std::string SoftwareIROutputPath = getStrValueFromEngine("SoftwareIROutput");
    std::string Error;
    SwOut = new raw_fd_ostream(SoftwareIROutputPath.c_str(), Error);
    initializeFunctionFilterPass(*PassRegistry::getPassRegistry());
  }

  ~FunctionFilter() {
    delete SwOut;
  }

  void releaseMemory() {
    TopFunctions.clear();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<CallGraph>();
    ModulePass::getAnalysisUsage(AU);
  }

  void partition(Module &HWM, Module &SWM,
                 SmallPtrSet<const Function*, 32> &HWFunctions);

  bool runOnModule(Module &M);
};
} // end anonymous.

bool FunctionFilter::runOnModule(Module &M) {
  typedef Module::global_iterator global_iterator;
  for (global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I)
    I->setAlignment(std::max(8u, I->getAlignment()));

  bool isSyntesizingMain = false;
  SmallPtrSet<const Function*, 32> HWFunctions;
  CallGraph &CG = getAnalysis<CallGraph>();

  for (Module::iterator I = M.begin(), E = M.end();I != E; ++I) {
    Function *F = I;

    if (F->isDeclaration()) continue;

    // Try to retrieve the synthesis configuration of the current function. 
    const char *FunctionInfoPath[2] = { "Functions",
                                        F->getValueName()->getKeyData() };
    std::string DesignName = getStrValueFromEngine(FunctionInfoPath);

    // Ignore the function if the synthesis configuration is not available.
    if (DesignName.empty()) continue;

    TopFunctions.insert(std::make_pair(F, DesignName));

    if (F->getName()=="main") isSyntesizingMain = true;

    // Export the hardware function, so that it can be called from the software
    // side.
    F->setLinkage(GlobalValue::ExternalLinkage);

    CallGraphNode *CGN = CG[F];
    assert(CGN && "Broken CallGraph!");
    // All functions called by this function is also hardware functions.
    for (df_iterator<CallGraphNode*> ICGN = df_begin(CGN),
         ECGN = df_end(CGN); ICGN != ECGN; ++ICGN){
      const CallGraphNode *SubCGN = *ICGN;
      Function *SubFN = SubCGN->getFunction();
      if (!SubFN || SubFN->isDeclaration())
        continue;

      HWFunctions.insert(SubFN);
    }
  }

  OwningPtr<Module> SWM(CloneModule(&M));

  if (!isSyntesizingMain) partition(M, *SWM, HWFunctions);

  // Rename the function after we split the module.
  typedef std::map<Function*, std::string>::iterator iterator;
  for (iterator I = TopFunctions.begin(), E = TopFunctions.end(); I != E; ++I) {
    Function *HWF = I->first;
    StringRef OriginalName = HWF->getName();

    if (HWF == 0 || HWF->isDeclaration() || HWF->getName() == "main")
      continue;

    HWF->setName(I->second);

    Function *SWF = SWM->getFunction(OriginalName);
    if (SWF == 0 || !SWF->isDeclaration()) continue;

    // Call the interface function from the software side instead.
    SWF->setName(I->second  + "_if");
  }

  if (!isSyntesizingMain) {
    // Write the module out.
    OwningPtr<AssemblyAnnotationWriter> Annotator;
    SWM->print(*SwOut, Annotator.get());
  }

  return true;
}

char FunctionFilter::ID = 0;

INITIALIZE_PASS_BEGIN(FunctionFilter, "FunctionFilter",
                      "Function Filter", false, false)
  INITIALIZE_AG_DEPENDENCY(CallGraph)
INITIALIZE_PASS_END(FunctionFilter, "FunctionFilter",
                    "Function Filter", false, false)

Pass *llvm::createFunctionFilterPass() {
  return new FunctionFilter();
}

void FunctionFilter::partition(Module &HWM, Module &SWM,
                               SmallPtrSet<const Function*, 32> &HWFunctions){
  SWM.setModuleIdentifier(HWM.getModuleIdentifier() + ".sw");
  for (Module::iterator I = HWM.begin(), E = HWM.end(); I != E; ++I) {
    Function *FHW = I;
    Function *FSW = SWM.getFunction(FHW->getName());

    // The function is s software function, delete it from the hardware module.
    if (!HWFunctions.count(FHW))
      FHW->deleteBody();
    else if (TopFunctions.count(FHW))
      // Remove hardware functions in software module and leave the declaretion
      // only.
      FSW->deleteBody();
    else if (FHW->getName() != "main" && !FHW->isDeclaration()) {
      FHW->setLinkage(GlobalValue::PrivateLinkage);
      FSW->setLinkage(GlobalValue::PrivateLinkage);
    }

    if (FSW->getName() == "main") FSW->setName("sw_main");
  }

  OwningPtr<ModulePass> GlobalDEC(createGlobalDCEPass());
  GlobalDEC->runOnModule(SWM);

  if (!SWM.empty()) {
    // If a global variable present in software module, set the linkage of
    // corresponding one in hardware module to external.
    typedef Module::global_iterator global_iterator;
    for (global_iterator I = SWM.global_begin(), E = SWM.global_end();
         I != E; ++I) {
      // Make sure we can link against the global variables in software module.
      I->setLinkage(GlobalVariable::LinkOnceAnyLinkage);

      if (GlobalVariable *GV = HWM.getGlobalVariable(I->getName(), true)) {
        GV->setLinkage(GlobalValue::ExternalLinkage);
        GV->setInitializer(0);
      }
    }

    for (Module::iterator I = HWM.begin(), E = HWM.end(); I != E; ++I) {
      Function *HWF = I;
      // Do not inline the functions that called by software module.
      if (Function *SWF = SWM.getFunction(I->getName())) {
        // Is it a software function?
        if (!SWF->isDeclaration()) continue;

        // Is it also not a hardware function?
        if (HWF->isDeclaration()) continue;

        // Do not inline the HW function.
        HWF->getAttributes().removeAttribute(HWF->getContext(),
                                             AttributeSet::FunctionIndex,
                                             Attribute::AlwaysInline);
        HWF->addFnAttr(Attribute::NoInline);
        assert(HWF->getLinkage() == GlobalValue::ExternalLinkage &&
               "Bad linkage for the hardware function!");

        DEBUG(dbgs() << "No inline -- " << I->getName() << '\n');
      } else if (!HWF->isDeclaration() && !HWFunctions.count(HWF)) {
        HWF->setLinkage(GlobalValue::PrivateLinkage);
        // The function only used in the hardware module
        //if (!I->hasFnAttr(Attribute::NoInline))
        //  I->addAttribute(~0, Attribute::AlwaysInline);
        // DEBUG(dbgs() << "Always inline " << I->getName() << '\n');
      }
    }
  }
}
