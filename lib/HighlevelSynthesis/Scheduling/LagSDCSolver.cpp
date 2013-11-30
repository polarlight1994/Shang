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
LagConstraint::LagConstraint(bool LeNotEq, unsigned Size, double *CArray,
                             int *IdxArray)
  : LeNotEq(LeNotEq), Size(Size), CArray(CArray), IdxArray(IdxArray),
    CurValue(0.0), Lambda(0.0) {}

bool LagConstraint::updateStatus(lprec *lp) {
  unsigned TotalRows = get_Norig_rows(lp);
  double RowVal = 0.0;

  // Calculate Ax
  for (unsigned i = 0; i < Size; ++i) {
    unsigned Idx = IdxArray[i];
    unsigned ResultIdx = TotalRows + Idx;
    REAL Val = get_var_primalresult(lp, ResultIdx);
    RowVal += CArray[i] * Val;
  }

  CurValue = CArray[Size] - RowVal;
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
template<unsigned N>
struct SmallConstraint : public LagConstraint {
  static const unsigned SmallSize = N;
  double SmallCoeffs[SmallSize + 1];
  int SmallVarIdx[SmallSize];

  // CreateIfNotSmallWithInitialize
  template<unsigned SmallSize, typename T>
  static T *Create(unsigned Size, T *SmallStorage, ArrayRef<T> InitVals) {
    T *Ptr = SmallStorage;
    if (Size > SmallSize)
      Ptr = new T[Size];

    bool HasInitVals = InitVals.empty();

    for (unsigned i = 0; i < Size; ++i)
      Ptr[i] = HasInitVals ? T() : InitVals[i];

    return Ptr;
  }

  SmallConstraint(bool LeNotEq, ArrayRef<double> Coeffs, ArrayRef<int> VarIdx)
    : LagConstraint(LeNotEq, VarIdx.size(),
                    Create<SmallSize + 1>(VarIdx.size() + 1, SmallCoeffs, Coeffs),
                    Create<SmallSize>(VarIdx.size(), SmallVarIdx, VarIdx)) {}

  ~SmallConstraint() {
    if (Size > SmallSize) {
      delete CArray;
      delete IdxArray;
    }
  }
};

struct CndDepLagConstraint : public SmallConstraint<4> {

  // friend struct ilist_sentinel_traits<CndDepLagConstraint>;
  // friend class LagSDCSolver;

  bool updateStatus(lprec *lp);

  CndDepLagConstraint(ArrayRef<int> VarIdx)
    : SmallConstraint(true, None, VarIdx) {}
};
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

  for (unsigned i = 0; i < Size; ++i) {
    unsigned SlackIdx = IdxArray[i];
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

  for (unsigned i = 0, e = Size; i != e; ++i) {
    // Calculate the partial derivative of geomean(Slack_{k}) on Slack_{k}.
    REAL PD = ProductExcluding(Slacks, i);
    PD = std::max<double>(PD, 1e-4);
    PD = pow(PD, exponent);
    double CurSlack = Slacks[i];
    if (CurSlack != 0)
      PD *= pow(CurSlack, exponent);
    PD *= exponent;

    // Penalty the voilating slack.
    CArray[i] = PD;
    DEBUG(dbgs().indent(2) << "Idx " << IdxArray[i]
                           << ", CurSlack " << Slacks[i]
                           << " pd " << PD << '\n');
  }

  DEBUG(dbgs() << '\n');

  // Calculate b - A
  CurValue = 0.0 - RowValue;

  // The constraint is preserved if the row value is zero.
  return CurValue == 0.0;
}

namespace {
struct SyncDepLagConstraint : public SmallConstraint<4> {
  const unsigned RowStart;
  // The weight vector to allow us to move the target offset toward feasiable
  // region.
  SmallVector<double, SmallSize> Weights;

  SyncDepLagConstraint(unsigned RowStart,
                       ArrayRef<double> Coeffs, ArrayRef<int> VarIdx)
    : SmallConstraint(true, Coeffs, VarIdx), RowStart(RowStart),
      Weights(VarIdx.size() / 2, 1.0) {}

  bool updateStatus(lprec *lp);
};
}

// For synchronization dependencies, we require all the slacks to have an
// indentical value.
bool SyncDepLagConstraint::updateStatus(lprec *lp) {
  unsigned TotalRows = get_Norig_rows(lp);

  SmallVector<int, 4> Offsets;
  REAL OffsetSum = 0.0, WeightSum = 0.0;

  for (unsigned i = 0; i < Size; i += 2) {
    unsigned PosSlackIdx = IdxArray[i];
    unsigned PosSlackResultIdx = TotalRows + PosSlackIdx;
    REAL PosSlack = get_var_primalresult(lp, PosSlackResultIdx);
    DEBUG(dbgs().indent(4) << get_col_name(lp, PosSlackIdx)
                           << ", Idx " << PosSlackIdx
                           << " (" << unsigned(PosSlack) << ")\n");

    unsigned NegSlackIdx = IdxArray[i + 1];
    unsigned NegSlackResultIdx = TotalRows + NegSlackIdx;
    REAL NegSlack = get_var_primalresult(lp, NegSlackResultIdx);
    DEBUG(dbgs().indent(4) << get_col_name(lp, NegSlackIdx)
                           << ", Idx " << NegSlackIdx
                           << " (" << unsigned(NegSlack) << ")\n");

    assert(PosSlack * NegSlack == 0 &&
           "Unexpected both positive and negative to be nonzero at the same time!");
    REAL Slack = PosSlack - NegSlack;
    // lpsolve introduce error to rh when it perform scaling, try to fix that
    // error.
    REAL RoundedRH = ceil(get_rh(lp, RowStart + i) - get_epsb(lp));
    REAL TranslatedSlack = RoundedRH + Slack;
    assert(get_rh(lp, RowStart + i) == get_rh(lp, RowStart + i + 1)
           && "Bad RH of slack constraint!");
    Offsets.push_back(TranslatedSlack);
    double CurWeight = Weights[i / 2];
    OffsetSum += TranslatedSlack * CurWeight;
    WeightSum += CurWeight;
    unsigned CurRowNum = RowStart + i;
    DEBUG(dbgs().indent(2) << "Offset: " << int(TranslatedSlack)
                           << " ("<< int(Slack) << ")\n");
  }

  // Calculate the weighted average offset, this average offset will move toward
  // the offset of the edge on which the constraint is hard to be preserved
  REAL AverageOffset = OffsetSum / WeightSum;
  int TargetOffset = ceil(AverageOffset - 0.5);
  DEBUG(dbgs() << "Average slack: " << AverageOffset << " ("
               << TargetOffset << ")\n");

  bool AllSlackIdentical = true;
  REAL RowValue = 0.0;

  for (unsigned i = 0; i < Size; i += 2) {
    REAL Violation = int(Offsets[i / 2]) - int(TargetOffset);
    RowValue += Violation * Violation;
    AllSlackIdentical &= (Violation == 0);
    // Apply different penalty to different slack variables, 0.1 is added to
    // avoid zero penalty.
    Weights[i / 2] += abs(Violation);

    unsigned PosRowNum = RowStart + i;
    DEBUG(dbgs().indent(2) << "Going to change RHS of constraint: "
                           << get_row_name(lp, PosRowNum) << " ("
                           << get_col_name(lp, IdxArray[i]) << ") from "
                           << get_rh(lp, PosRowNum) << " to "
                           << TargetOffset << '\n');
    set_rh(lp, PosRowNum, TargetOffset);

    unsigned NegRowNum = RowStart + i + 1;
    DEBUG(dbgs().indent(2) << "Going to change RHS of constraint: "
                           << get_row_name(lp, NegRowNum) << " ("
                           << get_col_name(lp, IdxArray[i + 1]) << ") from "
                           << get_rh(lp, NegRowNum) << " to "
                           << TargetOffset << '\n');
    set_rh(lp, NegRowNum, TargetOffset);
  }

  DEBUG(dbgs() << '\n');

  // Calculate b - A
  CurValue = 0.0 - sqrt(RowValue);

  // The constraint is preserved if the row value is zero.
  return AllSlackIdentical;
}

LagSDCSolver::ResultType
LagSDCSolver::update(lprec *lp, double &SubGradientSqr) {
  SubGradientSqr = 0.0;
  ResultType Result = LagSDCSolver::InFeasible;
  unsigned Violations = 0;

  for (iterator I = begin(), E = end(); I != E; ++I) {
    LagConstraint *C = I;
    if (!C->updateStatus(lp))
      ++Violations;

    // Calculate the partial derivative of for the lagrangian multiplier of the
    // current constraint.
    double PD = C->CurValue;
    SubGradientSqr += PD * PD;
  }

  DEBUG(dbgs() << "Violations: " << Violations << " in "
               << RelaxedConstraints.size()
               << " SGL: " << SubGradientSqr << "\n");

  if (Violations == 0) {
    Result = LagSDCSolver::Feasible;
    if (SubGradientSqr == 0.0)
      Result = LagSDCSolver::Optimal;
  }

  return Result;
}

void LagSDCSolver::reset() {
  RelaxedConstraints.clear();
}

void LagSDCSolver::addCndDep(ArrayRef<int> VarIdx) {
  RelaxedConstraints.push_back(new CndDepLagConstraint(VarIdx));
}

void LagSDCSolver::addSyncDep(unsigned IdxStart, unsigned IdxEnd,
                              unsigned RowStart) {
  SmallVector<int, 8> VarIdx;
  SmallVector<double, 8> Coeffs;
  for (unsigned i = IdxStart; i < IdxEnd; ++i) {
    Coeffs.push_back(1.0);
    VarIdx.push_back(i);
  }

  Coeffs.push_back(0.0);

  RelaxedConstraints.push_back(new SyncDepLagConstraint(RowStart,
                                                        Coeffs, VarIdx));
}
