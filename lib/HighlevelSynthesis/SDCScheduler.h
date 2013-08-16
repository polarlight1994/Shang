//===---------- SDCScheduler.h ------- SDCScheduler -------------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the scheduler based on the System of Difference
// Constraints formation.
//
//===----------------------------------------------------------------------===//

#ifndef SDC_SCHEDULER_H
#define SDC_SCHEDULER_H

#include "SchedulerBase.h"

namespace llvm {
class DominatorTree;

class SDCScheduler : public SchedulerBase {
  struct SoftConstraint {
    double Penalty;
    unsigned SlackIdx;
    unsigned C;
    unsigned LastValue;

    SoftConstraint() : Penalty(0.0), SlackIdx(0), C(0), LastValue(0) {}
  };
public:

  typedef VASTSchedGraph::iterator iterator;
  // Set the variables' name in the model.
  unsigned createLPAndVariables(iterator I, iterator E);
  void addSoftConstraint(VASTSchedUnit *Src, VASTSchedUnit *Dst, unsigned C,
                         double Penalty);

  double getLastPenalty(VASTSchedUnit *Src, VASTSchedUnit *Dst) const;

  void addSoftConstraints();

  void addObjectCoeff(const VASTSchedUnit *U, double Value) {
    // Ignore the constants.
    if (U->isScheduled()) return;

    ObjFn[getSUIdx(U)] += Value;
  }

  unsigned getSUIdx(const VASTSchedUnit* U) const {
    SUIdxIt at = SUIdx.find(U);
    assert(at != SUIdx.end() && "Idx not existed!");
    return at->second;
  }

  /// Add linear order edges to resolve resource conflict.
  //
  void addLinOrdEdge(DominatorTree &DT,
                     std::map<Value*, SmallVector<VASTSchedUnit*, 4> >
                     &IR2SUMap);

private:
  lprec *lp;

  // Helper class to build the object function for lp.
  struct LPObjFn : public std::map<unsigned, double> {
    LPObjFn &operator*=(double val) {
      for (iterator I = begin(), E = end(); I != E; ++I)
        I->second *= val;

      return *this;
    }

    LPObjFn &operator+=(const LPObjFn &Other) {
      for (const_iterator I = Other.begin(), E = Other.end(); I != E; ++I)
        (*this)[I->first] += I->second;

      return *this;
    }

    void setLPObj(lprec *lp) const;

    void dump() const;
  };

  LPObjFn ObjFn;

  // The table of the index of the VSUnits and the column number in LP.
  typedef std::map<const VASTSchedUnit*, unsigned> SUI2IdxMapTy;
  typedef SUI2IdxMapTy::const_iterator SUIdxIt;
  SUI2IdxMapTy SUIdx;

  typedef std::pair<VASTSchedUnit*, VASTSchedUnit*> EdgeType;
  typedef std::map<EdgeType, SoftConstraint> SoftCstrVecTy;
  SoftCstrVecTy SoftConstraints;

  // Create step variables, which represent the c-step that the VSUnits are
  // scheduled to.
  unsigned createStepVariable(const VASTSchedUnit *U, unsigned Col);
  unsigned createSlackVariable(unsigned Col);

  void addSoftConstraint(lprec *lp, VASTSchedUnit *Dst, VASTSchedUnit *Src,
                         const SoftConstraint &C);
  unsigned updateSoftConstraintPenalties();
  bool solveLP(lprec *lp);

  // Build the schedule form the result of ILP.
  unsigned buildSchedule(lprec *lp, iterator I, iterator E);

  // The schedule should satisfy the dependences.
  void addDependencyConstraints(lprec *lp);

public:
  SDCScheduler(VASTSchedGraph &G, unsigned EntrySlot)
    : SchedulerBase(G, EntrySlot), lp(0) {}
  ~SDCScheduler();

  unsigned createLPAndVariables();
  void addDependencyConstraints();

  // Build the schedule object function.
  void buildASAPObject(double weight);
  void buildOptSlackObject(double weight);

  bool schedule();

  void printVerision() const;
};

}


#endif
