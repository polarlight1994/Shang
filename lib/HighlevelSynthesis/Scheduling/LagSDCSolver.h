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
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/ArrayRef.h"

//Dirty Hack
struct _lprec;
typedef _lprec lprec;

namespace llvm {
class LagSDCSolver;

// The constraints for conditional dependencies to who Lagrangrain relaxation
// is applied. They correspond to a row in the LP model, but they are sparse
// so we only store the non-zero entries.
class CndDepLagConstraint : public ilist_node<CndDepLagConstraint> {
  SmallVector<double, 2> Coeffs;
  SmallVector<int, 2> VarIdx;
  // The value of current row, i.e. for constraint f(x) <= b, the value of
  // b - f(x). Please note that this is under the assumption that we are going
  // to maximize the object function of LP module.
  double CurValue;

  // The Lagrangrian multiplier
  double Lambda;

  friend struct ilist_sentinel_traits<CndDepLagConstraint>;
  friend class LagSDCSolver;
  CndDepLagConstraint() : CurValue(0.0), Lambda(0.0) {}

  double updateConfficients(lprec *lp);
  void updateMultiplier(double StepSize);
  CndDepLagConstraint(ArrayRef<int> VarIdx);
public:
  unsigned size() const { return VarIdx.size(); }
  unsigned getVarIdx(unsigned i) const { return VarIdx[i]; }
  double getObjCoefIdx(unsigned i) const {
    return - Lambda * Coeffs[i];
  }
};

class LagSDCSolver {
  ilist<CndDepLagConstraint> RelaxedConstraints;
public:

  void addCndDep(ArrayRef<int> VarIdx);
  void reset();

  typedef ilist<CndDepLagConstraint>::iterator iterator;
  iterator begin() { return RelaxedConstraints.begin(); }
  iterator end() { return RelaxedConstraints.end(); }

  // 1. Update constraints and compate violations
  double updateConstraints(lprec *lp);
  // 2. Update Lagrangrian multipliers (subgradient method)
  void updateMultipliers(double StepSize);
};

}

#endif
