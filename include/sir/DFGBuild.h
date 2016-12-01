#ifndef DFG_BUILD_H
#define DFG_BUILD_H

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

  DFGNode *getOrCreateDFGNode(Value *V, unsigned BitWidth);
  void verifyDFGCorrectness() const;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(SIRRegisterSynthesisForCodeGenID);
    AU.setPreservesAll();
  }
};
}

#endif
