//==- LagSDCSolver.cpp - Solve the IP with Lagrangian Relaxation -*- C++ -*-==//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the scheduler based on the System of Differential
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

#include "SDCScheduler.h"
#include "LagSDCSolver.h"

#include "llvm/Analysis/Dominators.h"
#define DEBUG_TYPE "sdc-scheduler-heuristics"
#include "llvm/Support/Debug.h"

#include "lpsolve/lp_lib.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
LagConstraint::LagConstraint(bool LeNotEq, ArrayRef<double> C, ArrayRef<int> V)
  : LeNotEq(LeNotEq), C(C), V(V), CurValue(0.0), Lambda(0.0) {}

bool LagConstraint::updateStatus(lprec *lp) {
  unsigned TotalRows = get_Norig_rows(lp);
  double RowVal = 0;

  // Calculate Ax
  for (unsigned i = 0; i < V.size(); ++i) {
    unsigned Idx = V[i];
    unsigned ResultIdx = TotalRows + Idx;
    REAL Val = get_var_primalresult(lp, ResultIdx);
    RowVal += C[i] * Val;
  }

  CurValue = C[V.size()] - RowVal;
  // For
  return LeNotEq ? CurValue >= 0.0 : CurValue == 0.0;
}

void LagConstraint::updateMultiplier(double StepSize) {
  double NewVal = Lambda - CurValue * StepSize;

  if (LeNotEq)
    NewVal = std::max<double>(0.0, NewVal);

  Lambda = NewVal;
}

namespace {
struct CndDepLagConstraint : public LagConstraint {
  SmallVector<double, 2> Coeffs;
  SmallVector<int, 2> VarIdx;

  // friend struct ilist_sentinel_traits<CndDepLagConstraint>;
  // friend class LagSDCSolver;

  bool updateStatus(lprec *lp);
  CndDepLagConstraint(ArrayRef<int> VarIdx);
};
}

CndDepLagConstraint::CndDepLagConstraint(ArrayRef<int> VarIdx)
  : LagConstraint(true), Coeffs(VarIdx.size(), 0),
    VarIdx(VarIdx.begin(), VarIdx.end()) {
  V = this->VarIdx;
  C = Coeffs;
}

static REAL ProductExcluding(ArrayRef<REAL> A, unsigned Idx) {
  REAL P = 1.0;

  for (unsigned i = 0; i < A.size(); ++i)
    if (i != Idx)
      P *= A[i];

  return P;
}

// For conditional dependencies, we require one of the slack must be zero,
// i.e. geomean(Slack_{i}) <= 0.
bool CndDepLagConstraint::updateStatus(lprec *lp) {
  unsigned TotalRows = get_Norig_rows(lp);
  SmallVector<REAL, 2> Slacks;

  for (unsigned i = 0; i < VarIdx.size(); ++i) {
    unsigned SlackIdx = VarIdx[i];
    unsigned SlackResultIdx = TotalRows + SlackIdx;
    REAL Slack = get_var_primalresult(lp, SlackResultIdx);
    Slacks.push_back(Slack);

    DEBUG(dbgs().indent(2) << get_col_name(lp, SlackIdx) << ", Idx " << SlackIdx
                           << " (" << unsigned(Slack) << ")\n";);
  }

  DEBUG(dbgs() << '\n');

  double RowValue = ProductExcluding(Slacks, -1);
  double exponent = 1.0 / Slacks.size();
  if (RowValue != 0.0)
    RowValue = pow(RowValue, exponent);

  DEBUG(dbgs() << "Violation of current Lagrangian constraint: "
               << -RowValue << '\n');

  for (unsigned i = 0, e = VarIdx.size(); i != e; ++i) {
    // Calculate the partial derivative of geomean(Slack_{k}) on Slack_{k}.
    REAL PD = ProductExcluding(Slacks, i);
    PD = std::max<double>(PD, 1e-4);
    PD = pow(PD, exponent);
    double CurSlack = Slacks[i];
    if (CurSlack != 0)
      PD *= pow(CurSlack, exponent);
    PD *= exponent;

    // Penalty the voilating slack.
    Coeffs[i] = PD;
    DEBUG(dbgs().indent(2) << "Idx " << VarIdx[i] << ", CurSlack " << Slacks[i]
                           << " pd " << PD << '\n');
  }

  DEBUG(dbgs() << '\n');

  // Calculate b - A
  CurValue = 0.0 - RowValue;

  // The constraint is preserved if the row value is zero.
  return CurValue == 0.0;
}

bool LagSDCSolver::update(lprec *lp, double StepSizeFactor) {
  unsigned Violations = 0;
  double SubGradientSqr = 0.0;

  for (iterator I = begin(), E = end(); I != E; ++I) {
    LagConstraint *C = I;
    if (!C->updateStatus(lp))
      ++Violations;

    // Calculate the partial derivative of for the lagrangian multiplier of the
    // current constraint.
    double PD = C->CurValue;
    SubGradientSqr += PD * PD;
  }

  dbgs() << "Violations: " << Violations << " SGL: " << SubGradientSqr << "\n";

  // Calculate the stepsize, based on:
  // An Applications Oriented Guide to Lagrangian Relaxation
  // by ML Fisher, 1985
  double StepSize = StepSizeFactor / SubGradientSqr;

  // Calculate the step size.
  for (iterator I = begin(), E = end(); I != E; ++I)
    I->updateMultiplier(StepSize);

  return Violations == 0;
}

void LagSDCSolver::reset() {
  RelaxedConstraints.clear();
}

void LagSDCSolver::addCndDep(ArrayRef<int> VarIdx) {
  RelaxedConstraints.push_back(new CndDepLagConstraint(VarIdx));
}
