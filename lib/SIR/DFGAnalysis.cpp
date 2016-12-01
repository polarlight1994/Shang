#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/DFGBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"
#include "sir/LangSteam.h"

#include "vast/FUInfo.h"
#include "vast/LuaI.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <sstream>
#include "math.h"

using namespace llvm;
using namespace vast;

namespace {
struct DFGAnalysis : public SIRPass {
  static char ID;
  DataFlowGraph *DFG;

  DFGAnalysis() : SIRPass(ID) {
    initializeDFGAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSIR(SIR &SM);

  void ArrivalTimeAnalysis();

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(DFGBuildID);
    AU.addRequiredID(DFGOptID);
    AU.setPreservesAll();
  }
};
}

char DFGAnalysis::ID = 0;
char &llvm::DFGAnalysisID = DFGAnalysis::ID;
INITIALIZE_PASS_BEGIN(DFGAnalysis, "DFG-analysis",
                      "Perform the DFG analysis",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(DFGBuild)
  INITIALIZE_PASS_DEPENDENCY(DFGOpt)
INITIALIZE_PASS_END(DFGAnalysis, "DFG-analysis",
                    "Perform the DFG analysis",
                    false, true)

bool DFGAnalysis::runOnSIR(SIR &SM) {
  // Get the DFG.
  DFGBuild &DB = getAnalysis<DFGBuild>();
  this->DFG = DB.getDFG();

  ArrivalTimeAnalysis();

  return false;
}

void DFGAnalysis::ArrivalTimeAnalysis() {

}