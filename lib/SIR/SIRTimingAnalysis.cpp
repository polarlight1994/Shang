//--SIRTimingAnalysis.cpp - Abstract Interface for Timing Analysis -*- C++ -*-//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file data-path define the delay estimator based on linear approximation.
//
//===----------------------------------------------------------------------===//
#include "sir/SIRTimingAnalysis.h"
#include "sir/Passes.h"
#include "sir/SIR.h"
#include "sir/SIRPass.h"
#include "sir/SIRBuild.h"

#include "vast/FUInfo.h"
#include "vast/LuaI.h"

#include "llvm/Support/MathExtras.h"
#define DEBUG_TYPE "shang-sir-timing-estimator"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/PostOrderIterator.h"

using namespace llvm;
using namespace vast;

static unsigned LogCeiling(unsigned x, unsigned n) {
  unsigned log2n = Log2_32_Ceil(n);
  return (Log2_32_Ceil(x) + log2n - 1) / log2n;
}

SIRDelayModel::SIRDelayModel(SIR *SM, DataLayout *TD, Instruction *Node)
  : SM(SM), TD(TD), Node(Node) {}

float SIRDelayModel::getDelayInBit(unsigned BitNum) {
  assert(ModelDelay.count(BitNum) && "Unexpected BitNum!");

  return ModelDelay[BitNum];
}

void SIRDelayModel::calcArrival() {
  // These instructions have not been transformed into SIR,
  // but clearly they cost no delay.
  if (isa<PtrToIntInst>(Node) || isa<IntToPtrInst>(Node) || isa<BitCastInst>(Node))
    return calcArrivalParallel(0.0f);

  // Since all data-path instruction in SIR is Intrinsic Inst.
  // So the opcode of data-path instruction is its InstrisicID.
  IntrinsicInst *I = dyn_cast<IntrinsicInst>(Node);
  assert(I && "Unexpected non-IntrinsicInst!");

  Intrinsic::ID ID = I->getIntrinsicID();

  switch (ID) {
  case Intrinsic::shang_bit_cat:
  case Intrinsic::shang_bit_repeat:
  case Intrinsic::shang_bit_extract:
  case Intrinsic::shang_not:
    return calcArrivalParallel(0.0f);

  case Intrinsic::shang_and:
  case Intrinsic::shang_or:
  case Intrinsic::shang_xor: {
    // The Input BitWidth is InputNums * BitWidth, the output
    // BitWidth is BitWidth, and each logic level can shrink
    // the width by LUTSize times, so the number of levels is
    // calculated by log operation. To be noted that, in LLVM
    // IR the return value is counted in Operands, so the real
    // numbers of operands should be minus one.
    unsigned IONums = Node->getNumOperands() - 1;
    unsigned LogicLevels = LogCeiling(IONums, VFUs::MaxLutSize);
    return calcArrivalParallel(LogicLevels * VFUs::LUTDelay);
  }
  case Intrinsic::shang_rand: {
    // The Input BitWidth is BitWidth, the output BitWidth is 1,
    // and each logic level can shrink the width by LUTSize times,
    // so the number of levels is calculated by log operation.
    unsigned IONums = TD->getTypeSizeInBits(Node->getOperand(0)->getType());
    unsigned LogicLevels = LogCeiling(IONums, VFUs::MaxLutSize);
    return calcArrivalParallel(LogicLevels * VFUs::LUTDelay);
  }

  case Intrinsic::shang_add:
  case Intrinsic::shang_addc:
    return calcAddArrival();
  case Intrinsic::shang_mul:
    return calcMulArrival();

  case Intrinsic::shang_sdiv:
  case Intrinsic::shang_udiv:
    // Hack: Need to add the lookUpDelay function of Div into VFUs.
    return calcArrivalParallel(345.607);

  case Intrinsic::shang_shl:
  case Intrinsic::shang_ashr:
  case Intrinsic::shang_lshr:
    return calcShiftArrival();

  case Intrinsic::shang_sgt:
  case Intrinsic::shang_ugt:
    return calcCmpArrivial();

  case  Intrinsic::shang_reg_assign:
    // To be noted that, reg_assign instruction is created
    // to represent the SeqVal stored in register, so it
    // will devote 0 delay.
    return calcArrivalParallel(0.0f);

  default:
    llvm_unreachable("Unexpected opcode!");
    break;
  }
}

void SIRDelayModel::calcArrivalParallel(float delay) {
  unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());

  for (int i = 0; i < BitWidth; ++i)
    ModelDelay.insert(std::make_pair(i, delay));

  // Also index the critical path delay as (BitWidth, CriticalDelay)
  ModelDelay.insert(std::make_pair(BitWidth, delay));
}

void SIRDelayModel::calcArrivalLinear(float Base, float PerBit) {
  unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());

  for (int i = 0; i < BitWidth; ++i)
    ModelDelay.insert(std::make_pair(i, Base + i * PerBit));

  float CriticalDelay = PerBit >= 0 ? (Base + (BitWidth - 1) * PerBit)
    : Base;

  // Also index the critical path delay as (BitWidth, CriticalDelay)
  ModelDelay.insert(std::make_pair(BitWidth, CriticalDelay));
}

void SIRDelayModel::calcAddArrival() {
  unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());
  float Delay = LuaI::Get<VFUAddSub>()->lookupLatency(std::min(BitWidth, 64u));

  // Calculate the Base and PerBit. In fact, if we build add chain expression
  // like a + b + c + ..., then the Base and PerBit should be modified because
  // the delay is less that n * DelayOfAdd.
  float PerBit = Delay / BitWidth;
  float Base = PerBit;

  calcArrivalLinear(Base, PerBit);
}

void SIRDelayModel::calcMulArrival() {
  unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());
  float Delay = LuaI::Get<VFUMult>()->lookupLatency(std::min(BitWidth, 64u));

  // Calculate the Base and PerBit. In fact, if we build add chain expression
  // like a + b + c + ..., then the Base and PerBit should be modified because
  // the delay is less that n * DelayOfAdd.
  float PerBit = Delay / BitWidth;
  float Base = PerBit;

  calcArrivalLinear(Base, PerBit);
}

void SIRDelayModel::calcCmpArrivial() {
  unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());
  float Delay = LuaI::Get<VFUICmp>()->lookupLatency(std::min(BitWidth, 64u));

  calcArrivalParallel(Delay);
}

void SIRDelayModel::calcShiftArrival() {
  Value *V = Node->getOperand(0);
  unsigned BitWidth = TD->getTypeSizeInBits(Node->getType());

  float Delay = LuaI::Get<VFUShift>()->lookupLatency(std::min(BitWidth, 64u));

  calcArrivalParallel(Delay);
}

SIRDelayModel *SIRTimingAnalysis::createModel(Instruction *Inst, SIR *SM,
                                              DataLayout &TD) {
  SIRDelayModel *&Model = ModelMap[Inst];
  assert(Model == NULL && "Model had already existed!");

  Model = new SIRDelayModel(SM, &TD, Inst);
  Model->calcArrival();

  Models.push_back(Model);
  ModelMap.insert(std::make_pair(Inst, Model));

  return Model;
}

SIRDelayModel *SIRTimingAnalysis::lookUpDelayModel(Instruction *Inst) const {
  std::map<Instruction *, SIRDelayModel *>::const_iterator I = ModelMap.find(Inst);
  assert(I != ModelMap.end() && "Model of Inst cannot be found!");
  return I->second;
}

void SIRTimingAnalysis::extractArrivals(DataLayout *TD, SIRSeqOp *SeqOp,
                                        ArrivalMap &Arrivals) {
  SIRRegister *DstReg = SeqOp->getDst();

  // Considering two data-path coming to the Op: 1) SrcVal; 2) Guard.
  Value *SrcVal = SeqOp->getSrc();
  Value *Guard = SeqOp->getGuard();

  SmallVector<Value *, 4> Srcs;
  Srcs.push_back(SrcVal);
  Srcs.push_back(Guard);

  for (int i = 0; i < Srcs.size(); i++) {
    Instruction *Inst = dyn_cast<Instruction>(Srcs[i]);

    if (!Inst) {
      assert(isa<ConstantInt>(Srcs[i]) || isa<Argument>(Srcs[i]) ||
        isa<UndefValue>(Srcs[i]) && "Unexpected NULL Inst!");
      continue;
    }

    SIRDelayModel *DM = lookUpDelayModel(Inst);

    unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());
    PhysicalDelay Delay = PhysicalDelay(DM->getDelayInBit(BitWidth));

    PhysicalDelay &OldDelay = Arrivals[Inst];
    OldDelay = std::max(OldDelay, Delay);
  }
}

void SIRTimingAnalysis::extractArrivals(DataLayout *TD, Instruction *CombOp,
                                        ArrivalMap &Arrivals) {
  // Since all data-path instruction in SIR is Intrinsic Inst.
  // So the opcode of data-path instruction is its InstrisicID.
  IntrinsicInst *II = dyn_cast<IntrinsicInst>(CombOp);
  assert(II || isa<IntToPtrInst>(CombOp) || isa<PtrToIntInst>(CombOp) ||
         isa<BitCastInst>(CombOp) && "Unexpected non-IntrinsicInst!");

  // The bit_extract instruction should be handled specially, since the delay
  // of the data dependency is only related to part bits of the SrcVal.
  if (II && II->getIntrinsicID() == Intrinsic::shang_bit_extract) {
    Value *OperandVal = CombOp->getOperand(0);

    // If the Operand is Argument or ConstantInt, then this SUnit has no data dependence.
    if (isa<Argument>(OperandVal) || isa<ConstantInt>(OperandVal))
      return;

    Instruction *Operand = dyn_cast<Instruction>(OperandVal);
    assert(Operand && "Unexpected NULL Operand!");

    SIRDelayModel *DM = lookUpDelayModel(Operand);

    int UB = getConstantIntValue(dyn_cast<ConstantInt>(CombOp->getOperand(1)));
    int LB = getConstantIntValue(dyn_cast<ConstantInt>(CombOp->getOperand(2)));

    float UBDelay = DM->getDelayInBit(UB);
    float LBDelay = DM->getDelayInBit(LB);
    PhysicalDelay Delay = PhysicalDelay(std::max(UBDelay, LBDelay));

    PhysicalDelay &OldDelay = Arrivals[Operand];
    OldDelay = std::max(OldDelay, Delay);
    return;
  }

  SmallVector<Value *, 4> Operands;

  typedef Instruction::op_iterator iterator;
  for (int i = 0; i < CombOp->getNumOperands() - 1; ++i) {
    Value *Operand = CombOp->getOperand(i);

    // Ignore these Values since they have no corresponding DelayModel.
    if (isa<Argument>(Operand) || isa<ConstantInt>(Operand) ||
        isa<UndefValue>(Operand))
      continue;

    Operands.push_back(CombOp->getOperand(i));
  }

  for (int i = 0; i < Operands.size(); ++i) {
    Instruction *Operand = dyn_cast<Instruction>(Operands[i]);
    assert(Operand && "Unexpected NULL Operand!");

    SIRDelayModel *DM = lookUpDelayModel(Operand);
    unsigned BitWidth = TD->getTypeSizeInBits(Operand->getType());
    PhysicalDelay Delay = PhysicalDelay(DM->getDelayInBit(BitWidth));

    PhysicalDelay &OldDelay = Arrivals[Operand];
    OldDelay = std::max(OldDelay, Delay);
  }
}

void SIRTimingAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  SIRPass::getAnalysisUsage(AU);
  AU.addRequired<DataLayout>();
  AU.addRequired<SIRInit>();
  AU.addRequiredID(SIRRegisterSynthesisForAnnotationID);
  AU.setPreservesAll();
}

char SIRTimingAnalysis::ID = 0;
INITIALIZE_PASS_BEGIN(SIRTimingAnalysis,
                      "SIR-timing-analysis",
                      "Implement the timing analysis for SIR",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRRegisterSynthesisForAnnotation)
INITIALIZE_PASS_END(SIRTimingAnalysis,
                    "SIR-timing-analysis",
                    "Implement the timing analysis for SIR",
                    false, true)

bool SIRTimingAnalysis::runOnSIR(SIR &SM) {
  DataLayout &TD = getAnalysis<DataLayout>();
  Function *F = SM.getFunction();

  typedef Function::iterator bb_iterator;
  for (bb_iterator BBI = F->begin(), BBE = F->end(); BBI != BBE; ++BBI) {
    BasicBlock *BB = BBI;

    typedef BasicBlock::iterator inst_iterator;
    for (inst_iterator InstI = BB->begin(), InstE = BB->end(); InstI != InstE; ++InstI) {
      Instruction *Inst = InstI;

      if (!isa<IntrinsicInst>(Inst) && !isa<IntToPtrInst>(Inst) &&
        !isa<PtrToIntInst>(Inst) && !isa<BitCastInst>(Inst))
        continue;

      createModel(Inst, &SM, TD);
    }
  }

  return false;
}