#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

namespace llvm {
struct DFGBuild : public SIRPass {
  static char ID;
  SIR *SM;
  DataLayout *TD;
  DataFlowGraph *G;

  DFGBuild() : SIRPass(ID) {
    initializeDFGBuildPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSIR(SIR &SM);
  DataFlowGraph *getDFG() const { return G; }

  unsigned getBitWidth(const Value *Val) const {
    if (Val->getType()->isVoidTy())
      return 0;

    return TD->getTypeSizeInBits(Val->getType());
  }
  float getCriticalPathDelay(Instruction *Inst) const;

  void verifyDFGCorrectness() const;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    // Build DFG after the register synthesis temporary.
    AU.addRequiredID(BitMaskAnalysisID);
    AU.setPreservesAll();
  }
};
}
