//--- SIRTimingAnalysis.h - Abstract Interface for Timing Analysis -*- C++ -*-//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the abstract interface for timing analysis
//
//===----------------------------------------------------------------------===//
#include "sir/SIRPass.h"

#include "llvm/IR/Instruction.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/ArrayRef.h"
#include <map>

#ifndef SIR_TIMING_ANALYSIS_H
#define SIR_TIMING_ANALYSIS_H

namespace llvm {
class Pass;
class AnalysisUsage;
class BasicBlock;
class Value;
class raw_ostream;

class SIR;
class SIRRegister;

// Bit level arrival time.
struct ArrivalTime : public ilist_node<ArrivalTime> {
  Value *Src;
  float Arrival;
  // Arrival to bit range [ToLB, ToUB)
  uint8_t ToUB, ToLB;
  ArrivalTime(Value *Src, float Arrival, uint8_t ToUB, uint8_t ToLB);
  ArrivalTime() : Src(NULL), Arrival(0.0f), ToUB(0), ToLB(0) {}
  void verify() const;

  unsigned width() const { return ToUB - ToLB; }
};

class SIRDelayModel : public ilist_node<SIRDelayModel> {
  // Need the SIR module and the Data Layout
  // to provide some basic informations
  // like bit-width and so on.
  SIR *SM;
  DataLayout &TD;
  Instruction *Node;
  // The FanIn to the current delay model, order matters.
  ArrayRef<SIRDelayModel *> Fanins;

  ilist<ArrivalTime> Arrivals;
  // The delay from Src value to this model
  std::map<Value *, ArrivalTime *> ArrivalStart;

  ilist<ArrivalTime>::iterator
    findInsertPosition(ArrivalTime *Start, Value *V, uint8_t ToLB);

  // The arrival time from Src Value to [ToLB, ToUB) bit range of this model
  void addArrival(Value *V, float Arrival, uint8_t ToUB, uint8_t ToLB);

  void updateArrivalCarryChain(unsigned i, float Base, float PerBit);
  void updateArrivalCritial(unsigned i, float Delay);
  void updateArrivalCritial(float delay);
  void updateArrivalParallel(unsigned i, float Delay);
  void updateArrivalParallel(float delay);
  

  void updateBitCatArrival();
  void updateBitRepeatArrival();
  void updateBitExtractArrival();
  void updateBitMaskArrival();
  void updateReductionArrival();
  void updateROMLookUpArrival();

  template<typename VFUTy>
  void updateCarryChainArrival(VFUTy *FU)  {
    unsigned BitWidth = Node->getBitWidth();

    // Dirty HACK: We only have the data up to 64 bit FUs.
    float Delay = FU->lookupLatency(std::min(BitWidth, 64u));
    float PreBit = Delay / BitWidth;

    unsigned NumOperands = Node->size();
    float Base = PreBit * (NumOperands - 1);

    for (unsigned i = 0, e = Node->size(); i < e; ++i)
      updateArrivalCarryChain(i, Base, PreBit);
  }

  void updateCmpArrivial();
  void updateShiftAmt();
  void updateShlArrival();
  void updateShrArrival();
public:
  SIRDelayModel();
  SIRDelayModel(SIR *SM, DataLayout &TD, Instruction *Node,
                ArrayRef<SIRDelayModel *> Fanins);
  ~SIRDelayModel();

  typedef ArrayRef<SIRDelayModel>::iterator iterator;

  void updateArrival();

  void verify() const;

  // Iterative the arrival times.
  typedef ilist<ArrivalTime>::iterator arrival_iterator;
  arrival_iterator arrival_begin() { return Arrivals.begin(); }
  arrival_iterator arrival_end() { return Arrivals.end(); }
  typedef ilist<ArrivalTime>::const_iterator const_arrival_iterator;
  const_arrival_iterator arrival_begin() const { return Arrivals.begin(); }
  const_arrival_iterator arrival_end() const { return Arrivals.end(); }

  bool hasArrivalFrom(Value *V) const {
    return ArrivalStart.count(V);
  }

  // Get the iterator point to the first arrival time of a given source node.
  const_arrival_iterator arrival_begin(Value *V) const {
    std::map<Value *, ArrivalTime *>::const_iterator I = ArrivalStart.find(V);
    if (I == ArrivalStart.end())
      return Arrivals.end();

    return I->second;
  }

  // Visit from the iterator I to the iterator which targets V.
  bool inRange(const_arrival_iterator I, Value *V) const {
    if (I == Arrivals.end())
      return false;

    return I->Src == V;
  }
};

class SIRTimingAnalysis : public SIRPass {
private:
  void InitializeSIRTimingAnalysis(Pass *P);
  void getAnalysisUsage(AnalysisUsage &AU) const;

public:
  static char ID;

  SIRTimingAnalysis() : SIRPass(ID) {}
  ~SIRTimingAnalysis() {}

  bool runOnSIR(SIR &SM);

  // Data structure that explicitly hold the total delay and cell delay of a
  // datapath. Based on total delay and cell delay we can calculate the
  // corresponding wire delay.
  struct PhysicalDelay {
    float TotalDelay;
    float CellDelay;

    PhysicalDelay() : TotalDelay(0.0f), CellDelay(0.0f) {}
    PhysicalDelay(float TotalDelay, float CellDelay)
      : TotalDelay(TotalDelay), CellDelay(CellDelay) {}

    explicit PhysicalDelay(float TotalDelay)
      : TotalDelay(TotalDelay), CellDelay(TotalDelay) {}
    PhysicalDelay(NoneType)
      : TotalDelay(-1.1e+10f), CellDelay(-1.1e+10f) {}

    bool operator==(NoneType) const {
      return TotalDelay < 0.0f;
    }

    bool operator!=(NoneType) const {
      return !operator==(None);
    }

    bool operator < (const PhysicalDelay &RHS) const {
      return TotalDelay < RHS.TotalDelay;
    }

    PhysicalDelay operator+(const PhysicalDelay &RHS) const {
      if (operator==(None) || RHS == None)
        return None;

      return PhysicalDelay(TotalDelay + RHS.TotalDelay,
        CellDelay + RHS.CellDelay);
    }

    operator float() const {
      return TotalDelay;
    }
  };

  // The bit-level delay model.
  ilist<SIRDelayModel> Models;
  std::map<Instruction *, SIRDelayModel *> ModelMap;
  
  SIRDelayModel *createModel(Instruction *Inst, SIR *SM, DataLayout &TD);
  SIRDelayModel *lookUpDelayModel(Instruction *Inst) const;

  void buildTimingNetlist(Value *V, SIR *SM, DataLayout &TD);

  PhysicalDelay getArrivalTime(Value *To, Value *From);
  PhysicalDelay getArrivalTime(SIRRegister *To, Value *From);
  PhysicalDelay getArrivalTime(SIRRegister *To, Value *Thu,
                               Value *From);

  void printArrivalPath(raw_ostream &OS, SIRRegister *To, Value *From);
  void printArrivalPath(raw_ostream &OS, SIRRegister *To, Value *Thu,
                        Value *From);

  typedef std::map<Value *, PhysicalDelay> ArrivalMap;
  void extractDelay(SIR *SM, SIRRegister *Reg, Value *V, ArrivalMap &Arrivals);

  bool isBasicBlockUnreachable(BasicBlock *BB) const;
};

}


#endif