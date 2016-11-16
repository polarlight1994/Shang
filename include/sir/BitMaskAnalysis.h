#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

#include "llvm/ADT/Statistic.h"

namespace llvm {
struct BitMaskAnalysis : public SIRPass {
  SIR *SM;
  DataLayout *TD;

  // Avoid visit the instruction twice in traverse.
  std::set<Instruction *> Visited;

  static char ID;
  BitMaskAnalysis() : SIRPass(ID) {
    initializeBitMaskAnalysisPass(*PassRegistry::getPassRegistry());
  }

  void printMask(raw_fd_ostream &Output);
  void verifyMaskCorrectness();

  // Bit extraction of BitMasks.
  static APInt getBitExtraction(const APInt &OriginMask,
                                unsigned UB, unsigned LB) {
    if (UB != OriginMask.getBitWidth() || LB != 0)
      return OriginMask.lshr(LB).sextOrTrunc(UB - LB);

    return OriginMask;
  }

  static BitMask computeAnd(BitMask LHS, BitMask RHS);
  static BitMask computeOr(BitMask LHS, BitMask RHS);
  static BitMask computeNot(BitMask Mask);
  static BitMask computeXor(BitMask LHS, BitMask RHS);
  static BitMask computeRand(BitMask Mask);
  static BitMask computeRxor(BitMask Mask);
  static BitMask computeBitCat(BitMask LHS, BitMask RHS);
  static BitMask computeBitExtract(BitMask Mask, unsigned UB, unsigned LB);
  static BitMask computeBitRepeat(BitMask Mask, unsigned RepeatTimes);
  static BitMask computeAdd(BitMask LHS, BitMask RHS, unsigned ResultBitWidth);
  static BitMask computeMul(BitMask LHS, BitMask RHS);
  static BitMask computeShl(BitMask LHS, BitMask RHS);
  static BitMask computeLshr(BitMask LHS, BitMask RHS);
  static BitMask computeAshr(BitMask LHS, BitMask RHS);
  static BitMask computeUgt(BitMask LHS, BitMask RHS);
  static BitMask computeSgt(BitMask LHS, BitMask RHS);
  static BitMask computeUDiv(BitMask LHS, BitMask RHS);
  static BitMask computeSDiv(BitMask LHS, BitMask RHS);

  BitMask computeMask(Instruction *Inst, SIR *SM, DataLayout *TD);
  bool computeAndUpdateMask(Instruction *Inst);
  bool traverseFromRoot(Value *Val);
  bool traverseDatapath();

  bool runIteration();
  bool runOnSIR(SIR &SM);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(SIRRegisterSynthesisForCodeGenID);
    AU.setPreservesAll();
  }
};
}
