#include "sir/SIR.h"
#include "sir/SIRBuild.h"

namespace llvm {
class SIRDatapathBLO {
  SIR *SM;
  DataLayout *TD;
  SIRDatapathBuilder Builder;
    raw_ostream &MulMaskOutput;

  // Avoid visiting the same instruction twice.
  std::set<Instruction *> Visited;

public:
  SIRDatapathBLO(SIR *SM, DataLayout *TD,   raw_ostream &MulMaskOutput): SM(SM), TD(TD), MulMaskOutput(MulMaskOutput), Builder(SM, *TD) {}

  Value *replaceKnownAllBits(Value *Val);
  Value *optimizeInst(Instruction *Inst);

  bool replaceIfNotEqual(Value *From, Value *To);

  Value *optimizeAndInst(Instruction *AndInst);
  Value *optimizeNotInst(Instruction *NotInst);
  Value *optimizeOrInst(Instruction *OrInst);
  Value *optimizeXorInst(Instruction *XorInst);
  Value *optimizeRandInst(Instruction *RandInst);
  Value *optimizeRxorInst(Instruction *RxorInst);
  Value *optimizeBitCatInst(Instruction *BitCatInst);
  Value *optimizeBitExtractInst(Instruction *BitExtractInst);
  Value *optimizeBitRepeatInst(Instruction *BitRepeatInst);
  Value *optimizeAddInst(Instruction *AddInst);
  Value *optimizeAddcInst(Instruction *AddcInst);
  Value *optimizeMulInst(Instruction *MulInst);
  Value *optimizeUDivInst(Instruction *UDivInst);
  Value *optimizeSDivInst(Instruction *SDivInst);
  Value *optimizeShlInst(Instruction *ShlInst);
  Value *optimizeLshrInst(Instruction *LshrInst);
  Value *optimizeAshrInst(Instruction *AshrInst);
  Value *optimizeUgtInst(Instruction *UgtInst);
  Value *optimizeSgtInst(Instruction *SgtInst);

  Value *optimizeRegAssignInst(Instruction *RegAssignInst);

  bool optimizeValue(Value *Val);
  bool optimizeRegister(SIRRegister *Reg);
  bool optimizeDatapath();

  void computeMask(Instruction *Inst);

  void updateRegMux();
};
}