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

  DFGNode *createNode(Value *Val) const;
  DFGNode *createDataPathNode(Instruction *Inst) const;
  DFGNode *createConstantIntNode(ConstantInt *CI) const;
  DFGNode *createGlobalValueNode(GlobalValue *GV) const;
  DFGNode *createUndefValueNode(UndefValue *UV) const;
  DFGNode *createArgumentNode(Argument *Arg) const;
  DFGNode *createSequentialNode(Value *Val) const;

  void createDependencies(DFGNode *Node) const;
  void createDependency(DFGNode *From, DFGNode *To) const;

  void verifyDFGCorrectness() const;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    // Build DFG after the register synthesis temporary.
    AU.addRequiredID(SIRBitMaskAnalysisID);
    AU.setPreservesAll();
  }
};
}
