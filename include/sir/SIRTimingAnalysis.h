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
#include "sir/SIR.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

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
  ArrivalTime(Value *Src, float Arrival, uint8_t ToUB, uint8_t ToLB)
    : Src(Src), Arrival(Arrival), ToUB(ToUB), ToLB(ToLB) {}
  ArrivalTime() : Src(NULL), Arrival(0.0f), ToUB(0), ToLB(0) {}
  void verify() const;

  unsigned width() const { return ToUB - ToLB; }
};

class SIRDelayModel : public ilist_node<SIRDelayModel> {
  // Need the SIR module and the Data Layout
  // to provide some basic informations
  // like bit-width and so on.
  SIR *SM;
  DataLayout *TD;
  Instruction *Node;

  // The delay from input to the output bit.
  std::map<unsigned, float> ModelDelay;

  void calcArrivalParallel(float Delay);
  void calcArrivalLinear(float Base, float PerBit);

  void calcAddArrival();
  void calcMulArrival();
  void calcCmpArrivial();
  void calcShiftArrival();

public:
  SIRDelayModel() {}
  SIRDelayModel(SIR *SM, DataLayout *TD, Instruction *Node);

  void calcArrival();
  float getDelayInBit(unsigned BitNum);
};

class SIRTimingAnalysis : public SIRPass {
private:
  void getAnalysisUsage(AnalysisUsage &AU) const;

public:
  static char ID;

  SIRTimingAnalysis() : SIRPass(ID) {
    initializeSIRTimingAnalysisPass(*PassRegistry::getPassRegistry());
  }

  ~SIRTimingAnalysis() {}

  bool runOnSIR(SIR &SM);

  // Data structure that explicitly hold the delay of a data-path. 
  struct PhysicalDelay {
    float Delay;

    PhysicalDelay() : Delay(0.0f) {}
    PhysicalDelay(float Delay) : Delay(Delay) {}

    bool operator==(NoneType) const {
      return Delay < 0.0f;
    }

    bool operator!=(NoneType) const {
      return !operator==(None);
    }

    bool operator < (const PhysicalDelay &RHS) const {
      return Delay < RHS.Delay;
    }

    PhysicalDelay operator+(const PhysicalDelay &RHS) const {
      if (operator==(None) || RHS == None)
        return None;

      return PhysicalDelay(Delay + RHS.Delay);
    }

    operator float() const {
      return Delay;
    }
  };

  // The bit-level delay model.
  ilist<SIRDelayModel> Models;
  std::map<Instruction *, SIRDelayModel *> ModelMap;

  SIRDelayModel *createModel(Instruction *Inst, SIR *SM, DataLayout &TD);
  SIRDelayModel *lookUpDelayModel(Instruction *Inst) const;

  typedef std::map<Value *, PhysicalDelay> ArrivalMap;
  void extractArrivals(DataLayout *TD, SIRSeqOp *SeqOp, ArrivalMap &Arrivals);
  void extractArrivals(DataLayout *TD, Instruction *CombOp, ArrivalMap &Arrivals);

  bool isBasicBlockUnreachable(BasicBlock *BB) const;
};
}


#endif