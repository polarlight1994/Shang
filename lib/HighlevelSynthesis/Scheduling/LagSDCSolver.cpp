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

CndDepLagConstraint::CndDepLagConstraint(ArrayRef<int> VarIdx)
  : Coeffs(VarIdx.size(), 0), VarIdx(VarIdx.begin(), VarIdx.end()),
    CurValue(0.0), Lambda(0.0) {}

static REAL ProductExcluding(ArrayRef<REAL> A, unsigned Idx) {
  REAL P = 1.0;

  for (unsigned i = 0; i < A.size(); ++i)
    if (i != Idx)
      P *= A[i];

  return P;
}

// For conditional dependencies, we require one of the slack must be zero,
// i.e. product(Slack_{i}) <= 0.
double CndDepLagConstraint::updateConfficients(lprec *lp) {
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

  CurValue = - ProductExcluding(Slacks, -1);

  DEBUG(dbgs() << "Violation of current Lagrangian constraint: "
               << - CurValue << '\n');

  for (unsigned i = 0, e = VarIdx.size(); i != e; ++i) {
    // Calculate the partial derivative of product(Slack_{k}) on Slack_{k}.
    REAL PD = ProductExcluding(Slacks, i);
    // Penalty the voilating slack.
    Coeffs[i] = PD;
    DEBUG(dbgs().indent(2) << "Idx " << VarIdx[i] << ", CurSlack " << Slacks[i]
                     << " pd " << PD << '\n');
  }

  DEBUG(dbgs() << '\n');

  return CurValue;
}

void CndDepLagConstraint::updateMultiplier(double StepSize) {
  Lambda = std::max<double>(0.0, Lambda - CurValue * StepSize);
  // Lambda = Lambda - CurValue * StepSize;
}

double LagSDCSolver::updateConstraints(lprec *lp) {
  double SubGradiantLengthSqr = 0.0;

  for (iterator I = begin(), E = end(); I != E; ++I) {
    CndDepLagConstraint *C = I;
    // Calculate the partial derivative of for the lagrangian multiplier of the
    // current constraint.
    double PD = C->updateConfficients(lp);
    SubGradiantLengthSqr += PD * PD;
  }

  return SubGradiantLengthSqr;
}

void LagSDCSolver::updateMultipliers(double StepSize) {
  // Calculate the step size.
  for (iterator I = begin(), E = end(); I != E; ++I)
    I->updateMultiplier(StepSize);
}

void LagSDCSolver::reset() {
  RelaxedConstraints.clear();
}

void LagSDCSolver::addCndDep(ArrayRef<int> VarIdx) {
  RelaxedConstraints.push_back(new CndDepLagConstraint(VarIdx));
}
