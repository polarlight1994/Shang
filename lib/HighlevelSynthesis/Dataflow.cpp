//===------- Dataflow.cpp - Dataflow Analysis on LLVM IR --------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the interface of Dataflow Analysis. The dataflow analysis
// build the flow dependencies on LLVM IR.
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "TimingNetlist.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"

#include "llvm/Analysis/Dominators.h"
#define DEBUG_TYPE "shang-dataflow"
#include "llvm/Support/Debug.h"

using namespace llvm;

Dataflow::Dataflow() : FunctionPass(ID) {
  initializeDataflowPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(Dataflow,
                      "vast-dataflow", "Dataflow Anlaysis", false, true)
  INITIALIZE_PASS_DEPENDENCY(DominatorTree)
INITIALIZE_PASS_END(Dataflow,
                    "vast-dataflow", "Dataflow Anlaysis", false, true)

char Dataflow::ID = 0;
char &llvm::DataflowID = Dataflow::ID;

void Dataflow::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DominatorTree>();
  AU.setPreservesAll();
}

void Dataflow::releaseMemory() {
  FlowDeps.clear();
  Incomings.clear();
}

void Dataflow::getFlowDep(Instruction *Inst, SrcSet &Set) const {
  std::map<Instruction*, SrcSet>::const_iterator I = FlowDeps.find(Inst);
  if (I == FlowDeps.end()) {
    //assert(isa<TerminatorInst>(Inst) && "Flow dependencies do not exists?");
    return;
  }

  assert(Set.empty() && "Expect empty set!");
  Set.insert(I->second.begin(), I->second.end());
}

void
Dataflow::getIncomingFrom(Instruction *Inst, BasicBlock *BB, SrcSet &Set) const {
  std::map<Instruction*, std::map<BasicBlock*, SrcSet> >::const_iterator
    I = Incomings.find(Inst);

  if (I == Incomings.end()) {
    assert((!isa<PHINode>(Inst) ||
            isa<Constant>(cast<PHINode>(Inst)->getIncomingValueForBlock(BB)) ||
            isa<GlobalVariable>(cast<PHINode>(Inst)->getIncomingValueForBlock(BB))) &&
           "Incoming value dose not existed?");
    return;
  }

  std::map<BasicBlock*, SrcSet>::const_iterator J = I->second.find(BB);
  if (J == I->second.end()) {
    //assert((!isa<PHINode>(Inst) ||
    //        isa<Constant>(cast<PHINode>(Inst)->getIncomingValueForBlock(BB)) ||
    //        isa<GlobalVariable>(cast<PHINode>(Inst)->getIncomingValueForBlock(BB))) &&
    //       "Incoming value dose not existed?");
    return;
  }

  assert(Set.empty() && "Expect empty set!");
  Set.insert(J->second.begin(), J->second.end());
}

void Dataflow::annotateDelay(Instruction *Inst, BasicBlock *Parent,
                             Value *V, float delay) {
  assert(V && "Unexpected VASTSeqValue without underlying llvm Value!");

  if (!isa<PHINode>(Inst) && Inst->getParent() == Parent) {
    FlowDeps[Inst][V] = delay;
    return;
  }

  Incomings[Inst][Parent][V] = delay;
}

DataflowAnnotation::DataflowAnnotation(bool Accumulative)
  : VASTModulePass(ID), Accumulative(Accumulative) {
  initializeDataflowPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(DataflowAnnotation,
                      "vast-dataflow-annotation", "Dataflow Annotation",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(Dataflow)
  INITIALIZE_PASS_DEPENDENCY(TimingNetlist)
  INITIALIZE_PASS_DEPENDENCY(ControlLogicSynthesis)
  INITIALIZE_PASS_DEPENDENCY(SelectorSynthesis)
  INITIALIZE_PASS_DEPENDENCY(DatapathNamer)
INITIALIZE_PASS_END(DataflowAnnotation,
                    "vast-dataflow-annotation", "Dataflow Annotation",
                    false, true)

char DataflowAnnotation::ID = 0;

void DataflowAnnotation::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequired<Dataflow>();
  AU.addRequiredID(ControlLogicSynthesisID);
  AU.addRequiredID(SelectorSynthesisID);
  AU.addRequiredID(DatapathNamerID);
  AU.addRequired<TimingNetlist>();
  AU.setPreservesAll();
}

BasicBlock *DataflowAnnotation::getIncomingBB(VASTSeqOp *Op) {
  VASTSlot *S = Op->getSlot();
  BasicBlock *ParentBB = S->getParent();

  if (S->IsSubGrp) {
    S = S->getParentGroup();
    if (BasicBlock *BB = S->getParent())
      ParentBB = BB;
  }

  return ParentBB;
}

void DataflowAnnotation::annotateDelay(Instruction *Inst, BasicBlock *Parent,
                                       VASTSeqValue *SV, float delay) {
  DF->annotateDelay(Inst, Parent, SV->getLLVMValue(), delay);
}

void DataflowAnnotation::extractFlowDep(VASTSeqOp *Op, TimingNetlist &TNL) {
  Instruction *Inst = dyn_cast_or_null<Instruction>(Op->getValue());

  // Nothing to do if Op does not have an underlying instruction.
  if (!Inst)
    return;

  std::map<VASTSeqValue*, float> Srcs;

  VASTValPtr Cnd = Op->getGuard();
  TNL.extractDelay(0, Cnd.get(), Srcs);

  for (unsigned i = 0, e = Op->num_srcs(); i != e; ++i) {
    VASTLatch L = Op->getSrc(i);
    VASTValPtr FI = L;

    // Extract the delay from the fan-in and the guarding condition.
    TNL.extractDelay(L.getSelector(), FI.get(), Srcs);
    TNL.extractDelay(L.getSelector(), Cnd.get(), Srcs);
  }

  BasicBlock *ParentBB = getIncomingBB(Op);

  typedef std::map<VASTSeqValue*, float>::iterator src_iterator;
  for (src_iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I)
    annotateDelay(Inst, ParentBB, I->first, I->second);
}

void DataflowAnnotation::internalDelayAnnotation(VASTModule &VM) {
  TimingNetlist &TNL = getAnalysis<TimingNetlist>();

  typedef VASTModule::seqop_iterator iterator;
  for (iterator I = VM.seqop_begin(), E = VM.seqop_end(); I != E; ++I)
    extractFlowDep(I, TNL);
}

bool DataflowAnnotation::runOnVASTModule(VASTModule &VM) {
  DF = &getAnalysis<Dataflow>();

  // Force release the context if the annotatio is not accumulative.
  if (!Accumulative)
    DF->releaseMemory();

  externalDelayAnnotation(VM);

  return false;
}
