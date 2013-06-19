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

#include "llvm/PassManagers.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/SourceMgr.h"
#define DEBUG_TYPE "shang-iterative-scheduling"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<unsigned> MaxIteration("shang-max-scheduling-iteration",
  cl::desc("Perform memory optimizations e.g. coalescing or banking"),
  cl::init(1));

static cl::opt<bool> DumpIntermediateNetlist("shang-dump-intermediate-netlist",
  cl::desc("Dump the netlists produced by each iteration"),
  cl::init(false));

namespace llvm {
typedef TimingNetlist::delay_type delay_type;
typedef std::map<Value*, delay_type> SrcDelayInfo;
typedef std::map<Value*, SrcDelayInfo> PathDelayInfo;
typedef StringMap<SrcDelayInfo> FaninDelayInfo;

void initializeDelayInfoStoragePass(PassRegistry &Registry);

class DelayInfoStorage : public ImmutablePass {
  PathDelayInfo PathInfo;
  FaninDelayInfo FaninInfo;

  delay_type getDelayFromSrc(const SrcDelayInfo &SrcInfo, Value *SrcV) const {
    SrcDelayInfo::const_iterator I = SrcInfo.find(SrcV);
    if (I == SrcInfo.end()) return delay_type(0.0f);

    return I->second;
  }
public:
  static char ID;
  DelayInfoStorage() : ImmutablePass(ID) {
    initializeDelayInfoStoragePass(*PassRegistry::getPassRegistry());
  }

  delay_type getPathDelay(Value *DstV, Value *SrcV) const {
    PathDelayInfo::const_iterator I = PathInfo.find(DstV);
    if (I == PathInfo.end()) return delay_type(0.0f);

    return getDelayFromSrc(I->second, SrcV);
  }

  void annotatePathDelay(Value *DstV, Value *SrcV, delay_type delay) {
    PathInfo[DstV][SrcV] = delay;
  }

  delay_type getFaninDelay(StringRef SelName, Value *SrcV) const {
    FaninDelayInfo::const_iterator I = FaninInfo.find(SelName);
    if (I == FaninInfo.end()) return delay_type(0.0f);

    return getDelayFromSrc(I->second, SrcV);
  }

  void annotateFaninDelay(StringRef SelName, Value *SrcV, delay_type delay) {
    FaninInfo[SelName][SrcV] = delay;
  }
};

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
          if (Inst == 0 || isa<UnreachableInst>(Inst) || isa<ReturnInst>(Inst))
            continue;

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
      if (Op->isLatch()) {
        visitFULatchAndSelector( Op->getSrc(0), Inst);
        return;
      }

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
      unsigned FinIdx = RI->getNumOperands();
      // If the finish port is not assigned at the first operand, we are
      // assigning the return value at the first operand.
      if (FinIdx) {
        VASTLatch RetVal = Op->getSrc(0);
        visitConeAndSelector(RetVal, RI, RI->getReturnValue());
        visitGuardingConditionCone(RetVal, ParentSlot, RI);
      }

      visitGuardingConditionCone(Op->getSrc(FinIdx), ParentSlot, RI);
      return;
    }
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::URem:
    case Instruction::SRem: {
      // Handle the binary operators.
      if (Op->isLatch()) {
        visitFULatchAndSelector( Op->getSrc(0), Inst);
        return;
      }

      visitConeAndSelector(Op->getSrc(0), Inst, Inst->getOperand(0));
      visitGuardingConditionCone(Op->getSrc(0), ParentSlot, Inst);

      visitConeAndSelector(Op->getSrc(1), Inst, Inst->getOperand(1));
      visitGuardingConditionCone(Op->getSrc(1), ParentSlot, Inst);
      return;
    }
    default: llvm_unreachable("Unexpected opcode!"); return;
    }
  }

  void visitFULatchAndSelector(const VASTLatch &L, Instruction *Inst) {
    static_cast<SubClass*>(this)->visitFULatch(L, Inst);

    // If the cone exsit, also extract/annotate the delay from root of the
    // cone to the selector.
    static_cast<SubClass*>(this)->visitSelFanin(L.getSelector(), Inst,
                                                VASTValPtr(L).get(), Inst);
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

void initializeDelayExtractorPass(PassRegistry &Registry);
/// DelayExtractor - Extract the delay from the timing netlist.
///
struct DelayExtractor : public VASTModulePass,
                        DatapathVisitor<DelayExtractor> {
  static char ID;
  TimingNetlist *TNL;
  DelayInfoStorage *DIS;
  DelayExtractor() : VASTModulePass(ID), TNL(0) {
    initializeDelayExtractorPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<DelayInfoStorage>();
    AU.addRequired<TimingNetlist>();
    AU.setPreservesAll();
  }

  bool runOnVASTModule(VASTModule &VM) {
    TNL = &getAnalysis<TimingNetlist>();
    DIS = &getAnalysis<DelayInfoStorage>();
    visit(VM);
    return false;
  }

  void visitPair(VASTValue *Dst, Value *DstV, VASTSeqValue *Src, Value *SrcV) {
    delay_type NewDelay = TNL->getDelay(Src, Dst);
    delay_type OldDelay = DIS->getPathDelay(DstV, SrcV);

    if (Src->isFUOutput() && SrcV != DstV) {
      // Remove the latency of the single cycle path from the output to the
      // latching register.
      delay_type NextStageDelay = std::max(0.0f, NewDelay - 1.0f);
      // Calculate the delay from the FU output to the latch pipeline stage.
      delay_type CurStageDelay = DIS->getPathDelay(SrcV, SrcV);
      // Move the removed delay to current stage.
      DIS->annotatePathDelay(SrcV, SrcV,
                             std::max(CurStageDelay, NewDelay - NextStageDelay));
      // Always extract the delay from the latch register instead of the FU
      // output.
      NewDelay = NextStageDelay;
    }

    // FIXME: Use better update algorithm, e.g. somekinds of iir filter.
    DIS->annotatePathDelay(DstV, SrcV, std::max(OldDelay, NewDelay));
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

    delay_type NewDelay = TNL->getDelay(FI, Sel);
    delay_type OldDelay = DIS->getFaninDelay(SelName, Operand);
    DIS->annotateFaninDelay(SelName, Operand, std::max(OldDelay, NewDelay));
  }

  void visitFULatch(const VASTLatch &L, Instruction *Inst) {
    VASTValPtr Src = L;

    TimingNetlist::RegDelaySet Srcs;
    typedef TimingNetlist::RegDelaySet::iterator src_iterator;

    // Extract the delay from the FU output.
    TNL->extractDelay(L.getSelector(), Src.get(), Srcs);
    for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      VASTSeqValue *SrcV = I->first;
      // The sources must be FU output.
      assert(SrcV->isFUOutput() && SrcV->getLLVMValue() == Inst && "Bad latch!");
      delay_type OldDelay = DIS->getPathDelay(Inst, Inst);
      DIS->annotatePathDelay(Inst, Inst, std::max(OldDelay, I->second));
    }
  }
};

/// DelayAnnotator - Back annotate the delay to the timing netlist.
///
struct DelayAnnotator : public DatapathVisitor<DelayAnnotator> {
  TimingNetlist &TNL;
  DelayInfoStorage &DIS;

  DelayAnnotator(TimingNetlist &TNL, DelayInfoStorage &DIS)
    : TNL(TNL), DIS(DIS) {}

  void visitPair(VASTValue *Dst, Value *DstV, VASTSeqValue *Src, Value *SrcV) {
    TNL.annotateDelay(Src, Dst, DIS.getPathDelay(DstV, SrcV));
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

    TNL.annotateDelay(FI, Sel, DIS.getFaninDelay(SelName, Operand));
  }

  void visitFULatch(const VASTLatch &L, Instruction *Inst) {
    VASTValue *Src = VASTValPtr(L).get();

    // Extract the source registers.
    std::set<VASTSeqValue*> Srcs;
    if (VASTExpr *Expr = dyn_cast<VASTExpr>(Src))
      Expr->extractSupporingSeqVal(Srcs);

    typedef std::set<VASTSeqValue*>::iterator src_iterator;
    for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
      VASTSeqValue *SrcV = *I;
      // The sources must be FU output.
      assert(SrcV->isFUOutput() && SrcV->getLLVMValue() == Inst && "Bad latch!");
      TNL.annotateDelay(SrcV, Src, DIS.getPathDelay(Inst, Inst));
    }
  }
};

class IterativeScheduling : public VASTModulePass, public PMDataManager {
  const std::string OriginalRTLOutput, OriginalMCPOutput;
  const StringRef RTLFileName;
public:
  static char ID;
  IterativeScheduling() : VASTModulePass(ID), PMDataManager(),
    OriginalRTLOutput(getStrValueFromEngine("RTLOutput")),
    OriginalMCPOutput(getStrValueFromEngine("MCPDataBase")),
    RTLFileName(sys::path::filename(OriginalRTLOutput)) {
    initializeIterativeSchedulingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    VASTModulePass::getAnalysisUsage(AU);
    AU.addRequired<AliasAnalysis>();
    AU.addRequired<DominatorTree>();
    AU.addRequired<LoopInfo>();
    AU.addRequired<BranchProbabilityInfo>();
    AU.addRequired<TimingNetlist>();
    AU.addRequired<DelayInfoStorage>();
    // Initialize the DelayInfoStorage by the unscheduled netlist.
    AU.addRequired<DelayExtractor>();
  }

  void recoverOutputPath();
  void changeOutputPaths(unsigned i);

  bool runSingleIteration(Function &F) {
    bool Changed = false;

    // Collect inherited analysis from Module level pass manager.
    populateInheritedAnalysis(TPM->activeStack);

    for (unsigned Index = 0; Index < getNumContainedPasses(); ++Index) {
      FunctionPass *FP = getContainedPass(Index);
      bool LocalChanged = false;

      dumpPassInfo(FP, EXECUTION_MSG, ON_FUNCTION_MSG, F.getName());
      dumpRequiredSet(FP);

      initializeAnalysisImpl(FP);

      {
        PassManagerPrettyStackEntry X(FP, F);
        TimeRegion PassTimer(getPassTimer(FP));

        LocalChanged |= FP->runOnFunction(F);
      }

      Changed |= LocalChanged;
      if (LocalChanged)
        dumpPassInfo(FP, MODIFICATION_MSG, ON_FUNCTION_MSG, F.getName());
      dumpPreservedSet(FP);

      verifyPreservedAnalysis(FP);
      removeNotPreservedAnalysis(FP);
      recordAvailableAnalysis(FP);
      removeDeadPasses(FP, F.getName(), ON_FUNCTION_MSG);
    }

    return Changed;
  }

  bool runOnVASTModule(VASTModule &VM) {
    bool Changed = false;
    TimingNetlist &TNL = getAnalysis<TimingNetlist>();
    DelayInfoStorage &DIS = getAnalysis<DelayInfoStorage>();
    Function &F = VM.getLLVMFunction();

    VASTScheduling Scheduler;
    AnalysisResolver *AR = new AnalysisResolver(*getResolver());
    Scheduler.setResolver(AR);
    Scheduler.runOnVASTModule(VM);

    for (unsigned i = 1, e = MaxIteration; i < e; ++i) {
      // Redirect the output path for the intermediate netlist.
      if (DumpIntermediateNetlist) changeOutputPaths(i - 1);

      runSingleIteration(F);
      TNL.releaseMemory();
      Scheduler.releaseMemory();

      VASTModule &NextVM = rebuildModule();
      DelayAnnotator(TNL, DIS).visit(NextVM);
      Scheduler.runOnVASTModule(NextVM);
    }

    // Fix the output pass if necessary.
    if (DumpIntermediateNetlist) recoverOutputPath();

    return Changed;
  }

  void assignPassManager(PMStack &PMS, PassManagerType T) {
    FunctionPass::assignPassManager(PMS, T);

    // Inherit the existing analyses
    populateInheritedAnalysis(PMS);

    // Setup the pass managers stack.
    PMDataManager *PMD = PMS.top();
    PMTopLevelManager *TPM = PMD->getTopLevelManager();
    TPM->addIndirectPassManager(this);
    PMS.push(this);

    // Add the passes for each single iteration.
    schedulePass(createLUTMappingPass());
    schedulePass(new DelayExtractor());

    // Finish the whole HLS process if we want to dump the intermediate netlist
    // for each iteration.
    if (DumpIntermediateNetlist) {
      schedulePass(createSelectorPipeliningPass());
      schedulePass(createRTLCodeGenPass());
      schedulePass(createTimingScriptGenPass());
    }
  }

  VASTModulePass *getContainedPass(unsigned N) {
    assert ( N < PassVector.size() && "Pass number out of range!");
    VASTModulePass *FP = static_cast<VASTModulePass *>(PassVector[N]);
    return FP;
  }

  /// cleanup - After running all passes, clean up pass manager cache.
  void cleanup() {
    for (unsigned Index = 0; Index < getNumContainedPasses(); ++Index) {
      FunctionPass *FP = getContainedPass(Index);
      AnalysisResolver *AR = FP->getResolver();
      assert(AR && "Analysis Resolver is not set");
      AR->clearAnalysisImpls();
    }
  }

  void schedulePass(Pass *P);

  virtual PMDataManager *getAsPMDataManager() { return this; }
  virtual Pass *getAsPass() { return this; }

  virtual const char *getPassName() const {
    return "Iterative Scheduling Driver";
  }

  // Pretend we are a basic block pass manager to prevent the normal passes
  // from using this pass as their manager.
  PassManagerType getPassManagerType() const {
    return PMT_BasicBlockPassManager;
  }
};
}
//===----------------------------------------------------------------------===//
char DelayInfoStorage::ID = 0;
INITIALIZE_PASS(DelayInfoStorage, "shang-delay-information-storage",
                "The Storage to maintian the delay information across"
                " scheduling iterations",
                false, true)

char DelayExtractor::ID = 0;
INITIALIZE_PASS_BEGIN(DelayExtractor, "shang-delay-information-extractor",
                      "Extract the delay information to cross-iteration storage",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(DelayInfoStorage)
INITIALIZE_PASS_END(DelayExtractor, "shang-delay-information-extractor",
                    "Extract the delay information to cross-iteration storage",
                    false, true)

char IterativeScheduling::ID = 0;

INITIALIZE_PASS_BEGIN(IterativeScheduling, "shang-iterative-scheduling",
                      "Preform iterative scheduling",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(DelayInfoStorage)
  INITIALIZE_PASS_DEPENDENCY(DelayExtractor)
  INITIALIZE_PASS_DEPENDENCY(LoopInfo)
  INITIALIZE_PASS_DEPENDENCY(BranchProbabilityInfo)
INITIALIZE_PASS_END(IterativeScheduling, "shang-iterative-scheduling",
                    "Preform iterative scheduling",
                    false, true)

Pass *llvm::createIterativeSchedulingPass() {
  return new IterativeScheduling();
}

void IterativeScheduling::schedulePass(Pass *P) {
  // If P is an analysis pass and it is available then do not
  // generate the analysis again. Stale analysis info should not be
  // available at this point.
  const PassInfo *PI =
    PassRegistry::getPassRegistry()->getPassInfo(P->getPassID());
  if (PI && PI->isAnalysis() && findAnalysisPass(P->getPassID(), true)) {
    delete P;
    return;
  }

  AnalysisUsage *AnUsage = TPM->findAnalysisUsage(P);
  PassManagerType CurPMT = P->getPotentialPassManagerType();

  bool checkAnalysis = true;
  while (checkAnalysis) {
    checkAnalysis = false;

    const AnalysisUsage::VectorType &RequiredSet = AnUsage->getRequiredSet();
    typedef AnalysisUsage::VectorType::const_iterator iterator;
    for (iterator I = RequiredSet.begin(), E = RequiredSet.end(); I != E; ++I) {
      Pass *AnalysisPass = findAnalysisPass(*I, true);
      if (AnalysisPass) continue;

      const PassInfo *PI = PassRegistry::getPassRegistry()->getPassInfo(*I);

      assert(PI && "Expected required passes to be initialized");
      AnalysisPass = PI->createPass();
      PassManagerType AnalysisPMT = AnalysisPass->getPotentialPassManagerType();

      if (CurPMT == AnalysisPMT)
        // Schedule analysis pass that is managed by the same pass manager.
        schedulePass(AnalysisPass);
      else if (CurPMT > AnalysisPMT) {
        // Schedule analysis pass that is managed by a new manager.
        schedulePass(AnalysisPass);
        // Recheck analysis passes to ensure that required analyses that
        // are already checked are still available.
        checkAnalysis = true;
      }
      else
        // Do not schedule this analysis. Lower level analsyis
        // passes are run on the fly.
        delete AnalysisPass;
    }
  }

  // Now all required passes are available.
  if (ImmutablePass *IP = P->getAsImmutablePass()) {
    // P is a immutable pass and it will be managed by this
    // top level manager. Set up analysis resolver to connect them.
    PMDataManager &ParentDM = getResolver()->getPMDataManager();
    AnalysisResolver *AR = new AnalysisResolver(ParentDM);
    P->setResolver(AR);
    initializeAnalysisImpl(P);
    TPM->addImmutablePass(IP);
    recordAvailableAnalysis(IP);
    return;
  }

  // Add the requested pass to the current manager.
  add(P);
}

void IterativeScheduling::changeOutputPaths(unsigned i) {
  SmallString<256> NewPath = sys::path::parent_path(OriginalRTLOutput);
  sys::path::append(NewPath, utostr_32(i));
  bool Existed;
  sys::fs::create_directories(StringRef(NewPath), Existed);
  (void)Existed;
  sys::path::append(NewPath, RTLFileName);

  std::string Script;
  raw_string_ostream SS(Script);
  SS << "RTLOutput = [[" << NewPath << "]]\n";
  sys::path::replace_extension(NewPath, ".sql");
  SS << "MCPDataBase = [[" << NewPath << "]]\n";

  SMDiagnostic Err;
  if (!runScriptStr(SS.str(), Err))
    report_fatal_error("Cannot set output paths for intermediate netlist!");
}

void IterativeScheduling::recoverOutputPath() {
  std::string Script;
  raw_string_ostream SS(Script);
  SS << "InputFile = [[" << OriginalRTLOutput << "]]\n"
        "MCPDataBase = [[" << OriginalMCPOutput << "]]\n";

  SMDiagnostic Err;
  if (!runScriptStr(SS.str(), Err))
    report_fatal_error("Cannot recover output paths!");
}
