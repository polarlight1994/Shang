//===-- IterativeScheduling.cpp - the Iterative Scheduling Pass -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the IterativeScheduling pass.
// The IterativeScheduling pass repeat the process of "schedule then update
// estimation" multiple times to achieve timing closure.
//
//===----------------------------------------------------------------------===//
//

#include "TimingNetlist.h"
#include "VASTScheduling.h"

#include "shang/Passes.h"
#include "shang/Strash.h"
#include "shang/VASTModule.h"
#include "shang/VASTModulePass.h"

#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "shang-iterative-scheduling"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<unsigned> MaxIteration("shang-max-scheduling-iteration",
  cl::desc("Perform memory optimizations e.g. coalescing or banking"),
  cl::init(2));

namespace {
typedef TimingNetlist::delay_type delay_type;
typedef std::map<Value*, delay_type> SrcDelayInfo;
typedef std::map<Value*, SrcDelayInfo> PathDelayInfo;
typedef StringMap<SrcDelayInfo> FaninDelayInfo;

template<typename SubClass>
struct DatapathVisitor {
  void visit(VASTModule &VM) {
    // Extract/Annotate the delay for the SeqOp and the state-transition
    // condition.
    typedef VASTModule::slot_iterator slot_iterator;
    for (slot_iterator SI = VM.slot_begin(), SE = VM.slot_end(); SI != SE; ++SI) {
      VASTSlot *S = SI;

      typedef VASTSlot::const_op_iterator op_iterator;

      // Print the logic of the datapath used by the SeqOps.
      for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
        if (VASTSeqInst *Op = dyn_cast<VASTSeqInst>(*I)) {
          visitOperands(Op);
          continue;
        }

        // Also extract the transition condition between states.
        if (VASTSlotCtrl *SlotCtrl = dyn_cast<VASTSlotCtrl>(*I)) {
          Instruction *Inst = dyn_cast_or_null<Instruction>(SlotCtrl->getValue());
          if (Inst == 0 || isa<UnreachableInst>(Inst)) continue;

          assert(Inst->getNumOperands() && "Expect operand for the condition!");
          visitCone(VASTValPtr(SlotCtrl->getGuard()).get(), Inst->getOperand(0));
        }
      }
    }
  }

  Value *getGuardingCondition(VASTSlot *S) {
    VASTSlot *PredSlot = S->getParentGroup();
    BasicBlock *PredBB = PredSlot->getParent();

    // Ignore the VASTSlot which does not have a corresponding basic block.
    if (PredBB == 0) return 0;

    TerminatorInst *Inst = PredBB->getTerminator();
    assert(Inst->getNumOperands() && "Expect operand for the condition!");
    return Inst->getOperand(0);
  }

  void visitOperands(VASTSeqInst *Op) {
    Instruction *Inst = dyn_cast<Instruction>(Op->getValue());
    if (!Inst) {
      assert(isa<Argument>(Op->getValue())
             && "Uexpected VASTSeqInst without Instruction!");
      return;
    }

    VASTSlot *ParentSlot = Op->getSlot();

    // Get the VASTValue and the corresponding LLVM Operand according to the
    // underlying LLVM Instruction.
    switch (Inst->getOpcode()) {
    case Instruction::Load: {
      if (Op->getSeqOpType() == VASTSeqInst::Latch) return;

      LoadInst *LI = cast<LoadInst>(Inst);
      VASTLatch Addr = Op->getSrc(0);
      visitConeAndSelector(Addr, LI, LI->getPointerOperand());
      visitGuardingConditionCone(Addr, ParentSlot, LI);
      return;
    }
    case Instruction::Store: {
      StoreInst *SI = cast<StoreInst>(Inst);
      VASTLatch Addr = Op->getSrc(0);
      visitConeAndSelector(Addr, SI, SI->getPointerOperand());
      visitGuardingConditionCone(Addr, ParentSlot, SI);

      VASTLatch Data = Op->getSrc(1);
      visitConeAndSelector(Data, SI, SI->getValueOperand());
      visitGuardingConditionCone(Data, ParentSlot, SI);
      return;
    }
    case Instruction::PHI: {
      PHINode *PN = cast<PHINode>(Inst);
      // Because the assignments to PHINodes are always conditional executed,
      // the parent slot of the assignment should always be a subgroup.
      assert(ParentSlot->IsSubGrp
             && "Expect SubGrp as the parent of PHI assignment!");
      VASTSlot *IncomingSlot = ParentSlot->getParentGroup();
      BasicBlock *BB = IncomingSlot->getParent();
      Value *Incoming = PN->getIncomingValueForBlock(BB);
      VASTLatch L = Op->getSrc(0);

      visitConeAndSelector(L, PN, Incoming);
      visitGuardingConditionCone(L, ParentSlot, PN);
      return;
    }
    case Instruction::Ret: {
      ReturnInst *RI = cast<ReturnInst>(Inst);
      assert(RI->getNumOperands() && "'ret void' should not be a VASTSeqInst!");
      VASTLatch Addr = Op->getSrc(0);
      visitConeAndSelector(Addr, RI, RI->getReturnValue());
      return;
    }
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::URem:
    case Instruction::SRem: {
      // Handle the binary operators.
      if (Op->getSeqOpType() == VASTSeqInst::Latch) return;

      visitConeAndSelector(Op->getSrc(0), Inst, Inst->getOperand(0));
      visitGuardingConditionCone(Op->getSrc(0), ParentSlot, Inst);

      visitConeAndSelector(Op->getSrc(1), Inst, Inst->getOperand(1));
      visitGuardingConditionCone(Op->getSrc(1), ParentSlot, Inst);
      return;
    }
    default: llvm_unreachable("Unexpected opcode!"); return;
    }
  }

  void visitGuardingConditionCone(VASTLatch L, VASTSlot *ParentSlot,
                                  Instruction *Inst) {
    // Guarding condition only presents when the parent slot is a sub group.
    if (!ParentSlot->IsSubGrp) return;

    Value *ConditionOperand = getGuardingCondition(ParentSlot);

    VASTValue *Condition = VASTValPtr(L.getGuard()).get();
    if (!visitCone(Condition, ConditionOperand)) return;

    // If the cone exsit, also extract/annotate the delay from root of the
    // cone to the selector.
    static_cast<SubClass*>(this)->visitSelFanin(L.getSelector(), Inst,
                                                Condition, ConditionOperand);
  }

  void visitConeAndSelector(VASTLatch L, Instruction *Inst, Value *Operand) {
    VASTValue *Root = VASTValPtr(L).get();
    if (!visitCone(Root, Operand)) return;

    // If the cone exsit, also extract/annotate the delay from root of the
    // cone to the selector.
    static_cast<SubClass*>(this)->visitSelFanin(L.getSelector(), Inst,
                                                Root, Operand);
  }

  bool visitCone(VASTValue *Root, Value *Operand) {
    // There is not a cone at all if the Root is a SeqValue.
    if (isa<VASTSeqValue>(Root)) return true;

    // Dirty Hack: Temporary ignore the place holder for the direct output of
    // block RAMs/some functional units.
    // TODO: Add the delay from the corresponding launch operation?
    if (isa<VASTWire>(Root)) return false;

    // Annotate/Extract the delay from the leaves of this cone.
    std::set<VASTSeqValue*> Srcs;
    Root->extractSupporingSeqVal(Srcs);

    if (Srcs.empty()) return false;

    typedef std::set<VASTSeqValue*>::iterator iterator;
    for (iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
      visitPair(Root, Operand, *I);

    return true;
  }

  void visitPair(VASTValue *Dst, Value *DstV, VASTSeqValue *Src) {
    Value *SrcV = Src->getLLVMValue();
    assert(SrcV && "Cannot get the corresponding value!");
    static_cast<SubClass*>(this)->visitPair(Dst, DstV, Src, SrcV);
  }
};

/// DelayExtractor - Extract the delay from the timing netlist.
///
struct DelayExtractor : public DatapathVisitor<DelayExtractor> {
  TimingNetlist &TNL;
  PathDelayInfo &PathInfo;
  FaninDelayInfo &FaninInfo;

  DelayExtractor(TimingNetlist &TNL, PathDelayInfo &PathInfo,
                 FaninDelayInfo &FaninInfo)
    : TNL(TNL), PathInfo(PathInfo), FaninInfo(FaninInfo) {}

  void visitPair(VASTValue *Dst, Value *DstV, VASTSeqValue *Src, Value *SrcV) {
    delay_type &OldDelay = PathInfo[DstV][SrcV];
    delay_type NewDelay = TNL.getDelay(Src, Dst);

    // Remove the latency of the single cycle path from the output to the
    // latching register.
    if (Src->isFUOutput())
      NewDelay = std::max(0.0f, NewDelay - 1.0f);

    // FIXME: Use better update algorithm, e.g. somekinds of iir filter.
    OldDelay = std::max(OldDelay, NewDelay);
  }

  void visitSelFanin(VASTSelector *Sel, Value *User,
                     VASTValue *FI, Value *Operand) {
    StringRef SelName = Sel->getName();
    if (Sel->num_defs()) {
      // If the Selector have any definition used by the datapath, the
      // corresponding LLVM IR should have a name!
      assert(!User->getName().empty() && "Unexpected empty name!");
      SelName = User->getName();
    }

    delay_type &delay = FaninInfo[SelName][Operand];
    delay = std::max(delay, TNL.getDelay(FI, Sel));
  }
};

/// DelayAnnotator - Back annotate the delay to the timing netlist.
///
struct DelayAnnotator : public DatapathVisitor<DelayAnnotator> {
  TimingNetlist &TNL;
  PathDelayInfo &PathInfo;
  FaninDelayInfo &FaninInfo;

  DelayAnnotator(TimingNetlist &TNL, PathDelayInfo &PathInfo,
                 FaninDelayInfo &FaninInfo)
    : TNL(TNL), PathInfo(PathInfo), FaninInfo(FaninInfo) {}

  void visitPair(VASTValue *Dst, Value *DstV, VASTSeqValue *Src, Value *SrcV) {
    TNL.annotateDelay(Src, Dst, PathInfo[DstV][SrcV]);
  }

  void visitSelFanin(VASTSelector *Sel, Value *User,
                     VASTValue *FI, Value *Operand) {
    StringRef SelName = Sel->getName();
    if (Sel->num_defs()) {
      // If the Selector have any definition used by the datapath, the
      // corresponding LLVM IR should have a name!
      assert(!User->getName().empty() && "Unexpected empty name!");
      SelName = User->getName();
    }

    TNL.annotateDelay(FI, Sel, FaninInfo[SelName][Operand]);
  }
};

/// The table to hold the data-path delay across the scheduling iteration.
class StatisticTimingEstimator {
public:
private:
  PathDelayInfo PathInfo;
  FaninDelayInfo FaninInfo;

  TimingNetlist &TNL;
public:
  explicit StatisticTimingEstimator(TimingNetlist &TNL) : TNL(TNL) {}

  void reset() {
    PathInfo.clear();
    FaninInfo.clear();
  }

  void extractDelay(VASTModule &VM) {
    DelayExtractor(TNL, PathInfo, FaninInfo).visit(VM);
  }

  void annotateDelay(VASTModule &VM) {
    DelayAnnotator(TNL, PathInfo, FaninInfo).visit(VM);
  }
};

struct IterativeScheduling : public VASTModulePass {
  static char ID;
  IterativeScheduling() : VASTModulePass(ID) {
    initializeIterativeSchedulingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const  {
    VASTModulePass::getAnalysisUsage(AU);
    // The passes required by VASTScheduling.
    AU.addRequired<AliasAnalysis>();
    AU.addRequired<DominatorTree>();
    AU.addRequired<LoopInfo>();
    AU.addRequired<BranchProbabilityInfo>();

    // The passes required by Timing netlist rewriting.
    AU.addRequired<CachedStrashTable>();
    AU.addRequired<TimingNetlist>();
  }

  void remapLUT(VASTModule &VM);
  void synthesisControlLogic(VASTModule &VM);

  bool runOnVASTModule(VASTModule &VM);
};
}
//===----------------------------------------------------------------------===//

char IterativeScheduling::ID = 0;

INITIALIZE_PASS_BEGIN(IterativeScheduling, "shang-iterative-scheduling",
                      "Preform iterative scheduling",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(CachedStrashTable)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(VASTScheduling)
INITIALIZE_PASS_END(IterativeScheduling, "shang-iterative-scheduling",
                    "Preform iterative scheduling",
                    false, true)

Pass *llvm::createIterativeSchedulingPass() {
  return new IterativeScheduling();
}

void IterativeScheduling::remapLUT(VASTModule &VM) {
  Pass *P = createLUTMappingPass();
  AnalysisResolver *AR = new AnalysisResolver(*getResolver());
  P->setResolver(AR);
  static_cast<VASTModulePass*>(P)->runOnVASTModule(VM);
  delete P;
}

void IterativeScheduling::synthesisControlLogic(VASTModule &VM) {
  PassRegistry *Registry = PassRegistry::getPassRegistry();
  Pass *P = Registry->getPassInfo(&ControlLogicSynthesisID)->createPass();
  AnalysisResolver *AR = new AnalysisResolver(*getResolver());
  P->setResolver(AR);
  static_cast<VASTModulePass*>(P)->runOnVASTModule(VM);
  delete P;
}

bool IterativeScheduling::runOnVASTModule(VASTModule &VM) {
  StringSet<> ExprNames;
  VASTModule *Mod = &VM;

  VASTScheduling Scheduler;
  AnalysisResolver *AR = new AnalysisResolver(*getResolver());
  Scheduler.setResolver(AR);

  // Fast path for the trivial case.
  if (MaxIteration == 1) {
    Scheduler.runOnVASTModule(*Mod);
    return true;
  }

  CachedStrashTable &CachedStrash = getAnalysis<CachedStrashTable>();
  TimingNetlist &TNL = getAnalysis<TimingNetlist>();
  StatisticTimingEstimator STE(TNL);

  // Run the StatisticTimingEstimator on the unscheduled module.
  STE.extractDelay(*Mod);

  Scheduler.runOnVASTModule(*Mod);

  for (unsigned i = 1, e = MaxIteration; i < e; ++i) {
    // Build the necessary logic for netlist generation.
    remapLUT(*Mod);
    synthesisControlLogic(*Mod);
    Mod->gc();

    // Name the datapath nodes.
    ExprNames.clear();
    Mod->nameDatapath(ExprNames, &CachedStrash);

    // Run the timing estimation again.
    TNL.releaseMemory();
    TNL.runOnVASTModule(*Mod);
    STE.extractDelay(*Mod);

    // Build a new module for the current iteration of scheduling.
    Mod = rebuildModule();
    // Reset the contents after the module is rebuilt.
    CachedStrash.releaseMemory();
    TNL.releaseMemory();
    Scheduler.releaseMemory();

    // Backannotate the delay after the module is rebuilt.
    STE.annotateDelay(*Mod);

    // Schedule the module again.
    Scheduler.runOnVASTModule(*Mod);
  }

  return true;
}

//===----------------------------------------------------------------------===//
