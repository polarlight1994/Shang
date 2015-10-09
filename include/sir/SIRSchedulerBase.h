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
  // MII in modulo schedule.
  unsigned MII;
  unsigned CriticalPathEnd;

  typedef std::map<const SIRSchedUnit *, TimeFrame> TFMapTy;
  TFMapTy SUnitToTF;

  SIRSchedGraph &G;

public:
  SIRScheduleBase(SIRSchedGraph &G, unsigned EntrySlot)
    : EntrySlot(EntrySlot), MII(0), CriticalPathEnd(0), G(G) {}

  // Forward some functions from SIRScheGraph.
  ArrayRef<SIRSchedUnit *> lookupSUs(Value *V) {
    return G.lookupSUs(V);
  }
  ArrayRef<SIRSchedUnit *> getSUsInBB(BasicBlock *BB) {
    return G.getSUsInBB(BB);
  }

  unsigned calculateASAP(const SIRSchedUnit *A) const;
  unsigned calculateALAP(const SIRSchedUnit *A) const;
  TimeFrame calculateTimeFrame(const SIRSchedUnit *A) const {
    return TimeFrame(calculateASAP(A), calculateALAP(A));
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
  unsigned getMII() const { return MII; }
  void setMII(unsigned II) { MII = II; }
  void increaseMII() { ++MII; }
  void decreaseMII() { --MII; }
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
}

#endif