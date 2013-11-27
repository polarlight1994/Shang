//===------------- SDCScheduler.h - The SDC Scheduler -----------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the scheduler based on the System of Differential
// Constraints
//
//===----------------------------------------------------------------------===//

#ifndef SDC_SCHEDULER_H
#define SDC_SCHEDULER_H

#include "SchedulerBase.h"

//Dirty Hack
struct _lprec;
typedef _lprec lprec;

namespace llvm {
class DominatorTree;
struct PostDominatorTree;

class LagSDCSolver;

class SDCScheduler : public SchedulerBase {
public:
  struct SoftConstraint {
    double Penalty;
    unsigned SlackIdx;
    unsigned C;
    unsigned LastValue;

    SoftConstraint() : Penalty(0.0), SlackIdx(0), C(0), LastValue(0) {}
  };

  typedef VASTSchedGraph::iterator iterator;
  // Set the variables' name in the model.
  void addSoftConstraint(VASTSchedUnit *Src, VASTSchedUnit *Dst, unsigned C,
                         double Penalty);
  SoftConstraint &getOrCreateSoftConstraint(VASTSchedUnit *Src,
                                            VASTSchedUnit *Dst);

  double getLastPenalty(VASTSchedUnit *Src, VASTSchedUnit *Dst) const;


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
  void addLinOrdEdge(PostDominatorTree &PDT,
                     std::map<Value*, SmallVector<VASTSchedUnit*, 4> >
                     &IR2SUMap);

  void initalizeCFGEdges();
private:
  lprec *lp;
  LagSDCSolver *LagSolver;

  DominatorTree &DT;
  LoopInfo &LI;

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

    double evaluateCurValue(lprec *lp) const;

    void dump() const;
  };

  LPObjFn ObjFn;

  // The table of the index of the VSUnits and the column number in LP.
  typedef std::map<const VASTSchedUnit*, unsigned> SUI2IdxMapTy;
  typedef SUI2IdxMapTy::const_iterator SUIdxIt;
  SUI2IdxMapTy SUIdx;
  // The variable weight for 'set_var_weights' function of lpsolve.
  std::vector<double> LPVarWeights;

  typedef std::pair<VASTSchedUnit*, VASTSchedUnit*> EdgeType;
  typedef std::map<EdgeType, SoftConstraint> SoftCstrVecTy;
  SoftCstrVecTy SoftConstraints;

  // The scheduling unit with conditional dependencies.
  std::vector<VASTSchedUnit*> ConditionalSUs, SynchronizeSUs;

  std::map<BasicBlock*, std::set<VASTSchedUnit*> > CFGEdges;
  void preserveAntiDependence(VASTSchedUnit *Src, VASTSchedUnit *Dst);
  void preserveAntiDependence(VASTSchedUnit *Src, VASTSchedUnit *Dst, Loop *L,
                              std::set<VASTSchedUnit*> &Exits);

  // Build constraints -Slack + BigM * AuxVar >= 0 and
  // Sum (AuxVar) <= Number of Slack - 1, where AuxVar is either 0 or 1.
  // With these constraints, we specify that at least one of the slack must be
  // 0
  void addConditionalConstraints(VASTSchedUnit *SU);
  void addConditionalConstraints();

  void addSynchronizeConstraints(VASTSchedUnit *SU);
  void addSynchronizeConstraints();

  // Create step variables, which represent the c-step that the VSUnits are
  // scheduled to.
  unsigned createStepVariable(const VASTSchedUnit *U, unsigned Col);
  unsigned createSlackVariable(unsigned Col, int UB, int LB,
                               const Twine &Name);
  unsigned createVarForCndDeps(unsigned Col);

  unsigned updateSoftConstraintPenalties();
  bool solveLP(lprec *lp, bool PreSolve);

  //
  bool resolveControlChainingHazard();

  // Solve the scheduling LP with Augmented Lagrangian Methods.
  bool lagSolveLP(lprec *lp);

  // Interpert the return code from lpsolve, translate it to true if a solution,
  // which maybe suboptimal, is found, false otherwise.
  bool interpertResult(int Result);

  // Build the schedule form the result of ILP.
  unsigned buildSchedule(lprec *lp);

  // The schedule should satisfy the dependences.
  void addDependencyConstraints(lprec *lp);

  void nameLastRow(const Twine &NamePrefix);
  void dumpModel() const;
  void reset();

  void addDependencyConstraints();
  void addSoftConstraints();
public:
  SDCScheduler(VASTSchedGraph &G, unsigned EntrySlot, DominatorTree &DT,
               LoopInfo &LI);
  ~SDCScheduler();

  unsigned createLPAndVariables();
  void addDifferentialConstraint(VASTSchedUnit *Dst, VASTSchedUnit *Src,
                                 int Ty, int RHS);

  // Build the schedule object function.
  void buildASAPObject(double weight);
  void buildOptSlackObject(double weight);

  bool schedule();

  void printVerision() const;
};

}


#endif
