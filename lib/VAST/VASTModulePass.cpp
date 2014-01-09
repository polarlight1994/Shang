//===-- VASTModulePass.cpp - Build the VASTModule on LLVM IR --------------===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implement the vast/VASTModulePass pass, which is the container of the
// VASTModule.
//
//===----------------------------------------------------------------------===//
#include "IR2Datapath.h"
#include "MinimalDatapathContext.h"
#include "Allocation.h"

#include "vast/LuaI.h"
#include "vast/Dataflow.h"
#include "vast/VASTMemoryBank.h"
#include "vast/VASTModulePass.h"
#include "vast/VASTModule.h"
#include "vast/VASTSubModules.h"

#include "vast/Passes.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vast-module-analysis"
#include "llvm/Support/Debug.h"

#include <map>

using namespace llvm;
STATISTIC(NumIPs, "Number of IPs Instantiated");
STATISTIC(NUMCombROM, "Number of Combinational ROM Generated");

namespace {
struct VASTModuleBuilder : public MinimalDatapathContext,
                           public InstVisitor<VASTModuleBuilder, void> {
  DatapathBuilder Builder;
  VASTCtrlRgn &R;
  HLSAllocation &A;
  Dataflow &DF;
  ScalarEvolution &SE;

  //===--------------------------------------------------------------------===//
  void buildInterface(Function *F);

  //===--------------------------------------------------------------------===//
  StringMap<VASTSubModule*> SubModules;
  VASTSubModule *getSubModule(StringRef Name) const {
    VASTSubModule *SubMod = SubModules.lookup(Name);
    assert(SubMod && "Submodule not allocated!");
    return SubMod;
  }

  VASTSubModule *getOrCreateSubModuleFromBinOp(BinaryOperator &BinOp);

  //===--------------------------------------------------------------------===//
  VASTSeqValue *getOrCreateSeqValImpl(Value *V, const Twine &Name);
  VASTSeqValue *getOrCreateSeqVal(Value *V, const Twine &Name) {
    std::string SeqValName = "v_" + Name.str() + "_r";
    SeqValName = VASTNamedValue::Mangle(SeqValName);
    return getOrCreateSeqValImpl(V, SeqValName);
  }

  static inline void intToStr(intptr_t V, SmallString<36> &S) {
    static char encoding_table[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                                    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
                                    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
                                    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
                                    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
                                    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
                                    'w', 'x', 'y', 'z', '0', '1', '2', '3',
                                    '4', '5', '6', '7', '8', '9'}; //, '+', '/'};
    const unsigned table_size = array_lengthof(encoding_table);

    assert(V && "Cannot convert 0 yet!");
    while (V) {
      unsigned char Digit = V % table_size;
      S += encoding_table[Digit];
      V /= table_size;
    }
  }

  static StringRef translatePtr2Str(void *V, SmallString<36> &S) {
    S.push_back('_');
    intToStr(intptr_t(V), S);
    S.push_back('_');
    return S.str();
  }

  VASTSeqValue *getOrCreateSeqVal(Value *V) {
    SmallString<36> S;
    std::string Name = "vast_" + VASTNamedValue::Mangle(V->getName()) + "_r";
    return getOrCreateSeqValImpl(V, Name);
  }

  VASTValPtr getAsOperandImpl(Value *Op);

  VASTRegister *createLoadRegister(Instruction &I, unsigned BitWidth) {
    SmallString<36> S;
    S += "vast_";
    S += VASTNamedValue::Mangle(I.getName());
    S += "_unshifted_r";
    return A->createRegister(S.str(), BitWidth);
  }

  // Remember the landing slot and the latest slot of a basic block.
  std::map<BasicBlock*, std::pair<VASTSlot*, VASTSlot*> > BB2SlotMap;
  unsigned NumSlots;
  VASTSlot *getOrCreateLandingSlot(BasicBlock *BB) {
    std::pair<VASTSlot*, VASTSlot*> &Slots = BB2SlotMap[BB];

    if (Slots.first == 0) {
      assert(Slots.second == 0 && "Unexpected Latest slot without landing slot!");
      Slots.first
        = (Slots.second = R.createSlot(++NumSlots, BB, 0));
    }

    return Slots.first;
  }

  VASTSlot *getLatestSlot(BasicBlock *BB) const {
    std::map<BasicBlock*, std::pair<VASTSlot*, VASTSlot*> >::const_iterator at
      = BB2SlotMap.find(BB);
    assert(at != BB2SlotMap.end() && "Latest slot not found!");
    return at->second.second;
  }

  // DIRTYHACK: Allocate enough slots for the read operation.
  VASTSlot *advanceToNextSlot(VASTSlot *CurSlot) {
    BasicBlock *BB = CurSlot->getParent();
    VASTSlot *&Slot = BB2SlotMap[BB].second;
    assert(Slot == CurSlot && "CurSlot not the last slot in the BB!");
    assert(CurSlot->succ_empty() && "CurSlot already have successors!");
    Slot = R.createSlot(++NumSlots, BB, 0);
    // Connect the slots.
    addSuccSlot(CurSlot, Slot);
    return Slot;
  }

  VASTSlot *advanceToNextSlot(VASTSlot *CurSlot, unsigned NumSlots) {
    VASTSlot *S = CurSlot;
    for (unsigned i = 0; i < NumSlots; ++i)
      S = advanceToNextSlot(S);

    return S;
  }

  VASTSlot *createSubGroup(BasicBlock *BB, VASTValPtr Cnd, VASTSlot *S) {
    VASTSlot *SubGrp = R.createSlot(++NumSlots, BB, S->Schedule, Cnd, true);
    // The subgroups are not actually the successors of S in the control flow.
    S->addSuccSlot(SubGrp, VASTSlot::SubGrp);
    return SubGrp;
  }

  void addSuccSlot(VASTSlot *S, VASTSlot *NextSlot,
                   VASTValPtr Cnd = VASTConstant::True,
                   TerminatorInst *Inst = 0) {
    // If the Br is already exist, simply or the conditions together.
    assert(!S->hasNextSlot(NextSlot) && "Edge had already existed!");
    assert((S->getParent() == NextSlot->getParent()
            || NextSlot->getParent() == NULL)
          && "Cannot change Slot and BB at the same time!");
    assert(!NextSlot->IsSubGrp && "Unexpected subgroup!");
    S->addSuccSlot(NextSlot, VASTSlot::Sucessor);
    VASTSlotCtrl *SlotBr = R.createStateTransition(NextSlot, S, Cnd);
    if (Inst) SlotBr->annotateValue(Inst);
  }

  void buildConditionalTransition(BasicBlock *DstBB, VASTSlot *CurSlot,
                                  VASTValPtr Cnd, TerminatorInst &I);

  //===--------------------------------------------------------------------===//
  void visitBasicBlock(BasicBlock *BB);
  void visitPHIsInSucc(VASTSlot *S, VASTValPtr Cnd, BasicBlock *CurBB);

  VASTValPtr indexVASTExpr(Value *Val, VASTValPtr V);

  // Build the SeqOps from the LLVM Instruction.
  void visitReturnInst(ReturnInst &I);
  void visitBranchInst(BranchInst &I);

  void visitSwitchInst(SwitchInst &I);
  void visitUnreachableInst(UnreachableInst &I);

  void visitCallSite(CallSite CS);
  void visitIntrinsicInst(IntrinsicInst &I);

  void visitBinaryOperator(BinaryOperator &I);
  void visitICmpInst(ICmpInst &I);
  void visitGetElementPtrInst(GetElementPtrInst &I);

  void visitLoadInst(LoadInst &I);
  void visitStoreInst(StoreInst &I);

  void visitInstruction(Instruction &I) {
    // Try to build a combinational node for I.
    if (VASTValPtr V = Builder.visit(I))
      indexVASTExpr(&I, V);
  }

  //===--------------------------------------------------------------------===//
  unsigned getByteEnable(Value *Addr) const;
  VASTValPtr alignLoadResult(VASTSeqValue *Result, VASTValPtr ByteOffset,
                             VASTMemoryBank *Bus);
  void buildMemoryTransaction(Value *Addr, Value *Data, VASTMemoryBank *Bus,
                              Instruction &Inst);
  void buildCombinationalROMLookup(Value *Addr, VASTMemoryBank *Bus,
                                   Instruction &Inst);

  void buildSubModuleOperation(VASTSeqInst *Inst, VASTSubModule *SubMod,
                               ArrayRef<VASTValPtr> Args);
  //===--------------------------------------------------------------------===//
  VASTModuleBuilder(VASTCtrlRgn &R, HLSAllocation &Allocation, DataLayout &TD,
                    Dataflow &DF, ScalarEvolution &SE)
    : MinimalDatapathContext(Allocation.getModule(), &TD), Builder(*this),
      R(R), A(Allocation), DF(DF), SE(SE), NumSlots(0)  {}
};
}

//===----------------------------------------------------------------------===//
VASTSeqValue *VASTModuleBuilder::getOrCreateSeqValImpl(Value *V,
                                                       const Twine &Name) {
  assert(!V->getType()->isVoidTy() && "Cannot create SeqVal for Inst!");
  VASTValPtr Val = Builder.lookupExpr(V);

  if (Val) {
    assert(!Val.isInverted() && "Bad value type!");
    if (VASTExpr *Expr = dyn_cast<VASTExpr>(Val)) {
      // The only case that Val is not a SeqVal is that the SeqVal is replaced
      // by some known bits.
      assert(Expr->getOpcode() == VASTExpr::dpBitCat && "Unexpected expr type!");
      std::set<VASTSeqValue*> SeqVals;
      Expr->extractCombConeLeaves(SeqVals);
      assert(SeqVals.size() == 1 && "Bad supporting seqvalue!");
      VASTSeqValue *SV = *SeqVals.begin();
      assert(SV->getLLVMValue() == V && "LLVM Value not match!");
      return SV;
    }

    return cast<VASTSeqValue>(Val.get());
  }

  // Create the SeqVal now.
  unsigned BitWidth = Builder.getValueSizeInBits(V);
  VASTRegister *R = A->createRegister(Name, BitWidth, 0);
  VASTSeqValue *SeqVal = A->createSeqValue(R->getSelector(), 0, V);

  // Index the value.
  indexVASTExpr(V, SeqVal);
  return SeqVal;
}


VASTValPtr VASTModuleBuilder::getAsOperandImpl(Value *V) {
  if (VASTValPtr Val = lookupExpr(V)) return Val;

  if (Instruction *Inst = dyn_cast<Instruction>(V)) {
    // The VASTValPtr of the instruction should had been created when we trying
    // to get it as operand, unless the basic block is unreachable.
    assert(DF.isBlockUnreachable(Inst->getParent())
           && "The VASTValPtr for Instruction not found!");
    return getConstant(0, getValueSizeInBits(Inst));
  }

  if (ConstantInt *Int = dyn_cast<ConstantInt>(V))
    return indexVASTExpr(V, getConstant(Int->getValue()));

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(V)) {
    unsigned SizeInBits = getValueSizeInBits(GV);

    VASTMemoryBank *Bus = A.getMemoryBank(*GV);
    const std::string Name = VASTNamedValue::Mangle(GV->getName());
    if (!Bus->isDefault()) {
      unsigned StartOffset = Bus->getStartOffset(GV);
      VASTConstant *C = getConstant(StartOffset, SizeInBits);
      // FIXME: Annotate the GV to the Constant.
      return indexVASTExpr(GV, C);
    }
    
    // If the GV is assigned to the memory port 0, create a wrapper wire for it.
    return indexVASTExpr(GV, A->getOrCreateWrapper(Name, SizeInBits, GV));
  }

  if (GEPOperator *GEP = dyn_cast<GEPOperator>(V))
    return indexVASTExpr(V, Builder.visitGEPOperator(*GEP));

  // Try to build the datapath for the constant expression.
  if (ConstantExpr *CExpr = dyn_cast<ConstantExpr>(V)) {
    switch (CExpr->getOpcode()) {
    default:break;
    case Instruction::GetElementPtr:
      return indexVASTExpr(V, Builder.visitGEPOperator(*cast<GEPOperator>(CExpr)));
    case Instruction::IntToPtr: {
      VASTValPtr Operand = getAsOperandImpl(CExpr->getOperand(0));
      unsigned SizeInBits = getValueSizeInBits(V);
      return indexVASTExpr(V, Builder.buildZExtExprOrSelf(Operand, SizeInBits));
    }
    case Instruction::PtrToInt: {
      VASTValPtr Operand = getAsOperandImpl(CExpr->getOperand(0));
      unsigned SizeInBits = getValueSizeInBits(V);
      return indexVASTExpr(V, Builder.buildBitExtractExpr(Operand, SizeInBits, 0));
    }
    case Instruction::BitCast: {
      VASTValPtr Operand = getAsOperandImpl(CExpr->getOperand(0));
      assert(getValueSizeInBits(V) == Operand->getBitWidth()
             && "Cast between types with different size found!");
      return indexVASTExpr(V, Operand);
    }
    case Instruction::ZExt: {
      VASTValPtr Operand = getAsOperandImpl(CExpr->getOperand(0));
      unsigned SizeInBits = getValueSizeInBits(V);
      return indexVASTExpr(V, Builder.buildZExtExpr(Operand, SizeInBits));
    }
    }
  }

  if (UndefValue *UDef = dyn_cast<UndefValue>(V)) {
    unsigned SizeInBits = getValueSizeInBits(UDef);
    SmallString<36> S;
    return indexVASTExpr(V, A->getOrCreateWrapper(translatePtr2Str(UDef, S),
                                                  SizeInBits, UDef));
  }

  if (ConstantPointerNull *PtrNull = dyn_cast<ConstantPointerNull>(V)) {
    unsigned SizeInBit = getValueSizeInBits(PtrNull);
    return indexVASTExpr(V, getConstant(APInt::getNullValue(SizeInBit)));
  }

  llvm_unreachable("Unhandle value!");
}

//===----------------------------------------------------------------------===//
void VASTModuleBuilder::buildInterface(Function *F) {
  SmallVector<VASTSeqValue*, 4> ArgRegs;
  SmallVector<VASTValPtr, 4> ArgPorts;
  SmallVector<Value*, 4> Args;

  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E; ++I)
  {
    Argument *Arg = I;
    std::string Name = Arg->getName();
    unsigned BitWidth = TD->getTypeSizeInBits(Arg->getType());

    VASTValPtr V
      = A->addInputPort(Name, BitWidth, VASTModule::ArgPort)->getValue();
    // Remember the expression for the argument input.
    VASTSeqValue *SeqVal = getOrCreateSeqVal(Arg, Name);
    ArgRegs.push_back(SeqVal);
    ArgPorts.push_back(V);
    Args.push_back(Arg);
  }

  Type *RetTy = F->getReturnType();
  if (!RetTy->isVoidTy()) {
    assert(RetTy->isIntegerTy() && "Only support return integer now!");
    unsigned BitWidth = TD->getTypeSizeInBits(RetTy);
    A->addOutputPort("return_value", BitWidth, VASTModule::RetPort);
  }

  VASTSlot *IdleSlot = R.getStartSlot();

  // Create the virtual slot representing the idle loop.
  VASTValue *StartPort
    = cast<VASTInPort>(A->getPort(VASTModule::Start)).getValue();
  VASTSlot *IdleSlotGrp
    = createSubGroup(0, Builder.buildNotExpr(StartPort), IdleSlot);
  addSuccSlot(IdleSlotGrp, IdleSlot, Builder.buildNotExpr(StartPort));

  // Create the virtual slot represent the entry of the CFG.
  BasicBlock *EntryBB = &F->getEntryBlock();
  VASTSlot *EntryGrp = createSubGroup(EntryBB, StartPort, IdleSlot);

  // Connect the launch slot to the landing slot, with a real edge (which
  // represent a state transition)
  addSuccSlot(EntryGrp, getOrCreateLandingSlot(EntryBB), StartPort);

  // Copy the value to the register.
  for (unsigned i = 0, e = ArgRegs.size(); i != e; ++i)
    R.latchValue(ArgRegs[i], ArgPorts[i], EntryGrp, StartPort, Args[i]);

  // Also reset the finish signal.
  VASTSeqInst *ResetFin
    = R.lauchInst(EntryGrp, StartPort, 1, UndefValue::get(RetTy), true);
  VASTSelector *FinPort
    = cast<VASTOutPort>(A->getPort(VASTModule::Finish)).getSelector();
  ResetFin->addSrc(VASTConstant::False, 0, FinPort);
}

//===----------------------------------------------------------------------===//
void VASTModuleBuilder::visitBasicBlock(BasicBlock *BB) {
  // Create the landing slot for this BB.
  VASTSlot *S = getOrCreateLandingSlot(BB);

  if (DF.isBlockUnreachable(BB)) {
    TerminatorInst *Inst = BB->getTerminator();
    if (ReturnInst *Ret = dyn_cast<ReturnInst>(Inst)) {
      visitReturnInst(*Ret);
      return;
    }

    for (succ_iterator I = succ_begin(BB), E = succ_end(BB); I != E; ++I) {
      BasicBlock *Succ = *I;
      buildConditionalTransition(Succ, S, VASTConstant::False, *Inst);
    }

    return;
  }

  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
    // PHINodes will be handled in somewhere else.
    if (isa<PHINode>(I)) continue;

    // Otherwise build the SeqOp for this operation.
    visit(I);
  }
}

VASTValPtr VASTModuleBuilder::indexVASTExpr(Value *Val, VASTValPtr V) {
  if (VASTMaskedValue *MaskedValue = dyn_cast<VASTMaskedValue>(V.get()))
    MaskedValue->mergeAnyKnown(Val, SE, *TD, V.isInverted());

  // Replace the known bits before we index the expressions.
  return DatapathBuilderContext::indexVASTExpr(Val, V);
}

void VASTModuleBuilder::visitPHIsInSucc(VASTSlot *S, VASTValPtr Cnd,
                                        BasicBlock *CurBB) {
  BasicBlock *BB = S->getParent();
  assert(BB && "Unexpected null BB!");

  typedef BasicBlock::iterator iterator;
  for (iterator I = BB->begin(), E = BB->getFirstNonPHI(); I != E; ++I) {
    PHINode *PN = cast<PHINode>(I);
    unsigned BitWidth = getValueSizeInBits(PN);

    Value *LiveOutedFromBB = PN->DoPHITranslation(BB, CurBB);
    VASTValPtr LiveOut = DF.isBlockUnreachable(CurBB) ?
                         getConstant(0, BitWidth) :
                         getAsOperandImpl(LiveOutedFromBB);

    VASTSeqValue *PHISeqVal = getOrCreateSeqVal(PN);
    // Latch the incoming value when we are branching to the succ slot.
    R.latchValue(PHISeqVal, LiveOut, S,  Cnd, PN);
  }
}


void VASTModuleBuilder::buildConditionalTransition(BasicBlock *DstBB,
                                                   VASTSlot *CurSlot,
                                                   VASTValPtr Cnd,
                                                   TerminatorInst &I) {
  // Create the virtual slot represent the launch of the design.
  VASTSlot *SubGrp = createSubGroup(DstBB, Cnd, CurSlot);
  // Build the branch operation before building the PHIs, make sure the PHIs
  // are placed after the branch operation targeting the same BB with PHIs.
  addSuccSlot(SubGrp, getOrCreateLandingSlot(DstBB), Cnd, &I);
  visitPHIsInSucc(SubGrp, Cnd, CurSlot->getParent());
}

void VASTModuleBuilder::visitReturnInst(ReturnInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());

  // Create the virtual slot represent the launch of the design.
  VASTSlot *SubGrp = createSubGroup(NULL, VASTConstant::True, CurSlot);
  // Jump back to the start slot on return.
  addSuccSlot(SubGrp, R.getStartSlot(), VASTConstant::True, &I);

  unsigned NumOperands = I.getNumOperands();
  VASTSeqInst *SeqInst =
    R.lauchInst(SubGrp, VASTConstant::True, NumOperands + 1, &I, true);

  // Assign the return port if necessary.
  if (NumOperands) {
    VASTSelector *RetPort = cast<VASTOutPort>(A->getRetPort()).getSelector();
    // Please note that we do not need to export the definition of the value
    // on the return port.
    SeqInst->addSrc(getAsOperandImpl(I.getReturnValue()), 0, RetPort);
  }

  // Enable the finish port.
  VASTSelector *FinPort
    = cast<VASTOutPort>(A->getPort(VASTModule::Finish)).getSelector();
  SeqInst->addSrc(VASTConstant::True, NumOperands, FinPort);
}

void VASTModuleBuilder::visitUnreachableInst(UnreachableInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());

  // Create the virtual slot represent the launch of the design.
  VASTSlot *SubGrp = createSubGroup(NULL, VASTConstant::True, CurSlot);
  // DIRTYHACK: Simply jump back the start slot.
  // Construct the control flow.
  addSuccSlot(SubGrp, R.getStartSlot(), VASTConstant::True, &I);
}

void VASTModuleBuilder::visitBranchInst(BranchInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());
  // TODO: Create alias operations.
  if (I.isUnconditional()) {
    BasicBlock *DstBB = I.getSuccessor(0);

    buildConditionalTransition(DstBB, CurSlot, VASTConstant::True, I);
    return;
  }

  // Connect the slots according to the condition.
  VASTValPtr Cnd = getAsOperandImpl(I.getCondition());
  BasicBlock *TrueBB = I.getSuccessor(0);

  buildConditionalTransition(TrueBB, CurSlot, Cnd, I);

  BasicBlock *FalseBB = I.getSuccessor(1);
  buildConditionalTransition(FalseBB, CurSlot, Builder.buildNotExpr(Cnd), I);
}

// Copy from LowerSwitch.cpp.
namespace {
struct CaseRange {
  APInt Low;
  APInt High;
  BasicBlock* BB;

  CaseRange(APInt low = APInt(), APInt high = APInt(), BasicBlock *bb = 0) :
    Low(low), High(high), BB(bb) { }

};

typedef std::vector<CaseRange>           CaseVector;
typedef std::vector<CaseRange>::iterator CaseItr;



/// The comparison function for sorting the switch case values in the vector.
/// WARNING: Case ranges should be disjoint!
struct CaseCmp {
  bool operator () (const CaseRange& C1,
                    const CaseRange& C2) {
    return C1.Low.slt(C2.High);
  }
};
}

// Clusterify - Transform simple list of Cases into list of CaseRange's
static unsigned Clusterify(CaseVector& Cases, SwitchInst *SI) {
  unsigned numCmps = 0;

  // Start with "simple" cases
  for (SwitchInst::CaseIt i = SI->case_begin(), e = SI->case_end(); i != e; ++i)
    Cases.push_back(CaseRange(i.getCaseValue()->getValue(),
                              i.getCaseValue()->getValue(),
                              i.getCaseSuccessor()));

  std::sort(Cases.begin(), Cases.end(), CaseCmp());

  // Merge case into clusters
  if (Cases.size()>=2) {
    for (CaseItr I=Cases.begin(), J=llvm::next(Cases.begin()); J!=Cases.end(); ) {
      int64_t nextValue = J->Low.getSExtValue();
      int64_t currentValue = I->High.getSExtValue();
      BasicBlock* nextBB = J->BB;
      BasicBlock* currentBB = I->BB;

      // If the two neighboring cases go to the same destination, merge them
      // into a single case.
      if ((nextValue-currentValue==1) && (currentBB == nextBB)) {
        I->High = J->High;
        J = Cases.erase(J);
      } else {
        I = J++;
      }
    }
  }

  for (CaseItr I=Cases.begin(), E=Cases.end(); I!=E; ++I, ++numCmps) {
    if (I->Low != I->High)
      // A range counts double, since it requires two compares.
      ++numCmps;
  }

  return numCmps;
}

void VASTModuleBuilder::visitSwitchInst(SwitchInst &I) {
  VASTSlot *CurSlot = getLatestSlot(I.getParent());
  VASTValPtr CndVal = getAsOperandImpl(I.getCondition());

  std::map<BasicBlock*, VASTValPtr> CaseMap;

  // Prepare cases vector.
  CaseVector Cases;
  Clusterify(Cases, &I);
  // Build the condition map.
  for (CaseItr CI = Cases.begin(), CE = Cases.end(); CI != CE; ++CI) {
    const CaseRange &Case = *CI;
    // Simple case, test if the CndVal is equal to a specific value.
    if (Case.High == Case.Low) {
      VASTValPtr CaseVal = getConstant(Case.High);
      VASTValPtr Pred = Builder.buildEQ(CndVal, CaseVal);
      VASTValPtr &BBPred = CaseMap[Case.BB];
      if (!BBPred) BBPred = Pred;
      else         Builder.orEqual(BBPred, Pred);

      continue;
    }

    // Test if Low <= CndVal <= High
    VASTValPtr Low = Builder.getConstant(Case.Low);
    VASTValPtr LowCmp = Builder.buildICmpOrEqExpr(VASTExpr::dpUGT, CndVal, Low);
    VASTValPtr High = Builder.getConstant(Case.High);
    VASTValPtr HighCmp = Builder.buildICmpOrEqExpr(VASTExpr::dpUGT, High, CndVal);
    VASTValPtr Pred = Builder.buildAndExpr(LowCmp, HighCmp, 1);
    VASTValPtr &BBPred = CaseMap[Case.BB];
    if (!BBPred) BBPred = Pred;
    else         Builder.orEqual(BBPred, Pred);
  }

  // The predicate for each non-default destination.
  SmallVector<VASTValPtr, 4> CasePreds;
  typedef std::map<BasicBlock*, VASTValPtr>::iterator CaseIt;
  for (CaseIt CI = CaseMap.begin(), CE = CaseMap.end(); CI != CE; ++CI) {
    BasicBlock *SuccBB = CI->first;
    VASTValPtr Pred = CI->second;
    CasePreds.push_back(Pred);

    buildConditionalTransition(SuccBB, CurSlot, Pred, I);
  }

  // Jump to the default block when all the case value not match, i.e. all case
  // predicate is false.
  VASTValPtr DefaultPred = Builder.buildNotExpr(Builder.buildOrExpr(CasePreds, 1));
  BasicBlock *DefBB = I.getDefaultDest();

  buildConditionalTransition(DefBB, CurSlot, DefaultPred, I);
}

void VASTModuleBuilder::visitCallSite(CallSite CS) {
  Function *Callee = CS.getCalledFunction();
  // Ignore the external function.
  if (Callee->isDeclaration()) return;

  assert(!CS.isInvoke() && "Cannot handle invoke at this moment!");
  CallInst *Inst = cast<CallInst>(CS.getInstruction());

  VASTSubModule *SubMod = getSubModule(Callee->getName());
  assert(SubMod && "Submodule not allocated?");
  unsigned NumArgs = CS.arg_size();

  SmallVector<VASTValPtr, 4> Args;
  for (unsigned i = 0; i < NumArgs; ++i)
    Args.push_back(getAsOperandImpl(CS.getArgument(i)));

  BasicBlock *ParentBB = CS->getParent();
  VASTSlot *Slot = getLatestSlot(ParentBB);
  VASTSeqInst *Op
    = R.lauchInst(Slot, VASTConstant::True, Args.size() + 1, Inst, false);
  // Build the logic to lauch the module and read the result.
  buildSubModuleOperation(Op, SubMod, Args);
}

void VASTModuleBuilder::visitGetElementPtrInst(GetElementPtrInst &I) {
  indexVASTExpr(&I, Builder.visit(I));
}

void VASTModuleBuilder::visitICmpInst(ICmpInst &I) {
  indexVASTExpr(&I, Builder.visit(I));
}

void VASTModuleBuilder::visitBinaryOperator(BinaryOperator &I) {
  // Try to build the combinational node.
  if (VASTValPtr V = Builder.visit(I)) {
    indexVASTExpr(&I, V);
    return;
  }

  // Otherwise we need to intanstiate a submodule.
  VASTSubModule *SubMod = getOrCreateSubModuleFromBinOp(I);

  if (SubMod == 0) {
    errs() << "Warning: Cannot generate IP to implement instruction:\n";
    I.print(errs());
    return;
  }

  VASTValPtr Ops[] = { getAsOperandImpl(I.getOperand(0)),
                       getAsOperandImpl(I.getOperand(1)) };

  BasicBlock *ParentBB = I.getParent();
  VASTSlot *Slot = getLatestSlot(ParentBB);
  VASTSeqInst *Op
    = R.lauchInst(Slot, VASTConstant::True, I.getNumOperands(), &I, false);
  buildSubModuleOperation(Op, SubMod, Ops);
}

VASTSubModule *
VASTModuleBuilder::getOrCreateSubModuleFromBinOp(BinaryOperator &BinOp) {
  unsigned ResultSize = getValueSizeInBits(BinOp);
  std::string SizeStr = utostr(ResultSize);
  std::string SubModuleName = BinOp.getOpcodeName() + SizeStr;

  VASTSubModule *&Mod = SubModules[SubModuleName];

  // Create the SubModuleNow if it didn't exist yet.
  if (Mod == 0) {
    unsigned FNNum = SubModules.size();
    Mod = A->addSubmodule(SubModuleName.c_str(), FNNum);
    // Build up the operand and outputs.
    VASTRegister *LHS = A->createRegister(SubModuleName + "lhs", ResultSize);
    Mod->addFanin(LHS->getSelector());
    VASTRegister *RHS = A->createRegister(SubModuleName + "rhs", ResultSize);
    Mod->addFanin(RHS->getSelector());

    // Look up the functional unit latency from the scripting engine.
    const char *FUDelayPath[] = { "FUs", BinOp.getOpcodeName(), "Latencies",
                                  SizeStr.c_str() };
    float Latency = LuaI::GetFloat(FUDelayPath);
    Mod->createRetPort(&A.getModule(), ResultSize, ceil(Latency));

    ++NumIPs;
  }

  Mod->addInstuction(&BinOp);
  return Mod;
}

void VASTModuleBuilder::buildSubModuleOperation(VASTSeqInst *Inst,
                                                VASTSubModule *SubMod,
                                                ArrayRef<VASTValPtr> Args) {
  for (unsigned i = 0; i < Args.size(); ++i)
    Inst->addSrc(Args[i], i, SubMod->getFanin(i));

  Value *V = Inst->getValue();
  VASTSlot *Slot = Inst->getSlot();
  unsigned Latency = std::max(1u, SubMod->getLatency());
  Slot = advanceToNextSlot(Slot, Latency);

  // Read the return value from the function if there is any.
  if (VASTSelector *RetPort = SubMod->getRetPort()) {
    VASTSeqValue *TimedReturn = A->createSeqValue(RetPort, 0, V);
    VASTSeqValue *Result = getOrCreateSeqVal(Inst->getValue());
    R.latchValue(Result, TimedReturn, Slot, VASTConstant::True, V, Latency);
    // Move the the next slot so that the operation can correctly read the
    // returned value
    advanceToNextSlot(Slot);
  }
}

//===----------------------------------------------------------------------===//

void VASTModuleBuilder::visitIntrinsicInst(IntrinsicInst &I) {
  // Try to build a combinational node for I.
  if (VASTValPtr V = Builder.visit(I)) {
    indexVASTExpr(&I, V);
    return;
  }

  I.dump();
}

void VASTModuleBuilder::visitLoadInst(LoadInst &I) {
  VASTMemoryBank *Bus = A.getMemoryBank(I);
  if (Bus->isCombinationalROM()) {
    buildCombinationalROMLookup(I.getPointerOperand(), Bus, I);
    return;
  }

  buildMemoryTransaction(I.getPointerOperand(), 0, Bus, I);
}

void VASTModuleBuilder::visitStoreInst(StoreInst &I) {
  VASTMemoryBank *Bus = A.getMemoryBank(I);
  assert(!Bus->isCombinationalROM() && "Cannot store to a combinational ROM!");
  buildMemoryTransaction(I.getPointerOperand(), I.getValueOperand(), Bus, I);
}

//===----------------------------------------------------------------------===//
// Memory transaction code building functions.
static unsigned GetByteEnable(unsigned SizeInBytes) {
  return (0x1 << SizeInBytes) - 1;
}

unsigned VASTModuleBuilder::getByteEnable(Value *Addr) const {
  PointerType *AddrTy = cast<PointerType>(Addr->getType());
  Type *DataTy = AddrTy->getElementType();
  return GetByteEnable(TD->getTypeStoreSize(DataTy));
}

VASTValPtr
VASTModuleBuilder::alignLoadResult(VASTSeqValue *Result, VASTValPtr ByteOffset,
                                   VASTMemoryBank *Bus) {
  VASTValPtr V = Builder.buildBitExtractExpr(Result, Bus->getDataWidth(), 0);

  // Build the shift to shift the bytes to LSB.
  if (Bus->requireByteEnable() && !Bus->isDefault()) {
    unsigned DataWidth = Bus->getDataWidth();
    unsigned ByteAddrWidth = Bus->getByteAddrWidth();
    // Use the known byte offset from address whenever possible.
    VASTValPtr ShiftAmt = ByteOffset;
    // If the byte offset is unknown in compile time, get the shift amount from
    // the higher part of the bus output.
    if (!isa<VASTConstant>(ByteOffset.get()))
      ShiftAmt = Builder.buildBitExtractExpr(Result, DataWidth + ByteAddrWidth,
                                             DataWidth);
    // Again, convert the shift amount in bytes to shift amount in bits.
    VASTValPtr ShiftAmtBits[] = { ShiftAmt, Builder.getConstant(0, 3) };
    ShiftAmt = Builder.buildBitCatExpr(ShiftAmtBits, ByteAddrWidth + 3);
    // Align the result.
    V = Builder.buildShiftExpr(VASTExpr::dpLshr, V, ShiftAmt, Bus->getDataWidth());
  }

  return V;
}

void
VASTModuleBuilder::buildMemoryTransaction(Value *Addr, Value *Data,
                                          VASTMemoryBank *Bus, Instruction &I) {
  BasicBlock *ParentBB = I.getParent();
  VASTSlot *Slot = getLatestSlot(ParentBB);

  // Build the logic to start the transaction.
  unsigned NumOperands = Data ? 2 : 1;
  // Double the operand number for default bus, because it requires the enable
  // (corresponding to the enable of the address) and the the write enable
  // (corresponding to the enable of the data).
  if (Bus->isDefault()) NumOperands *= 2;
  if (Bus->requireByteEnable()) NumOperands += 1;

  VASTSeqOp *Op
    = R.lauchInst(Slot, VASTConstant::True, NumOperands, &I, false);
  unsigned CurSrcIdx = 0;

  VASTValPtr AddrVal = getAsOperandImpl(Addr);
  // Clamp the address width, to the address width of the memory bank.
  // Please note that we are using the byte address in the memory banks, so
  // the lower bound of the bitslice is 0.
  AddrVal = Builder.buildBitExtractExpr(AddrVal, Bus->getAddrWidth(), 0);
  // Emit Address, use port 0.
  Op->addSrc(AddrVal, CurSrcIdx++, Bus->getAddr(0));

  VASTValPtr ByteOffset = None;

  if (Data) {
    // Assign store data, use port 0..
    VASTValPtr ValToStore = getAsOperandImpl(Data);
    assert(ValToStore->getBitWidth() <= Bus->getDataWidth()
           && "Storing data that exceed the width of databus!");
    ValToStore = Builder.buildZExtExprOrSelf(ValToStore, Bus->getDataWidth());
    if (Bus->requireByteEnable() && !Bus->isDefault()) {
      // We need to align the data based on its byte offset if byteenable
      // presents.
      unsigned ByteAddrWidth = Bus->getByteAddrWidth();
      ByteOffset = Builder.buildBitExtractExpr(AddrVal, ByteAddrWidth, 0);
      VASTValPtr ShiftAmt = ByteOffset;
      // Shift the data by Bytes requires the ShiftAmt multiplied by 8.
      VASTValPtr ShiftAmtBits[] = { ShiftAmt, Builder.getConstant(0, 3) };
      ShiftAmt = Builder.buildBitCatExpr(ShiftAmtBits, ByteAddrWidth + 3);
      ValToStore = Builder.buildShiftExpr(VASTExpr::dpShl, ValToStore, ShiftAmt,
                                          Bus->getDataWidth());
    }

    Op->addSrc(ValToStore, CurSrcIdx++, Bus->getWData(0));
    if (Bus->isDefault())
      Op->addSrc(VASTConstant::True, CurSrcIdx++, Bus->getWriteEnable());
  }

  // Explicitly enable the memory bus is required if the bus is external.
  if (Bus->isDefault())
    Op->addSrc(VASTConstant::True, CurSrcIdx++, Bus->getEnable());

  // Compute the byte enable, use port 0..
  if (Bus->requireByteEnable()) {
    VASTValPtr ByteEn
      = Builder.getConstant(getByteEnable(Addr), Bus->getByteEnWidth());
    if (!Bus->isDefault()) {
      // We also need to align the byte enables.
      unsigned ByteAddrWidth = Bus->getByteAddrWidth();
      ByteOffset = Builder.buildBitExtractExpr(AddrVal, ByteAddrWidth, 0);
      ByteEn = Builder.buildShiftExpr(VASTExpr::dpShl, ByteEn, ByteOffset,
                                      Bus->getByteEnWidth());
    }

    Op->addSrc(ByteEn, CurSrcIdx++, Bus->getByteEn(0));
  }

  // Read the result of the memory transaction.
  if (Data == NULL) {
    // The latency of the read operation is fixed to 1 if the byteenable is not
    // required.
    unsigned Latency = Bus->getReadLatency();
    // TODO: Enable each pipeline stage individually.
    // Please note that we had already advance 1 slot after we lauch the
    // load/store to disable the load/store. Now we need only wait Latency - 1
    // slots to get the result.
    Slot = advanceToNextSlot(Slot, Latency);

    // Use port 0 of the memory
    VASTValPtr TimedRData = A->createSeqValue(Bus->getRData(0), 0, &I);
    SmallString<36> S;
    VASTRegister *ResultRegister
      = createLoadRegister(I, TimedRData->getBitWidth());
    VASTSeqValue *Result = A->createSeqValue(ResultRegister->getSelector(), 0, &I);
    R.latchValue(Result, TimedRData, Slot, VASTConstant::True, &I, Latency);

    // Alignment is required if the Bus has byteenable.
    VASTValPtr V = alignLoadResult(Result, ByteOffset, Bus);
    // Trim the unused bits.
    V = Builder.buildBitExtractExpr(V, getValueSizeInBits(&I), 0);
    // Index the aligned and trimed result.
    indexVASTExpr(&I, V);
  }

  // Move the the next slot so that the other operations are not conflict with
  // the current memory operations.
  advanceToNextSlot(Slot);
}

void
VASTModuleBuilder::buildCombinationalROMLookup(Value *Addr, VASTMemoryBank *Bus,
                                               Instruction &Inst) {
  unsigned ResultWidth = getValueSizeInBits(Inst);
  VASTValPtr AddrVal = getAsOperandImpl(Addr);
  // Clamp the address width, to the address width of the memory bank.
  // Please note that we are using the byte address in the memory banks, so
  // the lower bound of the bitslice is 0.
  AddrVal = Builder.buildBitExtractExpr(AddrVal, Bus->getAddrWidth(), 0);

  indexVASTExpr(&Inst, Builder.buildROMLookUp(AddrVal, Bus, ResultWidth));
  ++NUMCombROM;
}

//===----------------------------------------------------------------------===//
namespace llvm {
void initializeVASTModuleAnalysisPass(PassRegistry &Registry);
}

namespace {
struct VASTModuleAnalysis : public FunctionPass {
  VASTModule *VM;

  static char ID;

  VASTModuleAnalysis() : FunctionPass(ID), VM(0) {
    initializeVASTModuleAnalysisPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F);
  void releaseMemory();
  void getAnalysisUsage(AnalysisUsage &AU) const;

  operator VASTModule*() const { return VM; }
  VASTModule *operator->() const { return VM; }
};
}

INITIALIZE_PASS_BEGIN(VASTModuleAnalysis,
                      "vast-module-builder", "VASTModule Builder",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_AG_DEPENDENCY(HLSAllocation)
  INITIALIZE_PASS_DEPENDENCY(Dataflow)
INITIALIZE_PASS_END(VASTModuleAnalysis,
                    "vast-module-builder", "VASTModule Builder",
                    false, true)

bool VASTModuleAnalysis::runOnFunction(Function &F) {
  assert(VM == 0 && "Module has been already created!");
  HLSAllocation &A = getAnalysis<HLSAllocation>();
  VM = &A.getModule();

  VASTModuleBuilder Builder(*VM, A,
                            getAnalysis<DataLayout>(),
                            getAnalysis<Dataflow>(),
                            getAnalysis<ScalarEvolution>());

  Builder.buildInterface(&F);

  // Visit the basic block in topological order.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I)
    Builder.visitBasicBlock(*I);

  // Release the dead objects generated during the VM construction.
  VM->gc();

  return false;
}

void VASTModuleAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DataLayout>();
  AU.addRequired<HLSAllocation>();
  AU.addRequired<Dataflow>();
  AU.addRequired<ScalarEvolution>();
  AU.setPreservesAll();
}

void VASTModuleAnalysis::releaseMemory() {}

char VASTModuleAnalysis::ID = 0;

//===----------------------------------------------------------------------===//
void VASTModulePass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequiredTransitive<HLSAllocation>();
  AU.addRequiredTransitive<VASTModuleAnalysis>();
  AU.addPreserved<VASTModuleAnalysis>();
  AU.addPreserved<AliasAnalysis>();
  AU.addPreserved<ScalarEvolution>();
  AU.addPreserved<HLSAllocation>();
  AU.addPreserved<DependenceAnalysis>();
  AU.addPreserved<BlockFrequencyInfo>();
  AU.addPreserved<BranchProbabilityInfo>();
  AU.addPreservedID(DataflowID);
  AU.setPreservesCFG();
}

bool VASTModulePass::runOnFunction(Function &F) {
  VASTModuleAnalysis &VMA = getAnalysis<VASTModuleAnalysis>();

  bool changed = runOnVASTModule(*VMA);

#ifndef NDEBUG
  VMA->verify();
#endif

  while (changed && VMA->gc())
    ;

  return changed;
}

VASTModule &VASTModulePass::rebuildModule() {
  // Get the old VASTModule
  VASTModuleAnalysis &VMA = getAnalysis<VASTModuleAnalysis>();
  // Preserve the bounding box constraint.
  unsigned BBX = VMA->getBBX(), BBY = VMA->getBBY(),
           BBWidth = VMA->getBBWidth(), BBHeight = VMA->getBBHeight();

  // And the corresponding LLVM Function, we will rebuild the VASTModule based
  // on the LLVM FUnction.
  Function &F = *VMA->getFunction();

  // Release and rebuild.
  VMA.releaseMemory();
  VMA.runOnFunction(F);

  VMA->setBoundingBoxConstraint(BBX, BBY, BBWidth, BBHeight);

  return *VMA;
}

void VASTModulePass::print(raw_ostream &OS) const {

}

// Initialize all passed required by a VASTModulePass.
VASTModulePass::VASTModulePass(char &ID) : FunctionPass(ID) {
  initializeVASTModuleAnalysisPass(*PassRegistry::getPassRegistry());
}
