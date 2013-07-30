//===--------- Dataflow.h - Dataflow Analysis on LLVM IR --------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the interface of Dataflow Analysis. The dataflow analysis
// build the flow dependencies on LLVM IR.
//
//===----------------------------------------------------------------------===//

#ifndef DATAFLOW_ANALYSIS_H
#define DATAFLOW_ANALYSIS_H

#include "shang/VASTModulePass.h"

#include "llvm/IR/Instructions.h"

namespace llvm {
class DominatorTree;
class VASTSeqOp;
class VASTSlot;
class VASTSeqValue;
class TimingNetlist;

class Dataflow : public FunctionPass {
public:
  typedef std::map<Value*, float> SrcSet;
private:
  typedef std::pair<Value*, float> SrcTy;
  std::map<Instruction*, SrcSet> FlowDeps;
  std::map<Instruction*, std::map<BasicBlock*, SrcSet> > Incomings;
  DominatorTree *DT;

  void annotateTriangleDelayFromPHI(SrcSet &Deps, Instruction *Src);
  SrcSet &getDeps(Instruction *Inst, BasicBlock *Parent);
  void dumpIncomings(raw_ostream &OS) const;
  void dumpFlowDeps(raw_ostream &OS) const;
public:
  static char ID;
  Dataflow();

  void annotateDelay(Instruction *Inst, VASTSlot *S, Value *V, float delay);

  void getFlowDep(Instruction *Inst, SrcSet &Set) const;
  void getIncomingFrom(Instruction *Inst, BasicBlock *BB, SrcSet &Set) const;

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnFunction(Function &F);
  void releaseMemory();

  void dumpToSQL() const;
};

class DataflowAnnotation : public VASTModulePass {
  Dataflow *DF;
  const bool Accumulative;

  bool externalDelayAnnotation(VASTModule &VM);
  void extractFlowDep(VASTSeqOp *SeqOp, TimingNetlist &TNL);
  void internalDelayAnnotation(VASTModule &VM);

  void annotateDelay(Instruction *Inst, VASTSlot *S,
                     VASTSeqValue *V, float delay);
public:

  static char ID;
  explicit DataflowAnnotation(bool Accumulative = false);

  typedef Dataflow::SrcSet SrcSet;
  void getFlowDep(Instruction *Inst, SrcSet &Set) const {
    DF->getFlowDep(Inst, Set);
  }

  void getIncomingFrom(Instruction *Inst, BasicBlock *BB, SrcSet &Set) const {
    DF->getIncomingFrom(Inst, BB, Set);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
};
}

#endif
