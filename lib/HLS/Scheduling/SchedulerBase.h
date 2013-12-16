//===- ForceDirectedSchedulingBase.h - ForceDirected information analyze --*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
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
#ifndef VBE_FORCE_DIRECTED_INFO
#define VBE_FORCE_DIRECTED_INFO

#include "VASTScheduling.h"

#include "llvm/ADT/PriorityQueue.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallSet.h"
#include <map>

namespace llvm {
class TimingNetlist;

class SchedulerBase {
public:
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
  };

protected:
  // MII in modulo schedule.
  const unsigned EntrySlot;
  unsigned MII, CriticalPathEnd;

protected:
  // Time frames for each schedule unit.
  typedef std::map<const VASTSchedUnit*, TimeFrame> TFMapTy;
  TFMapTy SUnitToTF;

  /// @name PriorityQueue
  //{
  VASTSchedGraph &G;

  unsigned computeStepKey(unsigned step) const;
  SchedulerBase(VASTSchedGraph &G, unsigned EntrySlot)
    : EntrySlot(EntrySlot), MII(0), CriticalPathEnd(0),
      G(G) {}

  unsigned calculateASAP(const VASTSchedUnit *A);
  // Apply the Bellman-Ford like algorithm at most |V|-1 times, return true if
  // negative cycle found.
  bool buildASAPStep();

  unsigned calculateALAP(const VASTSchedUnit *A);
  void buildALAPStep();

public:
  virtual ~SchedulerBase() {}

  VASTSchedGraph &operator*() const { return G; }
  VASTSchedGraph *operator->() const { return &G; }

  virtual bool scheduleState() { return false; };

  // Return true when resource constraints preserved after citical path
  // scheduled
  typedef VASTSchedGraph::iterator iterator;
  bool scheduleCriticalPath(iterator I, iterator E);
  typedef VASTSchedGraph::const_iterator const_iterator;
  bool allNodesSchedued(const_iterator I, const_iterator E) const;

  /// @name TimeFrame
  //{

  unsigned getASAPStep(const VASTSchedUnit *A) const {
    TFMapTy::const_iterator at = SUnitToTF.find(A);
    assert(at != SUnitToTF.end() && "TimeFrame for SU not exist!");
    return at->second.ASAP;
  }
  unsigned getALAPStep(const VASTSchedUnit *A) const {
    TFMapTy::const_iterator at = SUnitToTF.find(A);
    assert(at != SUnitToTF.end() && "TimeFrame for SU not exist!");
    return at->second.ALAP;
  }

  unsigned getTimeFrame(const VASTSchedUnit *A) const {
    return getALAPStep(A) - getASAPStep(A) + 1;
  }
  //}

  unsigned computeRecMII();

  void setMII(unsigned II) { MII = II; }
  unsigned getMII() const { return MII; }
  void increaseMII() { ++MII; }
  void decreaseMII() { --MII; }
  void lengthenCriticalPath() { ++CriticalPathEnd; }
  void shortenCriticalPath() { --CriticalPathEnd; }
  unsigned getCriticalPathLength() {
    assert(CriticalPathEnd > EntrySlot && "CriticalPathLength not available!");
    return CriticalPathEnd - EntrySlot;
  }
  void setCriticalPathLength(unsigned L) {
    CriticalPathEnd = EntrySlot + L;
  }

  iterator begin() const {
    return G.begin();
  }

  iterator end() const {
    return G.end();
  }

  unsigned buildTimeFrameAndResetSchedule(bool reset);
  void resetTimeFrame();
  void buildTimeFrame();
  void printSUTimeFrame(raw_ostream &OS, const VASTSchedUnit *A) const;

  void printTimeFrame(raw_ostream &OS) const {
    OS << "Time frame:\n";
    for (iterator I = begin(), E = end(); I != E; ++I)
      printSUTimeFrame(OS, I);
  }

  void dumpTimeFrame() const;

  bool scheduleCriticalPath() {
    buildTimeFrameAndResetSchedule(true);
    return scheduleCriticalPath(G.begin(), G.end());
  }

  // Find the minimal II which can eliminate the negative cycles. Where we
  // detect negative cycles by applying Bellman-Ford like algorithm at most
  // |V|-1 times to see if the ASAP steps convergence.
  unsigned computeRecMII(unsigned MinRecMII);

  void viewGraph();
};

template<> struct GraphTraits<SchedulerBase*>
  : public GraphTraits<VASTSchedGraph*> {

  typedef VASTSchedGraph::iterator nodes_iterator;

  static nodes_iterator nodes_begin(SchedulerBase *G) {
    return G->begin();
  }

  static nodes_iterator nodes_end(SchedulerBase *G) {
    return G->end();
  }
};
} // End namespace.
#endif
