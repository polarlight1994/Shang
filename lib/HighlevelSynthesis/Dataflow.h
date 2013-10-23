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

template<typename T>
struct simplify_type<const DataflowPtr<T> >
  : public simplify_type<DataflowPtr<T> > {};

typedef DataflowPtr<Value> DataflowValue;
typedef DataflowPtr<Instruction> DataflowInst;

class Dataflow : public FunctionPass {
  struct Annotation {
    float delay_sum, delay_sqr_sum, ic_delay_sum, ic_delay_sqr_sum;
    uint16_t num_samples;
    uint8_t generation;
    uint8_t violation;
    Annotation()
      : delay_sum(0), delay_sqr_sum(0), ic_delay_sum(0), ic_delay_sqr_sum(0),
        num_samples(0), generation(0), violation(0) {}

    void addSample(float delay, float ic_delay);
    void reset();
    void dump() const;
  };
public:
  struct delay_type {
    float total_delay, ic_delay;
    explicit delay_type(float total_delay = 0.0f, float ic_delay = 0.0f)
      : total_delay(total_delay), ic_delay(ic_delay) {}

    delay_type(const Annotation &Ann);
    void reduce_max(const delay_type &RHS);
    float expected() const;
    float expected_ic_delay() const;
  };
  typedef std::map<DataflowValue, delay_type> SrcSet;
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
  void updateDelay(float NewDelay, float NewICDelay, Annotation &OldDelay,
                   bool IsTimingViolation);

  std::set<BasicBlock*> UnreachableBlocks;

  void dumpIncomings(raw_ostream &OS) const;
  void dumpFlowDeps(raw_ostream &OS) const;
public:
  static char ID;
  Dataflow();

  void increaseGeneration() { ++generation; }
  unsigned getGeneration() const { return generation; }

  void annotateDelay(DataflowInst Inst, VASTSlot *S, DataflowValue V,
                     float delay, float ic_delay, unsigned Slack);
  delay_type getDelay(DataflowValue Src, DataflowInst Dst, VASTSlot *S) const;

  void getFlowDep(DataflowInst Inst, SrcSet &Set) const;
  void getIncomingFrom(DataflowInst Inst, BasicBlock *BB, SrcSet &Set) const;

  delay_type getSlackFromLaunch(Instruction *Inst) const;
  delay_type getDelayFromLaunch(Instruction *Inst) const;

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
                     float delay, float ic_delay);
public:
  typedef Dataflow::delay_type delay_type;

  static char ID;
  explicit DataflowAnnotation(bool Accumulative = false);

  typedef Dataflow::SrcSet SrcSet;
  void getFlowDep(DataflowInst Inst, SrcSet &Set) const {
    DF->getFlowDep(Inst, Set);
  }

  void getIncomingFrom(DataflowInst Inst, BasicBlock *BB, SrcSet &Set) const {
    DF->getIncomingFrom(Inst, BB, Set);
  }

  delay_type getSlackFromLaunch(Instruction *Inst) const {
    return DF->getSlackFromLaunch(Inst);
  }

  delay_type getDelayFromLaunch(Instruction *Inst) const {
    return DF->getDelayFromLaunch(Inst);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
};
}

#endif
