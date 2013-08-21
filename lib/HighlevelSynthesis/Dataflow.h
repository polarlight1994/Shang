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

#include "shang/VASTSeqValue.h"
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

  DataflowPtr(Base V = Base()) : Base(V) {}

  DataflowPtr(T *V, bool IsLaunch)
    : Base(V, IsLaunch) {}

  inline DataflowPtr(VASTSeqOp *Op);
  inline DataflowPtr(VASTSeqValue *SV);

  template<typename T1>
  DataflowPtr(const DataflowPtr<T1>& RHS)
    : PointerIntPair<T*, 1, bool>(RHS.get(), RHS.isInverted()) {}

  template<typename T1>
  DataflowPtr<T> &operator=(const DataflowPtr<T1> &RHS) {
    Base::setPointer(RHS.getPointer());
    Base::setInt(RHS.getInt());
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

template<>
inline DataflowPtr<Instruction>::DataflowPtr(VASTSeqOp *Op)
  : Base(dyn_cast_or_null<Instruction>(Op->getValue()), false) {
  if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(Op))
    setInt(SeqInst->isLaunch());
}

template<>
inline DataflowPtr<Value>::DataflowPtr(VASTSeqValue *SV)
  : Base(SV->getLLVMValue(), SV->isFUInput() || SV->isFUOutput()) {}

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
  // BasicBlock descriptor: The pointer and the "IsSubGrp" flag
  struct BBPtr : public PointerIntPair<BasicBlock*, 1, bool> {
    typedef PointerIntPair<BasicBlock*, 1, bool> Base;

    BBPtr(Base BB = Base()) : Base(BB) {}

    BBPtr(BasicBlock *BB, bool IsSubGrp)
      : Base(BB, IsSubGrp) {}

    BBPtr(VASTSlot *S);

    BBPtr &operator=(const BBPtr &RHS) {
      Base::setPointer(RHS.getPointer());
      Base::setInt(RHS.getInt());
      return *this;
    }

    bool isSubGrp() const { return Base::getInt(); }

    operator BasicBlock*() const {
      return Base::getPointer();
    }

    BasicBlock *operator->() const {
      return Base::getPointer();
    }

    bool operator==(const BasicBlock *BB) const {
      return getPointer() == BB;
    }
  };

  struct Annotation {
    float delay;
    uint16_t generation;
    uint8_t violation;
    Annotation(float delay = 0.0f, uint16_t generation = 0,
               uint8_t violation = 0)
      : delay(delay), generation(generation), violation(violation) {}
  };

  DominatorTree *DT;
  typedef std::map<DataflowValue, Annotation> TimedSrcSet;
  typedef std::map<DataflowInst, TimedSrcSet> FlowDepMapTy;
  FlowDepMapTy FlowDeps;
  typedef std::map<BasicBlock*, TimedSrcSet> IncomingBBMapTy;
  typedef std::map<DataflowInst, IncomingBBMapTy>  IncomingMapTy;
  IncomingMapTy Incomings;

  BBPtr getIncomingBlock(VASTSlot *S, Instruction *Inst, Value *Src) const;
  TimedSrcSet &getDeps(DataflowInst Inst, BBPtr Parent);

  unsigned generation;
  void updateDelay(float NewDelay, float Ratio, Annotation &OldDelay);

  std::set<BasicBlock*> UnreachableBlocks;

  void dumpIncomings(raw_ostream &OS) const;
  void dumpFlowDeps(raw_ostream &OS) const;
public:
  static char ID;
  Dataflow();

  void increaseGeneration() { ++generation; }
  unsigned getGeneration() const { return generation; }

  void annotateDelay(DataflowInst Inst, VASTSlot *S, DataflowValue V,
                     float delay, unsigned Slack);
  float getDelay(DataflowValue Src, DataflowInst Dst, VASTSlot *S) const;

  void getFlowDep(DataflowInst Inst, SrcSet &Set) const;
  void getIncomingFrom(DataflowInst Inst, BasicBlock *BB, SrcSet &Set) const;

  float getSlackFromLaunch(Instruction *Inst) const;
  float getDelayFromLaunch(Instruction *Inst) const;

  void addUnreachableBlocks(BasicBlock *BB) {
    UnreachableBlocks.insert(BB);
  }

  bool isBlockUnreachable(BasicBlock *BB) const {
    return UnreachableBlocks.count(BB);
  }

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

  void annotateDelay(DataflowInst Inst, VASTSlot *S, VASTSeqValue *V,
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

  float getDelayFromLaunch(Instruction *Inst) const {
    return DF->getDelayFromLaunch(Inst);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
};
}

#endif
