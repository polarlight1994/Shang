//===--------- SIRSDCScheduler.h ------- SDCScheduler -----------*- C++ -*-===//
//
//                          The SIR HLS framework                             //
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

#ifndef SIR_SDC_SCHEDULER_H
#define SIR_SDC_SCHEDULER_H

#include "SIRSchedulerBase.h"
#include "lp_solve/lp_types.h"

namespace llvm {
class SIRSDCScheduler : public SIRScheduleBase {
private:
  lprec *lp;

  // The map between the SUnit and the Col in LP model.
  typedef std::map<const SIRSchedUnit *, unsigned> SU2ColMapTy;
  SU2ColMapTy SU2Col;

  // The variable weight in LP model.
  std::vector<double> VarWeights;

  // Reset the SDC scheduler.
  void reset();
  // Create the LP model and corresponding Variables for the SUnit.
  unsigned createLPVariable(SIRSchedUnit *U, unsigned ColNum);
  unsigned createLPAndLPVariables();
  // Add constraints according to the dependencies.
  void addDependencyConstraints();
  // Assign weight to the object variable.
  void assignObjCoeff(SIRSchedUnit *ObjU, double weight);
  // Build the ASAP object.
  void buildASAPObj();
  // Solve the LP model and get the result.
  bool solveLP(lprec *lp);
  // Interpret the return code from the LPSolve.
  bool interpertResult(int Result);
  // Schedule the SUnits according to the result.
  bool scheduleSUs();

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
  };
  LPObjFn ObjFn;

public:
  SIRSDCScheduler(SIRSchedGraph &G, unsigned EntrySlot)
    : SIRScheduleBase(G, EntrySlot) {}

  SIRSchedGraph &operator*() const { return G; }
  SIRSchedGraph *operator->() const { return &G; }

  typedef SIRSchedGraph::iterator iterator;
  iterator begin() const { return G.begin(); }
  iterator end() const { return G.end(); }

  typedef SU2ColMapTy::const_iterator SU2ColIt;
  unsigned getSUCol(const SIRSchedUnit *U) const {
    SU2ColIt at = SU2Col.find(U);
    assert(at != SU2Col.end() && "Col not existed!");

    return at->second;
  }

  void scheduleBB(BasicBlock *BB);
  bool schedule();
};
}


#endif