//===- LagSDCSolver.h - Solve the IP with Lagrangian Relaxation -*- C++ -*-===//
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
//
// This file define the solver based on Lagrangrian Relaxation to solver the IP
// that contains differential constraints and other ``difficult'' constraints.
// Those difficult constraints are mainly introduced by the conditional
// dependencies, and (in future) 
//
//===----------------------------------------------------------------------===//

#ifndef LAG_SDC_SOLVER_H
#define LAG_SDC_SOLVER_H

#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/ArrayRef.h"

//Dirty Hack
struct _lprec;
typedef _lprec lprec;

namespace llvm {
class LagSDCSolver;

// The constraints for conditional dependencies to who Lagrangrain relaxation
// is applied. They correspond to a row in the LP model, but they are sparse
// so we only store the non-zero entries.
class LagConstraint : public ilist_node<LagConstraint> {
protected:
  const bool LeNotEq;
  const unsigned Size;
  double *const CArray;
  int *const IdxArray;
  // The value of current row, i.e. for constraint f(x) <= b, the value of
  // b - f(x). Please note that this is under the assumption that we are going
  // to maximize the object function of LP module.
  double CurValue;

  // The Lagrangrian multiplier
  double Lambda;

  friend struct ilist_sentinel_traits<LagConstraint>;
  friend class LagSDCSolver;
  LagConstraint() : LeNotEq(true), Size(0), CArray(0), IdxArray(0),
                    CurValue(0.0), Lambda(0.0) {}

  // Update the status of the constraint, return true if the constraint is preserved.
  virtual bool updateStatus(lprec *lp);

  void updateMultiplier(double StepSize);
  LagConstraint(bool LeNotEq, unsigned Size, double *CArray, int *IdxArray);
public:
  virtual ~LagConstraint() {}
  unsigned size() const { return Size; }
  unsigned getVarIdx(unsigned i) const {
    assert(i < Size && "Index out of bound!");
    return IdxArray[i];
  }

  double getObjCoefIdx(unsigned i) const {
    assert(i < Size && "Index out of bound!");
    return -Lambda * CArray[i];
  }
};

class LagSDCSolver {
  ilist<LagConstraint> RelaxedConstraints;
  // 2. Update Lagrangrian multipliers (subgradient method)
  void updateMultipliers(double StepSize);
public:

  void addCndDep(ArrayRef<int> VarIdx);
  void reset();

  typedef ilist<LagConstraint>::iterator iterator;
  iterator begin() { return RelaxedConstraints.begin(); }
  iterator end() { return RelaxedConstraints.end(); }

  // 1. Update constraints and compate violations
  bool update(lprec *lp, double StepSizeFactor);
};

}

#endif