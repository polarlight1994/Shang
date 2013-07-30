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
class VASTSeqValue;
class TimingNetlist;

class Dataflow : public VASTModulePass {
public:
  typedef std::map<Value*, float> SrcSet;
private:
  typedef std::pair<Value*, float> SrcTy;
  std::map<Instruction*, SrcSet> FlowDeps;
  std::map<Instruction*, std::map<BasicBlock*, SrcSet> > Incomings;
  DominatorTree *DT;

  void extractFlowDep(VASTSeqOp *SeqOp, TimingNetlist &TNL);
public:
  static char ID;
  Dataflow();
  
  void getFlowDep(Instruction *Inst, SrcSet &Set) const;
  void getIncomingFrom(Instruction *Inst, BasicBlock *BB, SrcSet &Set) const;
  void annotateDelay(Instruction *Inst, BasicBlock *Parent,
                     VASTSeqValue *V, float delay);

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
};
}

#endif
