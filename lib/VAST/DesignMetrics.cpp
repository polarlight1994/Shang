//===- DesignMetrics.cpp - Estimate the metrics of the design ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
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
#include "vast/DesignMetrics.h"
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

namespace llvm {
// FIXME: Move the class definition to a header file.
class DesignMetricsImpl : public DatapathBuilderContext {
  // Data-path container to hold the optimized data-path of the design.
  DatapathContainer DPContainer;
  DatapathBuilder Builder;

  // The data-path value which are used by control-path operations.
  typedef std::set<VASTValue*> ValSetTy;
  ValSetTy LiveOutedVal;

  ValSetTy AddressBusFanins, DataBusFanins;
  // The lower bound of the total control-steps.
  unsigned StepLB;
  unsigned NumCalls;
  // TODO: Model the control-path, in the control-path, we can focus on the MUX
  // in the control-path, note that the effect of FU allocation&binding
  // algorithm should also be considered when estimating resource usage.

  // TODO: To not perform cycle-accurate speed performance estimation at the IR
  // layer, instead we should only care about the number of memory accesses.

  using VASTExprBuilderContext::getConstant;

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

  // Visit the expression tree whose root is Root and return the cost of the
  // tree, insert all visited data-path nodes into Visited.
  uint64_t getExprTreeFUCost(VASTValPtr Root, std::set<VASTExpr*> &Visited) const;

  // Collect the fanin information of the memory bus.
  void visitLoadInst(LoadInst &I);
  void visitStoreInst(StoreInst &I);
public:
  explicit DesignMetricsImpl(DataLayout *TD)
    : DatapathBuilderContext(TD), Builder(*this), StepLB(0), NumCalls(0){}

  ~DesignMetricsImpl() {}

  void visit(Instruction &Inst);
  void visit(BasicBlock &BB);
  void visit(Function &F);

  void reset() {
    DPContainer.reset();
    LiveOutedVal.clear();
    AddressBusFanins.clear();
    DataBusFanins.clear();
    StepLB = 0;
    NumCalls = 0;
  }

  uint64_t getFUCost(VASTValue *V) const;

  // Visit all data-path expression and compute the cost.
  uint64_t getDatapathFUCost() const;
  unsigned getNumAddrBusFanin() const { return AddressBusFanins.size(); }
  unsigned getNumDataBusFanin() const { return DataBusFanins.size(); }
  unsigned getStepsLowerBound() const { return StepLB; }
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

  if (VASTValPtr V = Builder.lookupExpr(Op)) return V;

  unsigned NumBits = Builder.getValueSizeInBits(Op);

  // Else we need to create a leaf node for the expression tree.
  VASTWrapper *ValueOp
    = DPContainer.getAllocator().Allocate<VASTWrapper>();
    
  new (ValueOp) VASTWrapper("", NumBits, Op);

  // Remember the newly create VASTLLVMValue, so that it will not be created
  // again.
  indexVASTExpr(Op, ValueOp);
  return ValueOp;
}

void DesignMetricsImpl::visitLoadInst(LoadInst &I) {
  Value *Address = I.getPointerOperand();
  if (VASTValPtr V = getAsOperandImpl(Address))
    AddressBusFanins.insert(V.get());
}

void DesignMetricsImpl::visitStoreInst(StoreInst &I) {
  Value *Address = I.getPointerOperand();
  if (VASTValPtr V = getAsOperandImpl(Address))
    AddressBusFanins.insert(V.get());

  Value *Data = I.getValueOperand();
  if (VASTValPtr V = getAsOperandImpl(Data))
    DataBusFanins.insert(V.get());
}

void DesignMetricsImpl::visit(Instruction &Inst) {
  if (VASTValPtr V = Builder.visit(Inst)) {
    indexVASTExpr(&Inst, V);
    return;
  }

  // Else Inst is a control-path instruction, all its operand are live-outed.
  // A live-outed data-path expression and its children should never be
  // eliminated.
  typedef Instruction::op_iterator op_iterator;
  for (op_iterator I = Inst.op_begin(), E = Inst.op_end(); I != E; ++I) {
    if (VASTValPtr V = Builder.lookupExpr(*I))
      LiveOutedVal.insert(V.get());
  }

  if (LoadInst *LI = dyn_cast<LoadInst>(&Inst))
    visitLoadInst(*LI);
  else if (StoreInst *SI = dyn_cast<StoreInst>(&Inst))
    visitStoreInst(*SI);
  else if (isa<CallInst>(Inst)
           || Inst.getOpcode() == Instruction::SDiv
           || Inst.getOpcode() == Instruction::UDiv
           || Inst.getOpcode() == Instruction::SRem
           || Inst.getOpcode() == Instruction::URem)
    ++NumCalls;
  else // Unknown trivial instructions.
    return;

  // These control-path operations must be scheduled to its own step.
  ++StepLB;
}

void DesignMetricsImpl::visit(BasicBlock &BB) {
  typedef BasicBlock::iterator iterator;
  for (iterator I = BB.begin(), E = BB.end(); I != E; ++I) {
    // PHINodes will be handled in somewhere else.
    if (isa<PHINode>(I)) continue;

    visit(*I);
  }

  // Remember the incoming value from the current BB of the PHINodes in
  // successors as live-outed values.
  for (succ_iterator SI = succ_begin(&BB), SE = succ_end(&BB); SI != SE; ++SI) {
    BasicBlock *SuccBB = *SI;
    for (iterator I = SuccBB->begin(), E = SuccBB->end(); I != E; ++I) {
      PHINode *PN = dyn_cast<PHINode>(I);
      if (PN == 0) break;

      Value *LiveOutedFromBB = PN->DoPHITranslation(SuccBB, &BB);
      if (VASTValPtr V = Builder.lookupExpr(LiveOutedFromBB))
        LiveOutedVal.insert(V.get());
    }
  }

}

void DesignMetricsImpl::visit(Function &F) {
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator iterator;
  for (iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    visit(**I);
}

uint64_t DesignMetricsImpl::getFUCost(VASTValue *V) const {
  VASTExpr *Expr = dyn_cast<VASTExpr>(V);
  // We can only estimate the cost of VASTExpr.
  if (!Expr) return 0;

  unsigned ValueSize = std::min(V->getBitWidth(), 64u);

  switch (Expr->getOpcode()) {
  default: break;

  case VASTExpr::dpAdd: return LuaI::Get<VFUAddSub>()->lookupCost(ValueSize);
  case VASTExpr::dpMul: return LuaI::Get<VFUMult>()->lookupCost(ValueSize);
  case VASTExpr::dpSGT:
  case VASTExpr::dpUGT: return LuaI::Get<VFUICmp>()->lookupCost(ValueSize);
  case VASTExpr::dpShl:
  case VASTExpr::dpSRA:
  case VASTExpr::dpSRL: return LuaI::Get<VFUShift>()->lookupCost(ValueSize);
  }

  return 0;
}

namespace {
struct CostAccumulator {
  const DesignMetricsImpl &Impl;
  uint64_t Cost;

  CostAccumulator(const DesignMetricsImpl &Impl) : Impl(Impl), Cost(0) {}

  void operator()(VASTNode *N) {
    if (VASTValue *V = dyn_cast<VASTValue>(N))
      Cost += Impl.getFUCost(V);
  }
};
}

uint64_t DesignMetricsImpl::getExprTreeFUCost(VASTValPtr Root,
                                              std::set<VASTExpr*> &Visited)
                                              const {
  CostAccumulator Accumulator(*this);
  if (VASTExpr *Expr = dyn_cast<VASTExpr>(Root.get()))
    Expr->visitConeTopOrder(Visited, Accumulator);

  return Accumulator.Cost;
}

uint64_t DesignMetricsImpl::getDatapathFUCost() const {
  uint64_t Cost = 0;
  std::set<VASTExpr*> Visited;

  typedef ValSetTy::const_iterator iterator;
  for (iterator I = LiveOutedVal.begin(), E = LiveOutedVal.end(); I != E; ++I)
    Cost += getExprTreeFUCost(*I, Visited);

  return Cost;
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

FunctionPass *llvm::createDesignMetricsPass() {
  return new DesignMetricsPass();
}
