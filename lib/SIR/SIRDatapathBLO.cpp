#include "sir/SIRPass.h"
#include "sir/SIRDatapathBLO.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

#include "llvm/ADT/Statistic.h"

using namespace llvm;
using namespace vast;

STATISTIC(NumIterations, "Number of bit-level optimization iteration");
STATISTIC(ValueReplacedByKnownBits,
          "Number of Values whose bits are all known "
          "during the bit-level optimization");

std::set<Instruction *> VisitedMulInst;
std::set<Instruction *> AllMulInst;

bool SIRDatapathBLO::replaceIfNotEqual(Value *From, Value *To) {
  if (From == To || !To)
    return false;

  SIRBitMask OldMask = SM->getBitMask(From);

  //From->replaceAllUsesWith(To);

  if (Instruction *ToInst = dyn_cast<Instruction>(To)) {
    Visited.insert(ToInst);

    // Apply the old mask to the new instruction.
    SIRBitMask NewMask(Builder.getBitWidth(To));
    if (SM->hasBitMask(To))
      NewMask = SM->getBitMask(To);
    else
      NewMask.evaluateMask(ToInst, SM, TD);

    NewMask.mergeKnownByOr(OldMask);
    SM->IndexVal2BitMask(To, NewMask);
  }

//   // If the Value is sequential value, then we also need to
//   // update the corresponding register.
//   if (SIRRegister *Reg = SM->lookupSIRReg(From)) {
//     Reg->setLLVMValue(To);
//     SM->IndexSeqVal2Reg(To, Reg);
//   }

  return true;
}

Value *SIRDatapathBLO::replaceKnownAllBits(Value *Val) {
  if (!SM->hasBitMask(Val) || isa<ConstantInt>(Val))
    return Val;

  SIRBitMask Mask = SM->getBitMask(Val);
  Instruction *Inst = dyn_cast<Instruction>(Val);
  assert(Inst && "Unexpected Value type!");

  // Update the mask.
  Mask.evaluateMask(Inst, SM, TD);
  SM->IndexVal2BitMask(Val, Mask);

  // Only when all bits are known, the value can be replaced
  // by a constant value.
  if (!Mask.isAllBitKnown())
    return Val;

  ++ValueReplacedByKnownBits;
  return Builder.createIntegerValue(Mask.getKnownValues());
}

Value *SIRDatapathBLO::optimizeAndInst(Instruction *AndInst) {
  Value *LHS = AndInst->getOperand(0), *RHS = AndInst->getOperand(1);

  unsigned BitWidth = Builder.getBitWidth(LHS);
  assert(BitWidth == Builder.getBitWidth(RHS) && "BitWidth not matches!");

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(AndInst, SM, TD);
  SM->IndexVal2BitMask(AndInst, Mask);

//   // A & A = A;
//   if (LHS == RHS)
//     return LHS;
// 
//   // A & ~A = 0;
//   if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(LHS)) {
//     if (II->getIntrinsicID() == Intrinsic::shang_not && II->getOperand(0) == RHS)
//       return Builder.createIntegerValue(BitWidth, 0);
//   }
// 
//   // A & 1s = A; A & 0s = 0
//   APInt Ones = APInt::getAllOnesValue(BitWidth);
//   APInt Zeros = APInt::getNullValue(BitWidth);
//   if (ConstantInt *LHSCI = dyn_cast<ConstantInt>(LHS)) {
//     APInt LHSVal = LHSCI->getValue();
// 
//     if (LHSVal == Ones)
//       return RHS;
// 
//     if (LHSVal == Zeros)
//       return Builder.createIntegerValue(BitWidth, 0);
//   }
//   if (ConstantInt *RHSCI = dyn_cast<ConstantInt>(RHS)) {
//     APInt RHSVal = RHSCI->getValue();
// 
//     if (RHSVal == Ones)
//       return LHS;
// 
//     if (RHSVal == Zeros)
//       return Builder.createIntegerValue(BitWidth, 0);
//   }

  return AndInst;
}

Value *SIRDatapathBLO::optimizeNotInst(Instruction *NotInst) {
  Value *Operand = NotInst->getOperand(0);

  unsigned BitWidth = Builder.getBitWidth(Operand);

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(NotInst, SM, TD);
  SM->IndexVal2BitMask(NotInst, Mask);

  return NotInst;
}

Value *SIRDatapathBLO::optimizeOrInst(Instruction *OrInst) {
  Value *LHS = OrInst->getOperand(0), *RHS = OrInst->getOperand(1);

  unsigned BitWidth = Builder.getBitWidth(LHS);
  assert(BitWidth == Builder.getBitWidth(RHS) && "BitWidth not matches!");

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(OrInst, SM, TD);
  SM->IndexVal2BitMask(OrInst, Mask);

  return OrInst;
}

Value *SIRDatapathBLO::optimizeXorInst(Instruction *XorInst) {
  Value *LHS = XorInst->getOperand(0), *RHS = XorInst->getOperand(1);

  unsigned BitWidth = Builder.getBitWidth(LHS);
  assert(BitWidth == Builder.getBitWidth(RHS) && "BitWidth not matches!");

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(XorInst, SM, TD);
  SM->IndexVal2BitMask(XorInst, Mask);

  return XorInst;
}

Value *SIRDatapathBLO::optimizeRandInst(Instruction *RandInst) {
  Value *Operand = RandInst->getOperand(0);

  // Initialize the mask for this instruction.
  SIRBitMask Mask(1);

  // Evaluate the mask.
  Mask.evaluateMask(RandInst, SM, TD);
  SM->IndexVal2BitMask(RandInst, Mask);

  return RandInst;
}

Value *SIRDatapathBLO::optimizeRxorInst(Instruction *RxorInst) {
  Value *Operand = RxorInst->getOperand(0);

  // Initialize the mask for this instruction.
  SIRBitMask Mask(1);

  // Evaluate the mask.
  Mask.evaluateMask(RxorInst, SM, TD);
  SM->IndexVal2BitMask(RxorInst, Mask);

  return RxorInst;
}

Value *SIRDatapathBLO::optimizeBitCatInst(Instruction *BitCatInst) {
  Value *LHS = BitCatInst->getOperand(0), *RHS = BitCatInst->getOperand(1);

  unsigned BitWidth = Builder.getBitWidth(LHS) + Builder.getBitWidth(RHS);

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(BitCatInst, SM, TD);
  SM->IndexVal2BitMask(BitCatInst, Mask);

  return BitCatInst;
}

Value *SIRDatapathBLO::optimizeBitExtractInst(Instruction *BitExtractInst) {
  Value *Operand = BitExtractInst->getOperand(0);
  Value *UBVal = BitExtractInst->getOperand(1), *LBVal = BitExtractInst->getOperand(2);
  ConstantInt *UBCI = dyn_cast<ConstantInt>(UBVal), *LBCI = dyn_cast<ConstantInt>(LBVal);
  unsigned UB = UBCI->getValue().getZExtValue(), LB = LBCI->getValue().getZExtValue();

  unsigned BitWidth = UB - LB;

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(BitExtractInst, SM, TD);
  SM->IndexVal2BitMask(BitExtractInst, Mask);

  return BitExtractInst;
}

Value *SIRDatapathBLO::optimizeBitRepeatInst(Instruction *BitRepeatInst) {
  Value *Operand = BitRepeatInst->getOperand(0);
  Value *RepeatTimesVal = BitRepeatInst->getOperand(1);
  ConstantInt *RepeatTimesCI = dyn_cast<ConstantInt>(RepeatTimesVal);
  unsigned RepeatTimes = RepeatTimesCI->getValue().getZExtValue();

  // Initialize the mask for this instruction.
  SIRBitMask Mask(RepeatTimes);

  // Evaluate the mask.
  Mask.evaluateMask(BitRepeatInst, SM, TD);
  SM->IndexVal2BitMask(BitRepeatInst, Mask);

  return BitRepeatInst;
}

Value *SIRDatapathBLO::optimizeAddInst(Instruction *AddInst) {
  if (AddInst->getName() == "uadd.i270")
    int temp = 0;

  Value *LHS = AddInst->getOperand(0), *RHS = AddInst->getOperand(1);

  unsigned BitWidth = Builder.getBitWidth(LHS);
  assert(BitWidth == Builder.getBitWidth(RHS) && "BitWidth not matches!");

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(AddInst, SM, TD);
  SM->IndexVal2BitMask(AddInst, Mask);

  return AddInst;
}

Value *SIRDatapathBLO::optimizeAddcInst(Instruction *AddcInst) {
  Value *LHS = AddcInst->getOperand(0), *RHS = AddcInst->getOperand(1);
  Value *Carry = AddcInst->getOperand(2);

  unsigned BitWidth = Builder.getBitWidth(LHS);
  assert(BitWidth == Builder.getBitWidth(RHS) && "BitWidth not matches!");
  assert(Builder.getBitWidth(Carry) && "Unexpected Carry BitWidth!");

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(AddcInst, SM, TD);
  SM->IndexVal2BitMask(AddcInst, Mask);

  return AddcInst;
}

Value *SIRDatapathBLO::optimizeMulInst(Instruction *MulInst) {
  if (!AllMulInst.count(MulInst))
    AllMulInst.insert(MulInst);

  Value *LHS = MulInst->getOperand(0), *RHS = MulInst->getOperand(1);

  unsigned TargetBitWidth = Builder.getBitWidth(MulInst);

  // Initialize the mask for this instruction.
  SIRBitMask Mask(Builder.getBitWidth(LHS) + Builder.getBitWidth(RHS));

  // Evaluate the mask.
  Mask.evaluateMask(MulInst, SM, TD);
  SM->IndexVal2BitMask(MulInst, Mask.evaluateBitExtract(Mask, TargetBitWidth, 0));

  if (SM->hasBitMask(LHS) || SM->hasBitMask(RHS)) {
    SIRBitMask LHSMask = SM->getBitMask(LHS);
    SIRBitMask RHSMask = SM->getBitMask(RHS);

    if (LHSMask.hasAnyBitKnown() || RHSMask.hasAnyBitKnown()) {
      if (VisitedMulInst.count(MulInst))
        return MulInst;

      MulMaskOutput << MulInst->getName() << "\n";

      LHSMask.print(MulMaskOutput);
      MulMaskOutput << "\n";
      RHSMask.print(MulMaskOutput);

      VisitedMulInst.insert(MulInst);
    }
  }

  MulMaskOutput << "\n\n";

  return MulInst;
}

Value *SIRDatapathBLO::optimizeUDivInst(Instruction *UDivInst) {
  Value *LHS = UDivInst->getOperand(0), *RHS = UDivInst->getOperand(1);

  unsigned BitWidth = Builder.getBitWidth(LHS);
  assert(BitWidth == Builder.getBitWidth(RHS) && "BitWidth not matches!");

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(UDivInst, SM, TD);
  SM->IndexVal2BitMask(UDivInst, Mask);

  return UDivInst;
}

Value *SIRDatapathBLO::optimizeSDivInst(Instruction *SDivInst) {
  Value *LHS = SDivInst->getOperand(0), *RHS = SDivInst->getOperand(1);

  unsigned BitWidth = Builder.getBitWidth(LHS);
  assert(BitWidth == Builder.getBitWidth(RHS) && "BitWidth not matches!");

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(SDivInst, SM, TD);
  SM->IndexVal2BitMask(SDivInst, Mask);

  return SDivInst;
}

Value *SIRDatapathBLO::optimizeShlInst(Instruction *ShlInst) {
  Value *LHS = ShlInst->getOperand(0), *RHS = ShlInst->getOperand(1);

  unsigned BitWidth = Builder.getBitWidth(LHS);

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(ShlInst, SM, TD);
  SM->IndexVal2BitMask(ShlInst, Mask);

  return ShlInst;
}

Value *SIRDatapathBLO::optimizeLshrInst(Instruction *LshrInst) {
  Value *LHS = LshrInst->getOperand(0), *RHS = LshrInst->getOperand(1);

  unsigned BitWidth = Builder.getBitWidth(LHS);

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(LshrInst, SM, TD);
  SM->IndexVal2BitMask(LshrInst, Mask);

  return LshrInst;
}

Value *SIRDatapathBLO::optimizeAshrInst(Instruction *AshrInst) {
  Value *LHS = AshrInst->getOperand(0), *RHS = AshrInst->getOperand(1);

  unsigned BitWidth = Builder.getBitWidth(LHS);

  // Initialize the mask for this instruction.
  SIRBitMask Mask(BitWidth);

  // Evaluate the mask.
  Mask.evaluateMask(AshrInst, SM, TD);
  SM->IndexVal2BitMask(AshrInst, Mask);

  return AshrInst;
}

Value *SIRDatapathBLO::optimizeUgtInst(Instruction *UgtInst) {
  // Initialize the mask for this instruction.
  SIRBitMask Mask(1);

  // Evaluate the mask.
  Mask.evaluateMask(UgtInst, SM, TD);
  SM->IndexVal2BitMask(UgtInst, Mask);

  return UgtInst;
}

Value *SIRDatapathBLO::optimizeSgtInst(Instruction *SgtInst) {
  // Initialize the mask for this instruction.
  SIRBitMask Mask(1);

  // Evaluate the mask.
  Mask.evaluateMask(SgtInst, SM, TD);
  SM->IndexVal2BitMask(SgtInst, Mask);

  return SgtInst;
}

Value *SIRDatapathBLO::optimizeRegAssignInst(Instruction *RegAssignInst) {
  Value *RegVal = RegAssignInst->getOperand(0), *RegGuard = RegAssignInst->getOperand(1);
  unsigned BitWidth = Builder.getBitWidth(RegVal);


  SIRBitMask Mask(BitWidth);
  if (SM->hasBitMask(RegVal))
    Mask = SM->getBitMask(RegVal);

  if (Instruction *RegValInst = dyn_cast<Instruction>(RegVal))
    Mask.evaluateMask(RegValInst, SM, TD);

  SM->IndexVal2BitMask(RegVal, Mask);
  SM->IndexVal2BitMask(RegAssignInst, Mask);

  return RegAssignInst;
}

Value *SIRDatapathBLO::optimizeInst(Instruction *Inst) {
//   // Replace the instruction with known bits if possible.
//   Value *KnownBits = replaceKnownAllBits(Inst);
//   if (KnownBits != Inst)
//     return KnownBits;

  if (isa<PtrToIntInst>(Inst) || isa<IntToPtrInst>(Inst)) {
    Value *Operand = Inst->getOperand(0);

    unsigned BitWidth = Builder.getBitWidth(Operand);

    // Initialize the mask for this instruction.
    SIRBitMask Mask(BitWidth);

    if (SM->hasBitMask(Operand))
      Mask = SM->getBitMask(Operand);

    SM->IndexVal2BitMask(Inst, Mask);
    return Inst;
  }

  IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst);
  assert(II && "Unexpected instruction type!");

  switch (II->getIntrinsicID()) {
  case Intrinsic::shang_and:
    return optimizeAndInst(II);
  case Intrinsic::shang_not:
    return optimizeNotInst(II);
  case Intrinsic::shang_or:
    return optimizeOrInst(II);
  case Intrinsic::shang_xor:
    return optimizeXorInst(II);
  case Intrinsic::shang_rand:
    return optimizeRandInst(II);
  case Intrinsic::shang_rxor:
    return optimizeRxorInst(II);
  case Intrinsic::shang_bit_cat:
    return optimizeBitCatInst(II);
  case Intrinsic::shang_bit_extract:
    return optimizeBitExtractInst(II);
  case Intrinsic::shang_bit_repeat:
    return optimizeBitRepeatInst(II);
  case Intrinsic::shang_add:
    return optimizeAddInst(II);
  case Intrinsic::shang_addc:
    return optimizeAddcInst(II);
  case Intrinsic::shang_mul:
    return optimizeMulInst(II);
  case Intrinsic::shang_udiv:
    return optimizeUDivInst(II);
  case Intrinsic::shang_sdiv:
    return optimizeSDivInst(II);
  case Intrinsic::shang_shl:
    return optimizeShlInst(II);
  case Intrinsic::shang_lshr:
    return optimizeLshrInst(II);
  case Intrinsic::shang_ashr:
    return optimizeAshrInst(II);
  case Intrinsic::shang_ugt:
    return optimizeUgtInst(II);
  case Intrinsic::shang_sgt:
    return optimizeSgtInst(II);
  case Intrinsic::shang_reg_assign:
    return optimizeRegAssignInst(II);
  }
}

bool SIRDatapathBLO::optimizeValue(Value *Val) {
  Instruction *Inst = dyn_cast<Instruction>(Val);

  if (!Inst)
    return false;

  // Avoid visiting same instruction twice.
  if (Visited.count(Inst))
    return false;

  bool Changed = false;

  std::set<Instruction *> LocalVisited;

  typedef Instruction::op_iterator op_iterator;
  std::vector<std::pair<Instruction *, op_iterator> > VisitStack;

  VisitStack.push_back(std::make_pair(Inst, Inst->op_begin()));

  while(!VisitStack.empty()) {
    Instruction *CurNode = VisitStack.back().first;
    op_iterator &I = VisitStack.back().second;

    // All children of current node have been visited.
    if (I == CurNode->op_end()) {
      VisitStack.pop_back();

      optimizeInst(CurNode);
      //Value *NewNode = replaceKnownAllBits(optimizeInst(CurNode));
      //Changed |= replaceIfNotEqual(CurNode, NewNode);
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    Value *ChildVal = *I;
    ++I;
    Instruction *ChildInst = dyn_cast<Instruction>(ChildVal);

    // TODO: the mask of constant value may be useful.
    // Ignore the non-instruction value.
    if (!ChildInst)
      continue;

    // No need to visit the same node twice.
    if (!LocalVisited.insert(ChildInst).second || Visited.count(ChildInst))
      continue;

    VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
  }

  return Changed;
}

bool SIRDatapathBLO::optimizeRegister(SIRRegister *Reg) {
  assert(Reg->isSynthesized() && "Register synthesized not run yet?");

  bool Changed = false;

  // Initialize a mask for this register.
  SIRBitMask RegMask(Reg->getBitWidth());

  Instruction *RegAssignInst = dyn_cast<Instruction>(Reg->getLLVMValue());

  Changed |= optimizeValue(RegAssignInst->getOperand(0));
  Changed |= optimizeValue(RegAssignInst->getOperand(1));

  return Changed;
}

bool SIRDatapathBLO::optimizeDatapath() {
  bool Changed = false;

  typedef SIR::register_iterator iterator;
  for (iterator I = SM->registers_begin(), E = SM->registers_end(); I != E; ++I) {
    SIRRegister *Reg = I;

    Changed |= optimizeRegister(Reg);
  }

  return Changed;
}

void SIRDatapathBLO::updateRegMux() {
  typedef SIR::register_iterator iterator;
  for (iterator I = SM->registers_begin(), E = SM->registers_end(); I != E; ++I) {
    SIRRegister *Reg = I;

    Value *RegAssign = Reg->getLLVMValue();
    Instruction *RegAssignInst = dyn_cast<Instruction>(RegAssign);

    assert(RegAssignInst && "Unexpected Register LLVM Value!");

    Value *RegVal = RegAssignInst->getOperand(0);
    Value *RegGuard = RegAssignInst->getOperand(1);

    Reg->setMux(RegVal, RegGuard);
  }
}

namespace {
struct SIRBitLevelOpt : public SIRPass {
  static char ID;
  SIRBitLevelOpt() : SIRPass(ID) {
    initializeSIRBitLevelOptPass(*PassRegistry::getPassRegistry());
  }

  void printMask(SIR &SM, raw_fd_ostream &Output);
  bool runIteration(SIRDatapathBLO &BLO);
  bool runOnSIR(SIR &SM);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(SIRRegisterSynthesisForAnnotationID);
    AU.setPreservesAll();
  }
};
}

char SIRBitLevelOpt::ID = 0;
char &llvm::SIRBitLevelOptID = SIRBitLevelOpt::ID;
INITIALIZE_PASS_BEGIN(SIRBitLevelOpt, "sir-bit-level-opt",
                      "Perform the bit-level optimization",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRRegisterSynthesisForAnnotation)
INITIALIZE_PASS_END(SIRBitLevelOpt, "sir-bit-level-opt",
                      "Perform the bit-level optimization",
                      false, true)

void SIRBitLevelOpt::printMask(SIR &SM, raw_fd_ostream &Output) {
  // Visit the basic block in topological order.
  Function *F = SM.getFunction();

  unsigned MaskNum = 0;

  typedef Function::iterator bb_iterator;
  typedef BasicBlock::iterator inst_iterator;
  for (bb_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    BasicBlock *BB = BI;

    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;

      if (!SM.hasBitMask(Inst)) continue;
      SIRBitMask Mask = SM.getBitMask(Inst);

      if (Mask.hasAnyBitKnown()) {
        MaskNum++;

        Output << Inst->getName() << "\t";
        Mask.print(Output);
        Output << "\n";
      }
    }
  }

  Output << "Total useful mask numbers is " << MaskNum;
  Output << "\n\n\n";
}

bool SIRBitLevelOpt::runIteration(SIRDatapathBLO &BLO) {
  bool Changed = false;

  Changed |= BLO.optimizeDatapath();

  if (Changed)
    BLO.updateRegMux();

  ++NumIterations;
  return Changed;
}

bool SIRBitLevelOpt::runOnSIR(SIR &SM) {
  DataLayout &TD = getAnalysis<DataLayout>();

  std::string MulMaskOutputPath = LuaI::GetString("MulMaskOutput");
  std::string MulError;
  raw_fd_ostream MulOutput(MulMaskOutputPath.c_str(), MulError);
  SIRDatapathBLO BLO(&SM, &TD, MulOutput);

  // Get the output path for Verilog code.
  std::string MaskOutputPath = LuaI::GetString("MaskOutput");
  std::string Error;
  raw_fd_ostream Output(MaskOutputPath.c_str(), Error);

  runIteration(BLO);
  printMask(SM, Output);

  MulOutput << AllMulInst.size();

  return false;
}