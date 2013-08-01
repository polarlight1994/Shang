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
#include "llvm/ADT/PointerIntPair.h"

namespace llvm {
class DominatorTree;
class VASTSeqOp;
class VASTSlot;
class VASTSeqValue;
class TimingNetlist;
class raw_ostream;
class STGDistances;

template<typename T>
struct DataflowPtr : public PointerIntPair<T*, 1, bool> {
  typedef PointerIntPair<T*, 1, bool> Base;

  DataflowPtr(Base V = Base()) : PointerIntPair<T*, 1, bool>(V) {}

  DataflowPtr(T *V, bool IsLaunch)
    : PointerIntPair<T*, 1, bool>(V, IsLaunch) {}

  template<typename T1>
  DataflowPtr(const DataflowPtr<T1>& RHS)
    : PointerIntPair<T*, 1, bool>(RHS.get(), RHS.isInverted()) {}

  template<typename T1>
  DataflowPtr<T> &operator=(const DataflowPtr<T1> &RHS) {
    Base::setPointer(RHS.get());
    Base::setInt(RHS.isInverted());
    return *this;
  }

  bool IsLauch() const { return Base::getInt(); }

  operator T*() const {
    return Base::getPointer();
   }

  T *operator->() const {
    return Base::getPointer();
  }
};

template<typename T>
struct DenseMapInfo<DataflowPtr<T> >
  : public DenseMapInfo<PointerIntPair<T*, 1, bool> > {};

// simplify_type - Allow clients to treat uses just like values when using
// casting operators.
template<typename T> struct simplify_type<DataflowPtr<T> > {
  typedef T* SimpleType;
  static SimpleType getSimplifiedValue(DataflowPtr<T> Val) {
    return Val.getPointer();
  }
};

typedef DataflowPtr<Value> DataflowValue;
typedef DataflowPtr<Instruction> DataflowInst;

class Dataflow : public FunctionPass {
public:
  typedef std::map<DataflowValue, float> SrcSet;
private:
  DominatorTree *DT;
  typedef std::map<DataflowValue, std::pair<float, unsigned> > TimedSrcSet;
  typedef std::map<DataflowInst, TimedSrcSet> FlowDepMapTy;
  FlowDepMapTy FlowDeps;
  typedef std::map<BasicBlock*, TimedSrcSet> IncomingBBMapTy;
  typedef std::map<DataflowInst, IncomingBBMapTy>  IncomingMapTy;
  IncomingMapTy Incomings;

  BasicBlock *getIncomingBlock(VASTSlot *S, Instruction *Inst, Value *Src) const;
  TimedSrcSet &getDeps(DataflowInst Inst, BasicBlock *Parent);

  unsigned generation;
  float updateDelay(float NewDelay, float Ratio,
                    std::pair<float, unsigned> &OldDelay);

  void dumpIncomings(raw_ostream &OS) const;
  void dumpFlowDeps(raw_ostream &OS) const;
public:
  static char ID;
  Dataflow();

  void increaseGeneration() { ++generation; }
  unsigned getGeneration() const { return generation; }

  float annotateDelay(DataflowInst Inst, VASTSlot *S, DataflowValue V,
                      float delay, unsigned Slack);
  float getDelay(DataflowValue Src, DataflowInst Dst, VASTSlot *S) const;

  void getFlowDep(DataflowInst Inst, SrcSet &Set) const;
  void getIncomingFrom(DataflowInst Inst, BasicBlock *BB, SrcSet &Set) const;

  float getSlackFromLaunch(Instruction *Inst) const;

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnFunction(Function &F);
  void releaseMemory();

  void dumpToSQL() const;
};

class DataflowAnnotation : public VASTModulePass {
  Dataflow *DF;
  STGDistances *Distances;
  const bool Accumulative;

  bool externalDelayAnnotation(VASTModule &VM);
  void extractFlowDep(VASTSeqOp *SeqOp, TimingNetlist &TNL);
  void internalDelayAnnotation(VASTModule &VM);

  float annotateDelay(DataflowInst Inst, VASTSlot *S, VASTSeqValue *V,
                      float delay);
public:

  static char ID;
  explicit DataflowAnnotation(bool Accumulative = false);

  typedef Dataflow::SrcSet SrcSet;
  void getFlowDep(DataflowInst Inst, SrcSet &Set) const {
    DF->getFlowDep(Inst, Set);
  }

  void getIncomingFrom(DataflowInst Inst, BasicBlock *BB, SrcSet &Set) const {
    DF->getIncomingFrom(Inst, BB, Set);
  }

  float getSlackFromLaunch(Instruction *Inst) const {
    return DF->getSlackFromLaunch(Inst);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
};
}

#endif
