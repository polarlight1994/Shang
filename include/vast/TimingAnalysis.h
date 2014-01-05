//------ TimingAnalysis.h - Abstract Interface for Timing Analysis -*- C++ -*-//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the abstract interface for timing analsysis
//
//===----------------------------------------------------------------------===//

#ifndef VAST_TIMING_ANAYSIS_H
#define VAST_TIMING_ANAYSIS_H

#include "llvm/ADT/None.h"
#include <map>

namespace llvm {
class Pass;
class AnalysisUsage;
class BasicBlock;
class raw_ostream;
}

namespace vast {
using namespace llvm;

class VASTValue;
class VASTExpr;
struct VASTLatch;
class VASTSeqValue;
class VASTSelector;
class VASTModule;
class VASTExprBuilder;
class Dataflow;

class TimingAnalysis {
  // Previous TimingAnalysis to chain to.
  TimingAnalysis *TA;
protected:

  void InitializeTimingAnalysis(Pass *P);

  /// getAnalysisUsage - All TimingAnalysis implementations should invoke this
  /// directly (using TimingAnalysis::getAnalysisUsage(AU)).
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
public:
  static char ID;

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

  TimingAnalysis() : TA(0) {}

  virtual ~TimingAnalysis() {}
  virtual PhysicalDelay getArrivalTime(VASTSelector *To, VASTSeqValue *From);
  virtual PhysicalDelay getArrivalTime(VASTSelector *To, VASTExpr *Thu,
                                       VASTSeqValue *From);

  virtual void printArrivalPath(raw_ostream &OS, VASTSelector *To, VASTValue *From);
  virtual void printArrivalPath(raw_ostream &OS, VASTSelector *To, VASTExpr *Thu,
                                VASTValue *From);

  typedef std::map<VASTSeqValue*, PhysicalDelay> ArrivalMap;
  void extractDelay(const VASTLatch &L, VASTValue *V, ArrivalMap &Arrivals);

  virtual bool isBasicBlockUnreachable(BasicBlock *BB) const;
};
}

#endif
