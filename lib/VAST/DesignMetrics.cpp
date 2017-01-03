//===- DesignMetrics.cpp - Estimate the metrics of the design ---*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the DesignMetrics class, which estimate the resource
// usage and speed performance of the design at early stage.
//
//===----------------------------------------------------------------------===//

#include "IR2Datapath.h"
#include "vast/Passes.h"
#include "vast/VASTSeqValue.h"
#include "vast/VASTHandle.h"
#include "vast/DesignMetrics.h"
#include "vast/BitlevelOpt.h"
#include "vast/FUInfo.h"
#include "vast/LuaI.h"

#include "llvm/Pass.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CFG.h"
#define DEBUG_TYPE "vtm-design-metrics"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace vast {
using namespace llvm;

// FIXME: Move the class definition to a header file.
class DesignMetricsImpl : public DatapathBuilderContext {
  // Data-path container to hold the optimized data-path of the design.
  DatapathContainer DPContainer;
  DatapathBuilder Builder;

  // The data-path value which are used by control-path operations.
  std::map<Value*, VASTHandle> Addrs, Datas;
  typedef SmallVector<VASTHandle, 4> FaninVec;
  std::map<Value*, FaninVec> PHIs;
  ilist<VASTSeqValue> Values;

  // The lower bound of the total control-steps.
  unsigned NumCalls;
  // TODO: Model the control-path, in the control-path, we can focus on the MUX
  // in the control-path, note that the effect of FU allocation&binding
  // algorithm should also be considered when estimating resource usage.

  // TODO: To not perform cycle-accurate speed performance estimation at the IR
  // layer, instead we should only care about the number of memory accesses.

  VASTConstant *getConstant(const APInt &Value) {
    return DPContainer.getConstantImpl(Value);
  }

  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned BitWidth) {
    return DPContainer.createExprImpl(Opc, Ops, BitWidth);
  }

  VASTValPtr createBitExtract(VASTValPtr Op, unsigned UB, unsigned LB) {
    return DPContainer.createBitExtractImpl(Op, UB, LB);
  }

  VASTValPtr createROMLookUp(VASTValPtr Addr, VASTMemoryBank *Bank,
                             unsigned BitWidth) {
    return DPContainer.createROMLookUpImpl(Addr, Bank, BitWidth);
  }

  VASTValPtr getAsOperandImpl(Value *Op);

  // Collect the fanin information of the memory bus.
  void visitLoadInst(LoadInst &I);
  void visitStoreInst(StoreInst &I);
public:
  explicit DesignMetricsImpl(DataLayout *TD)
    : DatapathBuilderContext(TD), Builder(*this), NumCalls(0){}

  ~DesignMetricsImpl() {
    reset();
  }

  void visit(Instruction &Inst);
  void visit(BasicBlock &BB);
  void visit(Function &F);

  void reset() {
    PHIs.clear();
    Datas.clear();
    Addrs.clear();
    DPContainer.reset();
    Values.clear();
    NumCalls = 0;
  }

  void optimize();
  bool optimize(DatapathBLO &Opt, std::map<Value*, VASTHandle> &MUXes);
  bool optimize(DatapathBLO &Opt, std::map<Value*, FaninVec> &FIs);

  uint64_t getFUCost(const VASTExpr *Expr) const;

  // Visit all data-path expression and compute the cost.
  uint64_t getDatapathFUCost() const;
  unsigned getNumAddrBusFanin() const { return Addrs.size(); }
  unsigned getNumDataBusFanin() const { return Datas.size(); }
  unsigned getStepsLowerBound() const { return Addrs.size() + getNumCalls(); }
  unsigned getNumCalls() const { return NumCalls; }

  DataLayout *getDataLayout() const { return Builder.getDataLayout(); }
};

struct DesignMetricsPass : public FunctionPass {
  static char ID;

  DesignMetricsPass() : FunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<DataLayout>();
    AU.setPreservesAll();
  }

  bool runOnFunction(Function &F);
};
}

VASTValPtr DesignMetricsImpl::getAsOperandImpl(Value *Op) {
  if (ConstantInt *Int = dyn_cast<ConstantInt>(Op))
    return getConstant(Int->getValue());

  if (VASTValPtr V = Builder.lookupExpr(Op))
    return V;

  unsigned NumBits = Builder.getValueSizeInBits(Op);

  VASTSeqValue *Val = new VASTSeqValue(Op, NumBits);
  Values.push_back(Val);

  // Remember the newly create VASTLLVMValue, so that it will not be created
  // again.
  return indexVASTExpr(Op, Val);
}

void DesignMetricsImpl::visitLoadInst(LoadInst &I) {
  Value *Address = I.getPointerOperand();
  if (VASTValPtr V = getAsOperandImpl(Address))
    Addrs[&I] = V;
}

void DesignMetricsImpl::visitStoreInst(StoreInst &I) {
  Value *Address = I.getPointerOperand();
  if (VASTValPtr V = getAsOperandImpl(Address))
    Addrs[&I] = V;

  Value *Data = I.getValueOperand();
  if (VASTValPtr V = getAsOperandImpl(Data))
    Datas[&I] = V;
}

void DesignMetricsImpl::visit(Instruction &Inst) {
  if (VASTValPtr V = Builder.visit(Inst)) {
    indexVASTExpr(&Inst, V);
    return;
  }

  if (LoadInst *LI = dyn_cast<LoadInst>(&Inst))
    return visitLoadInst(*LI);

  if (StoreInst *SI = dyn_cast<StoreInst>(&Inst))
    return visitStoreInst(*SI);

  if (isa<CallInst>(Inst)
      || Inst.getOpcode() == Instruction::SDiv
      || Inst.getOpcode() == Instruction::UDiv
      || Inst.getOpcode() == Instruction::SRem
      || Inst.getOpcode() == Instruction::URem)
    ++NumCalls;

  // Else Inst is a control-path instruction, all its operand are live-outed.
  // A live-outed data-path expression and its children should never be
  // eliminated.
  typedef Instruction::op_iterator op_iterator;
  for (op_iterator I = Inst.op_begin(), E = Inst.op_end(); I != E; ++I) {
    Value *V = *I;

    if (isa<BasicBlock>(V))
      continue;

    if (VASTValPtr V = Builder.lookupExpr(*I))
      PHIs[&Inst].push_back(V);
  }
}

void DesignMetricsImpl::visit(BasicBlock &BB) {
  typedef BasicBlock::iterator iterator;
  for (iterator I = BB.begin(), E = BB.end(); I != E; ++I) {
    // PHINodes will be handled in somewhere else.
    if (isa<PHINode>(I))
     continue;

    visit(*I);
  }

  // Remember the incoming value from the current BB of the PHINodes in
  // successors as live-outed values.
  for (succ_iterator SI = succ_begin(&BB), SE = succ_end(&BB); SI != SE; ++SI) {
    BasicBlock *SuccBB = *SI;
    for (iterator I = SuccBB->begin(), E = SuccBB->end(); I != E; ++I) {
      PHINode *PN = dyn_cast<PHINode>(I);

      if (PN == NULL)
        break;

      Value *LiveOutedFromBB = PN->DoPHITranslation(SuccBB, &BB);
      if (VASTValPtr V = Builder.lookupExpr(LiveOutedFromBB))
        PHIs[PN].push_back(V);
    }
  }
}

void DesignMetricsImpl::visit(Function &F) {
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator iterator;
  for (iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    visit(**I);
}

static unsigned LogCeiling(unsigned x, unsigned n) {
  unsigned log2n = Log2_32_Ceil(n);
  return (Log2_32_Ceil(x) + log2n - 1) / log2n;
}

static unsigned DivCeiling(unsigned x, unsigned y) {
  return (x + y - 1) / y;
}

uint64_t DesignMetricsImpl::getFUCost(const VASTExpr *Expr) const {
  unsigned ValueSize = std::min(Expr->getBitWidth() - Expr->getNumKnownBits(), 64u);

  switch (Expr->getOpcode()) {
  default: break;
  case VASTExpr::dpLUT:
    return VFUs::LUTCost * ValueSize;
  case VASTExpr::dpAnd: {
    unsigned LL = LogCeiling(Expr->size(), VFUs::MaxLutSize);
    unsigned NumLUTs = DivCeiling(int(pow(double(VFUs::MaxLutSize), double(LL))) - 1,
                                  VFUs::MaxLutSize - 1);
    return VFUs::LUTCost * ValueSize * NumLUTs;
  }
  case VASTExpr::dpRAnd: {
    VASTValPtr V = Expr->getOperand(0);
    unsigned InputBits = V->getBitWidth() - VASTBitMask(V).getNumKnownBits();
    unsigned LL = LogCeiling(InputBits, VFUs::MaxLutSize);
    unsigned NumLUTs = DivCeiling(int(pow(double(VFUs::MaxLutSize), double(LL))) - 1,
                                  VFUs::MaxLutSize - 1);
    return VFUs::LUTCost * NumLUTs;
  }
  case VASTExpr::dpAdd:
    return LuaI::Get<VFUAddSub>()->lookupCost(ValueSize);
  case VASTExpr::dpMul:
    return LuaI::Get<VFUMult>()->lookupCost(ValueSize);
  case VASTExpr::dpSGT:
  case VASTExpr::dpUGT:
    return LuaI::Get<VFUGT_LT>()->lookupCost(Expr->getOperand(0)->getBitWidth());
  case VASTExpr::dpShl:
  case VASTExpr::dpAshr:
  case VASTExpr::dpLshr:
    return LuaI::Get<VFUShift>()->lookupCost(ValueSize);
  }

  return 0;
}

uint64_t DesignMetricsImpl::getDatapathFUCost() const {
  uint64_t Cost = 0;

  typedef DatapathContainer::const_expr_iterator iterator;
  for (iterator I = DPContainer.expr_begin(), E = DPContainer.expr_end();
       I != E; ++I)
    Cost += getFUCost(I);

  return Cost;
}

bool DesignMetricsImpl::optimize(DatapathBLO &Opt,
                                 std::map<Value*, VASTHandle> &Muxes) {
  bool Changed = false;

  typedef std::map<Value*, VASTHandle>::iterator iterator;
  for (iterator I = Muxes.begin(), E = Muxes.end(); I != E; ++I)
    Changed |= Opt.optimizeAndReplace(I->second);

  return Changed;    
}
bool
DesignMetricsImpl::optimize(DatapathBLO &Opt, std::map<Value*, FaninVec> &PHIs) {
  bool Changed = false;

  typedef std::map<Value*, FaninVec>::iterator iterator;
  for (iterator I = PHIs.begin(), E = PHIs.end(); I != E; ++I) {
    ArrayRef<VASTHandle> FIs = I->second;
    for (unsigned i = 0; i < FIs.size(); ++i)
      Changed |= Opt.optimizeAndReplace(FIs[i]);
  }

  return Changed;
}

void DesignMetricsImpl::optimize() {
  bool Changed = true;

  // Optimize the datapath to avoid overestimate the cost.
  DatapathBLO Opt(DPContainer);

  while (Changed) {
    Changed = false;
    Changed |= optimize(Opt, Addrs);
    Changed |= optimize(Opt, Datas);
    Changed |= optimize(Opt, PHIs);

    Opt.performLUTMapping();
    Opt.resetForNextIteration();
  }
}

DesignMetrics::DesignMetrics(DataLayout *TD)
  : Impl(new DesignMetricsImpl(TD)) {}

DataLayout *DesignMetrics::getDataLayout() const {
  return Impl->getDataLayout();
}

DesignMetrics::~DesignMetrics() { delete Impl; }

void DesignMetrics::visit(Instruction &Inst) {
  Impl->visit(Inst);
}

void DesignMetrics::visit(BasicBlock &BB) {
  Impl->visit(BB);
}

void DesignMetrics::visit(Function &F) {
  Impl->visit(F);
}

unsigned DesignMetrics::getNumCalls() const { return Impl->getNumCalls(); }

DesignMetrics::DesignCost::DesignCost(uint64_t DatapathCost,
                                      unsigned NumAddrBusFanin,
                                      unsigned NumDataBusFanin,
                                      unsigned StepLB)
  : DatapathCost(DatapathCost),
    NumAddrBusFanin(NumAddrBusFanin), NumDataBusFanin(NumDataBusFanin),
    StepLB(StepLB) {}

void DesignMetrics::optimize() {
  Impl->optimize();
}

DesignMetrics::DesignCost DesignMetrics::getCost() const {
  return DesignCost(std::max<uint64_t>(Impl->getDatapathFUCost(), 1),
                    Impl->getNumAddrBusFanin(),
                    Impl->getNumDataBusFanin(),
                    Impl->getStepsLowerBound());
}

void DesignMetrics::reset() { Impl->reset(); }

uint64_t DesignMetrics::DesignCost::getCostInc(unsigned Multiply, uint64_t Alpha,
                                               uint64_t Beta, uint64_t Gama) const {
  VFUMux *MUX = LuaI::Get<VFUMux>();
  VFUMemBus *MemBus = LuaI::Get<VFUMemBus>();
  unsigned AddrWidth = MemBus->getAddrWidth();
  unsigned DataWidth = MemBus->getDataWidth();

  return Alpha * (uint64_t(Multiply) - 1) * DatapathCost
         + Beta * (MUX->getMuxCost(Multiply * NumAddrBusFanin, AddrWidth)
                   - MUX->getMuxCost(NumAddrBusFanin, AddrWidth))
         + Beta * (MUX->getMuxCost(Multiply * NumDataBusFanin, DataWidth)
                   - MUX->getMuxCost(NumDataBusFanin, DataWidth))
         + Gama * (uint64_t(Multiply) - 1) * StepLB;
}

void DesignMetrics::DesignCost::print(raw_ostream &OS) const {
  OS << "{ Data-path: " << DatapathCost
     << ", ( AddrBusFI: " << NumAddrBusFanin
     << ", DataBusFI: " << NumDataBusFanin
     << " ), StepLB: " << StepLB << " }";
}

void DesignMetrics::DesignCost::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

char DesignMetricsPass::ID = 0;

bool DesignMetricsPass::runOnFunction(Function &F) {
  DesignMetrics Metrics(&getAnalysis<DataLayout>());

  Metrics.visit(F);

  //DEBUG(dbgs() << "Data-path cost of function " << F.getName() << ':'
  //             << Metrics.getResourceCost() << '\n');
  
  return false;
}

FunctionPass *vast::createDesignMetricsPass() {
  return new DesignMetricsPass();
}
