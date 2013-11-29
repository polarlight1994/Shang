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

  template<typename T>
  void applyPenalty(T &Obj) const {
    for (unsigned i = 0; i < size(); ++i)
      Obj[getVarIdx(i)] += getObjCoefIdx(i);
  }

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

template<unsigned N>
struct FixedSizeLagConstraint : public LagConstraint {
  double Coeffs[N + 1];
  int VarIdx[N];

  FixedSizeLagConstraint(bool LeNotEq, ArrayRef<double> Coefficients,
                         ArrayRef<int> Indecies)
    : LagConstraint(LeNotEq, N, Coeffs, VarIdx) {
    for (unsigned i = 0; i < N; ++i) {
      Coeffs[i] = Coefficients[i];
      VarIdx[i] = Indecies[i];
    }

    Coeffs[N] = Coefficients[N];
  }
};

class LagSDCSolver {
  ilist<LagConstraint> RelaxedConstraints;
public:
  LagSDCSolver() {}

  enum ResultType {
    InFeasible,
    Feasible,
    Optimal
  };

  void addCndDep(ArrayRef<int> VarIdx);
  void addSyncDep(unsigned IdxStart, unsigned IdxEnd, unsigned RowStart);

  template<unsigned N>
  void addGenericConstraint(bool LeNotEq, ArrayRef<double> Coefficients,
                            ArrayRef<int> VarIdx) {
    LagConstraint *C
      = new FixedSizeLagConstraint<N>(LeNotEq, Coefficients, VarIdx);
    RelaxedConstraints.push_back(C);
  }

  void reset();

  typedef ilist<LagConstraint>::iterator iterator;
  iterator begin() { return RelaxedConstraints.begin(); }
  iterator end() { return RelaxedConstraints.end(); }

  // 1. Update constraints and compate violations
  ResultType update(lprec *lp, double &SubGradientSqr);

  // 2. Update Lagrangrian multipliers (subgradient method)
  template<typename T>
  void updateMultipliers(T &Obj, double StepSize) {
    for (iterator I = begin(), E = end(); I != E; ++I) {
      I->updateMultiplier(StepSize);
      I->applyPenalty<T>(Obj);
    }
  }
};

}

#endif
