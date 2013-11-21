//===- LagSDCDriver.cpp - Solve SDC with Lagrangian relaxation --*- C++ -*-===//
//
//                      The VAST HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement Lagrangian relaxation based algorithm to solve the extend
// ILP model for the SDC scheduler.
//
//===----------------------------------------------------------------------===//

#include "SDCScheduler.h"

#include "llvm/Analysis/Dominators.h"
#define DEBUG_TYPE "sdc-scheduler-heuristics"
#include "llvm/Support/Debug.h"

#include "lpsolve/lp_lib.h"

using namespace llvm;

unsigned SDCScheduler::updateCndDepLagMultipliers() {
  unsigned TotalRows = get_Norig_rows(lp);
  unsigned Violations = 0;

  typedef std::vector<VASTSchedUnit*>::iterator iterator;
  for (iterator I = ConditionalSUs.begin(), E = ConditionalSUs.end();
       I != E; ++I) {
    VASTSchedUnit *SU = *I;
    unsigned SlackIdxStart = getSUIdx(SU) + 1;
    if (updateCndDepLagMultipliers(SU, SlackIdxStart, TotalRows))
      ++Violations;
  }

  return Violations;
}

static REAL ProductExcluding(ArrayRef<REAL> A, unsigned Idx) {
  REAL P = 1.0;

  for (unsigned i = 0; i < A.size(); ++i)
    if (i != Idx)
      P *= A[i];

  return P;
}

bool
SDCScheduler::updateCndDepLagMultipliers(VASTSchedUnit *SU, unsigned SlackIdx,
                                         unsigned TotalRows) {
  BasicBlock *CurBB = SU->getParent();
  DEBUG(dbgs() << "Update conditional slack for BB: " << CurBB->getName()
               << '\n');

  // For conditional dependencies, we require one of the slack must be zer,
  // i.e. product(Slack_{i}) = 0.

  SmallVector<REAL, 4> Slacks;
  SmallVector<int, 4> Indices;
  bool AnyZero = false;

  typedef VASTSchedUnit::dep_iterator dep_iterator;
  for (dep_iterator I = SU->dep_begin(), E = SU->dep_end(); I != E; ++I) {
    assert(I.getEdgeType() == VASTDep::Conditional && "Unexpected edge type!");
    VASTSchedUnit *Dep = *I;

    unsigned SlackResultIdx = TotalRows + SlackIdx;
    REAL Slack = get_var_primalresult(lp, SlackResultIdx);

    DEBUG(dbgs().indent(2);
          dbgs() << "From BB: " << Dep->getParent()->getName() << ' '
                 << Slack << " (" << unsigned(Slack) << ", Idx " << SlackIdx
                 << ", " << get_col_name(lp, SlackIdx) << ")\n";);

    if (DT.dominates(CurBB, Dep->getParent())) {
      DEBUG(dbgs().indent(4) << "Ignore the potential backedge!\n");
      ++SlackIdx;
      continue;
    }

    Slacks.push_back(Slack);
    Indices.push_back(SlackIdx);
    AnyZero |= (Slack == 0.0);

    ++SlackIdx;
  }

  DEBUG(dbgs() << '\n');

  if (AnyZero)
    return false;

  dbgs() << "Going to penalize conditional slack for BB: " << CurBB->getName()
         << '\n';

  for (unsigned i = 0, e = Slacks.size(); i != e; ++i) {
    // Calculate the partial derivative of product(Slack_{k}) on Slack_{k}.
    REAL PD = ProductExcluding(Slacks, i);
    // Penalty the voilating slack.
    LagObjFn[Indices[i]] -= PD;
    dbgs().indent(2) << "Idx " << Indices[i] << ", CurSlack " << Slacks[i]
                     << " pd " << PD << '\n';
  }

  dbgs() << '\n';

  return true;
}

unsigned SDCScheduler::updateLagMultipliers(lprec *lp) {
  set_break_at_value(lp, get_var_primalresult(lp, 0));
  return updateCndDepLagMultipliers();
}

bool SDCScheduler::lagSolveLP(lprec *lp) {
  set_break_at_first(lp, TRUE);

  unsigned Voilations = 0;
  LPObjFn Obj;

  // Create
  do {
    Obj = ObjFn;
    Obj += LagObjFn;

    Obj.setLPObj(lp);

    if (!solveLP(lp, false))
      return false;

    LagObjFn *= 0.8;

    set_break_at_first(lp, FALSE);

    unsigned LastVoilations = Voilations;
    Voilations = updateLagMultipliers(lp);
    dbgs() << "LastVoilation: " << LastVoilations
           << " CurVoilation: " << Voilations << '\n';
  } while (Voilations);

  return true;
}
