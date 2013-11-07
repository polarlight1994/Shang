//===- AlwaysSpeculate.cpp - Speculate all operations -----------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the implementation of the AlwaysSpeculate pass.
//
//===----------------------------------------------------------------------===//
#include "shang/Passes.h"
#include "shang/Utilities.h"

#include "llvm/Pass.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Function.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/CFG.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-data-path-speculation"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumSpeculated, "Number of operations Speculated");
STATISTIC(NumBBSimplified, "Number of blocks simplified");

namespace {
struct AlwaysSpeculate : public FunctionPass {
  static char ID;
  AlwaysSpeculate() : FunctionPass(ID) {
    initializeAlwaysSpeculatePass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F);
  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<TargetTransformInfo>();
    AU.addRequired<DominatorTree>();
    AU.addRequired<LoopInfo>();
  }

  bool optimizePHI(PHINode *PN, DominatorTree &DT);
  bool speculateInst(Instruction *Inst, DominatorTree &DT);
  bool speculateInstInFunction(Function &F, DominatorTree &DT);
};
}

Pass *llvm::createAlwaysSpeculatePass() { return new AlwaysSpeculate(); }
char AlwaysSpeculate::ID = 0;

INITIALIZE_PASS_BEGIN(AlwaysSpeculate,
                      "shang-datapath-hoisting",
                      "Hoist the Datapath Instructions",
                      false, false)
  INITIALIZE_AG_DEPENDENCY(TargetTransformInfo)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
  INITIALIZE_PASS_DEPENDENCY(LoopInfo)
INITIALIZE_PASS_END(AlwaysSpeculate,
                    "shang-datapath-hoisting",
                    "Hoist the Datapath Instructions",
                    false, false)

bool AlwaysSpeculate::optimizePHI(PHINode *PN, DominatorTree &DT) {
  return false;
}

bool AlwaysSpeculate::speculateInst(Instruction *Inst, DominatorTree &DT) {
  if (PHINode *PN = dyn_cast<PHINode>(Inst))
    return optimizePHI(PN, DT);

  // Do not touch the following instructions.
  if (Inst->isTerminator() || isLoadStore(Inst) || Inst->mayThrow()
      || isCall(Inst, false))
    return false;

  BasicBlock *Dst = DT.getRoot(), *CurBB = Inst->getParent();

  typedef Instruction::op_iterator op_iterator;
  for (op_iterator I = Inst->op_begin(), E = Inst->op_end(); I != E; ++I) {
    if (Instruction *Src = dyn_cast<Instruction>(I)) {
      BasicBlock *SrcBB = Src->getParent();
      if (DT.dominates(Dst, SrcBB)) {
        Dst = SrcBB;
        continue;
      }

      assert(DT.dominates(SrcBB, Dst)
             && "Operands not in a path of the dominator tree!");
    }
  }

  if (Dst == CurBB) return false;

  DEBUG(dbgs() << "Moving " << *Inst << '\n');

  Inst->removeFromParent();

  Dst->getInstList().insert(Dst->getTerminator(), Inst);
  ++NumSpeculated;

  return true;
}

bool AlwaysSpeculate::speculateInstInFunction(Function &F, DominatorTree &DT) {
  bool changed = false;
  // Visit the BBs in topological order this can benefit some of the later
  // algorithms.
  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator rpo_iterator;
  for (rpo_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    BasicBlock *BB = *I;

    if (BB->getSinglePredecessor())
      FoldSingleEntryPHINodes(BB, this);

    for (BasicBlock::iterator BI = BB->begin(), BE = BB->end(); BI != BE; /*++BI*/)
      changed |= speculateInst(BI++, DT);
  }

  return changed;
}

static bool SimplifyCFGOnFunction(Function &F, const TargetTransformInfo &TTI,
                                  const DataLayout *TD) {
  bool Changed = false;
  bool LocalChange = true;
  while (LocalChange) {
    LocalChange = false;

    // Loop over all of the basic blocks and remove them if they are unneeded...
    //
    for (Function::iterator BBIt = F.begin(); BBIt != F.end(); ) {
      if (SimplifyCFG(BBIt++, TTI, TD)) {
        LocalChange = true;
        ++NumBBSimplified;
      }
    }
    Changed |= LocalChange;
  }
  return Changed;
}

static bool IsLegalToFold(BasicBlock *BB) {
  if (llvm::next(pred_begin(BB)) != pred_end(BB))
    return false;

  Instruction &Inst = BB->front();
  return isa<BranchInst>(Inst) || isa<SwitchInst>(Inst);
}

static
void CollectSuccessors(BasicBlock *BB, std::set<BasicBlock*> &Succs) {
  for (succ_iterator I = succ_begin(BB), E = succ_end(BB); I != E; ++I) {
    BasicBlock *Succ = *I;
    if (IsLegalToFold(Succ))
      Succs.insert(Succ);
  }
}

namespace {
struct BBCnds {
  typedef std::map<BasicBlock*, Value*> MapTy;
  typedef MapTy::iterator iterator;
  typedef MapTy::const_iterator const_iterator;
  MapTy BB2CndMap;
  Instruction *InsertPos;
  LLVMContext &Cnxt;
  explicit BBCnds(Instruction *InsertPos) : InsertPos(InsertPos),
    Cnxt(InsertPos->getContext()) {}

  const_iterator begin() const { return BB2CndMap.begin(); }
  const_iterator end() const { return BB2CndMap.end(); }

  void synthesizeNewTerminator() {
    assert(BB2CndMap.size() <= 64 && "Too many successors!");

    // Create a new terminator based on BB2CndMap
    std::vector<BasicBlock*> Succs;
    IntegerType *I64 = IntegerType::get(Cnxt, 64);
    // The condition of the new terminator.
    Value *Zero = ConstantInt::get(I64, 0);
    Value *Condition = Zero;

    std::vector<ConstantInt*> CaseValues;
    unsigned CaseValueCounter = 0;

    for (iterator I = BB2CndMap.begin(), E = BB2CndMap.end(); I != E; ++I) {
      BasicBlock *Succ = I->first;
      Succs.push_back(Succ);
      // Use one-hot encode for the case value.
      unsigned CurBit = CaseValueCounter++;
      ConstantInt *CurCase = ConstantInt::get(I64, 0x1 << (CurBit));
      CaseValues.push_back(CurCase);
      Value *Cnd64 = CastInst::CreateIntegerCast(I->second, I64, false, "",
                                                 InsertPos);
      Value *CurCaseHit = BinaryOperator::CreateShl(Cnd64,
                                                    ConstantInt::get(I64, CurBit),
                                                    "", InsertPos);
      Condition = BinaryOperator::Create(Instruction::Or, Condition, CurCaseHit,
                                         "", InsertPos);
    }

    //
    BasicBlock *DefaultBlock = Succs.back();
    Succs.pop_back();
    CaseValues.pop_back();

    SwitchInst *SI = SwitchInst::Create(Condition, DefaultBlock, Succs.size(),
                                        InsertPos);

    while (!Succs.empty()) {
      BasicBlock *Succ = Succs.back();
      Succs.pop_back();

      ConstantInt *CurCase = CaseValues.back();
      CaseValues.pop_back();

      SI->addCase(CurCase, Succ);
    }
  }

  Value *popCnd(BasicBlock *BB) {
    iterator J = BB2CndMap.find(BB);
    assert(J != BB2CndMap.end() && "Successor condition missed!");
    Value *Cnd = J->second;
    BB2CndMap.erase(J);
    return Cnd;
  }

  // Add a new BasicBlock, Condition pair to the condition mapping.
  void addNewCnd(BasicBlock *BB, Value *NewCnd) {
    Value *&Cnd = BB2CndMap[BB];
    // Or the existing conditions together.
    if (Cnd)
      NewCnd = BinaryOperator::Create(Instruction::Or, Cnd, NewCnd, "", InsertPos);

    Cnd = NewCnd;
  }

  void applyAndCnd(Value *Cnd) {
    for (iterator I = BB2CndMap.begin(), E = BB2CndMap.end(); I != E; ++I)
      I->second = BinaryOperator::Create(Instruction::And, Cnd, I->second, "",
                                         InsertPos);
  }

  void merge(const BBCnds &From) {
    for (const_iterator I = From.begin(), E = From.end(); I != E; ++I)
      addNewCnd(I->first, I->second);
  }

  void buildSuccessorCnd(BranchInst *Br) {
    IntegerType *I1 = IntegerType::get(Cnxt, 1);
    ConstantInt *True = ConstantInt::get(I1, 1);

    if (Br->isUnconditional()) {
      BB2CndMap.insert(std::make_pair(Br->getSuccessor(0), True));
      return;
    }

    Value *Cnd = Br->getCondition();
    BasicBlock *TrueBB = Br->getSuccessor(0);
    BB2CndMap.insert(std::make_pair(TrueBB, Cnd));

    BasicBlock *FalseBB = Br->getSuccessor(1);
    // Build the invert of Cnd with Xor Cnd, 1
    Value *NotCnd = BinaryOperator::Create(Instruction::Xor, Cnd, True,
                                           Cnd->getName() + ".invert", InsertPos);
    BB2CndMap.insert(std::make_pair(FalseBB, NotCnd));
  }

  void buildSuccessorCnd(SwitchInst *Sw) {
    Value *CaseCnd = Sw->getCondition();

    typedef SwitchInst::CaseIt CaseIt;
    for (CaseIt i = Sw->case_begin(), e = Sw->case_end(); i != e; ++i) {
      ConstantInt *CaseValue = i.getCaseValue();
      BasicBlock *Succ = i.getCaseSuccessor();
      assert(Succ != Sw->getDefaultDest() && "Unexpected default Dst appears in"
                                             " case Dsts");

      Value *CurCnd = new ICmpInst(InsertPos, CmpInst::ICMP_EQ,
                                   CaseCnd, CaseValue);

      addNewCnd(Succ, CurCnd);
    }

    // Now Build the Default condition.
    IntegerType *I1 = IntegerType::get(Cnxt, 1);
    Value *Cnd = ConstantInt::get(I1, 0);

    // First of all, we OR all non-default block's condition together.
    for (iterator I = BB2CndMap.begin(), E = BB2CndMap.end(); I != E; ++I)
      Cnd = BinaryOperator::Create(Instruction::Or, Cnd, I->second, "",
                                   InsertPos);

    // Then build the default block condition by inverting the OR of all
    // non-default condition.
    ConstantInt *True = ConstantInt::get(I1, 1);
    Cnd = BinaryOperator::Create(Instruction::Xor, Cnd, True, "",
                                 InsertPos);
    BB2CndMap.insert(std::make_pair(Sw->getDefaultDest(), Cnd));
  }


  void buildSuccessorCnd(TerminatorInst *TI) {
    if (BranchInst *Br = dyn_cast<BranchInst>(TI)) {
      buildSuccessorCnd(Br);
      return;
    }

    if (SwitchInst *Sw = dyn_cast<SwitchInst>(TI)) {
      buildSuccessorCnd(Sw);
      return;
    }

    llvm_unreachable("Unexpected terminator type!");
  }

  void fixSuccessorPHINodes(BasicBlock *BB, BasicBlock *Succ, BasicBlock *Succ2,
                            Value *EdgeCnd) {
    assert(InsertPos->getParent() == BB && "Illegal insert position,"
                                           " it may break the SSA form!");

    typedef BasicBlock::iterator iterator;
    for (iterator I = Succ2->begin(); isa<PHINode>(I); ++I) {
      PHINode *PN = cast<PHINode>(I);

      int BBIdx = PN->getBasicBlockIndex(BB);

      if (BBIdx == -1)
        return;
      
      int SuccIdx = PN->getBasicBlockIndex(Succ);

      Value *BBIncoming = PN->getIncomingValue(BBIdx),
            *SuccIncoming = PN->getIncomingValue(SuccIdx);

      Value *NewIncoming = SelectInst::Create(EdgeCnd, SuccIncoming, BBIncoming,
                                              "", InsertPos);
      PN->setIncomingValue(BBIdx, NewIncoming);
      PN->removeIncomingValue(SuccIdx);
    }
  }

  void fixSuccessorPHINodes(BasicBlock *BB, BasicBlock *Succ, Value *EdgeCnd) {
    for (succ_iterator I = succ_begin(Succ), E = succ_end(Succ); I != E; ++I) {
      BasicBlock *Succ2 = *I;

      // The predecessor list of Succ2 maybe inaccurate at this moment. Hence
      // we cannot check if BB is also the predecessor of Succ2 like this.
      // if (std::find(pred_begin(Succ2), pred_end(Succ2), BB) == pred_end(Succ2))
      //  continue;

      fixSuccessorPHINodes(BB, Succ, Succ2, EdgeCnd);
    }
  }
};
}

static bool FoldSuccessors(BasicBlock *BB) {
  std::set<BasicBlock*> FoldableSuccs;
  CollectSuccessors(BB, FoldableSuccs);

  if (FoldableSuccs.empty())
    return false;

  dbgs() << "Going to fold successors of " << BB->getName() << '\n';

  TerminatorInst *TI = BB->getTerminator();
  BBCnds CurCnds(TI), NewCnds(TI);
  CurCnds.buildSuccessorCnd(TI);

  typedef std::set<BasicBlock*>::iterator iterator;
  for (iterator I = FoldableSuccs.begin(), E = FoldableSuccs.end();
       I != E; ++I) {
    BasicBlock *Succ = *I;

    dbgs() << "Folding " << Succ->getName() << '\n';

    BBCnds SuccCnds(TI);
    SuccCnds.buildSuccessorCnd(Succ->getTerminator());
    Value *Cnd = CurCnds.popCnd(Succ);
    SuccCnds.applyAndCnd(Cnd);
    NewCnds.merge(SuccCnds);
    
    SuccCnds.fixSuccessorPHINodes(BB, Succ, Cnd);
    Succ->replaceAllUsesWith(BB);
    Succ->eraseFromParent();
  }

  NewCnds.merge(CurCnds);
  NewCnds.synthesizeNewTerminator();
  TI->eraseFromParent();

  verifyFunction(*BB->getParent());
  return true;
}

static bool FoldBlocks(Function &F) {
  bool changed = false;

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;
    bool LocalChanged = true;
  
    while (LocalChanged)
      changed |= (LocalChanged = FoldSuccessors(BB));
  }

  return changed;
}

bool AlwaysSpeculate::runOnFunction(Function &F) {
  DominatorTree &DT = getAnalysis<DominatorTree>();
  TargetTransformInfo &TTI = getAnalysis<TargetTransformInfo>();
  DataLayout *TD = getAnalysisIfAvailable<DataLayout>();

  bool changed = false;
  bool LocalChanged = true;
  while (LocalChanged) {
    changed |= (LocalChanged = speculateInstInFunction(F, DT));
    changed |= (LocalChanged = SimplifyCFGOnFunction(F, TTI, TD));
    changed |= (LocalChanged = FoldBlocks(F));
    // Recalculate the Dominator tree.
    DT.runOnFunction(F);
    verifyFunction(F);
  }

  return changed;
}
