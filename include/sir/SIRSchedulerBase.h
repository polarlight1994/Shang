//===- SIRSchedulingBase.h - ForceDirected information analyze --*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the Force Direct information computation pass describe in
// Force-Directed Scheduling for the Behavioral Synthesis of ASIC's
//
//===----------------------------------------------------------------------===//
#ifndef SIR_SCHEDULER_BASE
#define SIR_SCHEDULER_BASE

#include "SIRSchedGraph.h"

#include "llvm/ADT/PriorityQueue.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/GraphTraits.h"
#include <map>

namespace llvm {
class SIRScheduleBase {
public:
  const unsigned EntrySlot;
  static const unsigned MaxSlot = UINT16_MAX >> 2;
  struct TimeFrame {
    unsigned ASAP, ALAP;

    TimeFrame(unsigned ASAP = UINT32_MAX, unsigned ALAP = 0)
      : ASAP(ASAP), ALAP(ALAP) {}

    TimeFrame(const TimeFrame &RHS) : ASAP(RHS.ASAP), ALAP(RHS.ALAP) {}
    TimeFrame &operator=(const TimeFrame &RHS) {
      ASAP = RHS.ASAP;
      ALAP = RHS.ALAP;
      return *this;
    }

    operator bool() const {
      return ASAP <= ALAP;
    }

    TimeFrame operator+(unsigned i) const {
      return TimeFrame(ASAP + i, ALAP + i);
    }

    float size() const {
      return ALAP - ASAP;
    }
  };

protected:
  unsigned CriticalPathEnd;

  typedef std::map<const SIRSchedUnit *, TimeFrame> TFMapTy;
  TFMapTy SUnitToTF;

  SIRSchedGraph &G;

public:
  SIRScheduleBase(SIRSchedGraph &G, unsigned EntrySlot)
    : EntrySlot(EntrySlot), CriticalPathEnd(0), G(G) {}

  // Forward some functions from SIRScheGraph.
  ArrayRef<SIRSchedUnit *> lookupSUs(Value *V) {
    return G.lookupSUs(V);
  }
  ArrayRef<SIRSchedUnit *> getSUsInBB(BasicBlock *BB) {
    return G.getSUsInBB(BB);
  }

  virtual unsigned calculateASAP(const SIRSchedUnit *A) const;
  virtual unsigned calculateALAP(const SIRSchedUnit *A) const;
  TimeFrame calculateTimeFrame(const SIRSchedUnit *A) const {
    return TimeFrame(this->calculateASAP(A), this->calculateALAP(A));
  }
  TimeFrame getTimeFrame(const SIRSchedUnit *A) const {
    TFMapTy::const_iterator at = SUnitToTF.find(A);
    assert(at != SUnitToTF.end() && "TimeFrame for SU not exist!");
    return at->second;
  }

  bool buildASAPStep();
  bool buildALAPStep();

  SIRSchedGraph &operator*() const { return G; }
  SIRSchedGraph *operator->() const { return &G; }

  typedef SIRSchedGraph::iterator iterator;
  typedef SIRSchedGraph::const_iterator const_iterator;
  typedef SIRSchedGraph::reverse_iterator reverse_iterator;
  typedef SIRSchedGraph::const_reverse_iterator const_reverse_iterator;

  unsigned getASAPStep(const SIRSchedUnit *A) const {
    TFMapTy::const_iterator at = SUnitToTF.find(A);
    assert(at != SUnitToTF.end() && "TimeFrame for SU not exist!");
    return at->second.ASAP;
  }

  unsigned getALAPStep(const SIRSchedUnit *A) const {
    TFMapTy::const_iterator at = SUnitToTF.find(A);
    assert(at != SUnitToTF.end() && "TimeFrame for SU not exist!");
    return at->second.ALAP;
  }

  iterator begin() const { return G.begin(); }
  iterator end() const { return G.end(); }
  reverse_iterator rbegin() const { return G.rbegin(); }
  reverse_iterator rend() const { return G.rend(); }

  unsigned getEntrySlot() const { return EntrySlot; }
  void lengthenCriticalPath() { CriticalPathEnd += 1; }
  void shortenCriticalPath() { CriticalPathEnd -= 1; }

  unsigned getCriticalPathLength() {
    assert(CriticalPathEnd > EntrySlot && "CriticalPathLength not available!");
    return CriticalPathEnd - EntrySlot;
  }
  void setCriticalPathLength(float L) {
    CriticalPathEnd = EntrySlot + L;
  }

  // Build TimeFrame for all SUnits and reset the scheduling
  // graph. The return value is the CriticalPathEnd.
  unsigned buildTimeFrameAndResetSchedule(bool reset);

  void resetTimeFrame();
  void buildTimeFrame();
};

template<> struct GraphTraits<SIRScheduleBase *>
: public GraphTraits<SIRSchedGraph *> {

  typedef SIRSchedGraph::iterator nodes_iterator;

  static nodes_iterator nodes_begin(SIRScheduleBase *G) {
    return G->begin();
  }

  static nodes_iterator nodes_end(SIRScheduleBase *G) {
    return G->end();
  }
};

struct PriorityHeuristic {
  typedef SIRScheduleBase::TimeFrame TimeFrame;
  const SIRScheduleBase &S;

  PriorityHeuristic(const SIRScheduleBase &S) : S(S) {}

  bool operator()(const SIRSchedUnit *LHS, const SIRSchedUnit *RHS) const {
    // we consider the priority from these aspects:
    // Size Of TF, ALAP, ASAP

    TimeFrame LHSTF = S.getTimeFrame(LHS),
      RHSTF = S.getTimeFrame(RHS);
    if (LHSTF.size() < RHSTF.size()) return true;
    if (LHSTF.size() > RHSTF.size()) return false;

    // Ascending order using ALAP.
    if (LHSTF.ALAP < RHSTF.ALAP) return true;
    if (LHSTF.ALAP > RHSTF.ALAP) return false;

    // Ascending order using ASAP.
    if (LHSTF.ASAP < RHSTF.ASAP) return true;
    if (LHSTF.ASAP > RHSTF.ASAP) return false;

    // Tie breaker: Original topological order.
    return LHS->getIdx() < RHS->getIdx();
  }
};
}

#endif