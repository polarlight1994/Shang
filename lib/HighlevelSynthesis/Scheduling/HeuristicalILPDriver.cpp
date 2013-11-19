//===- HeuristicalILPDriver.cpp - High-level ILP Heuristics -----*- C++ -*-===//
//
//                      The VAST HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Heuristical ILPDriver for the scheduler to solve the
// difficult ILP model.
//
//===----------------------------------------------------------------------===//

#include "SDCScheduler.h"

#define DEBUG_TYPE "sdc-scheduler-heuristics"
#include "llvm/Support/Debug.h"

#include "lpsolve/lp_lib.h"

using namespace llvm;

bool SDCScheduler::fixCndDepSlack() {
  unsigned TotalRows = get_Norig_rows(lp);
  bool AnyFix = false;

  typedef std::vector<VASTSchedUnit*>::iterator iterator;
  for (iterator I = ConditionalSUs.begin(), E = ConditionalSUs.end();
       I != E; ++I) {
    VASTSchedUnit *SU = *I;
    unsigned SlackIdxStart = getSUIdx(SU) + 1;
    AnyFix |= fixCndDepSlack(SU, SlackIdxStart, TotalRows);
  }

  return AnyFix;
}

bool SDCScheduler::fixCndDepSlack(VASTSchedUnit *SU, unsigned SlackIdx,
                                  unsigned TotalRows) {
  dbgs() << "Update conditional slack for BB: " << SU->getParent()->getName()
         << '\n';
  REAL SmallestSlack = get_infinite(lp);
  unsigned SmallestSlackIdx = 0;

  typedef VASTSchedUnit::dep_iterator dep_iterator;
  for (dep_iterator I = SU->dep_begin(), E = SU->dep_end(); I != E; ++I) {
    assert(I.getEdgeType() == VASTDep::Conditional && "Unexpected edge type!");
    VASTSchedUnit *Dep = *I;

    unsigned SlackResultIdx = TotalRows + SlackIdx;
    REAL Slack = get_var_primalresult(lp, SlackResultIdx);

    dbgs().indent(2) << "From BB: " << Dep->getParent()->getName() << ' '
                     << Slack << " (" << unsigned(Slack) << ", Idx " << SlackIdx
                     << ", " << get_col_name(lp, SlackIdx) << ")\n";

    if (Dep->getIdx() > SU->getIdx()) {
      dbgs().indent(4) << "Ignore the potential backedge!\n";
      continue;
    }

    if (Slack < SmallestSlack) {
      SmallestSlack = Slack;
      SmallestSlackIdx = SlackIdx;
    }

    ++SlackIdx;
  }

  assert(SmallestSlackIdx && "SmallestSlackIdx not updated in the loop?");

  if (SmallestSlack > 0.0) {
    dbgs() << "Going to fix slack variable " << SmallestSlackIdx << ", "
           << get_col_name(lp, SmallestSlackIdx)
           << "\n\tcurrent slack value: " << SmallestSlack << '\n';
  }

  set_upbo(lp, SmallestSlackIdx, 0.0);

  dbgs() << '\n';

  return SmallestSlack > 0.0;
}

bool SDCScheduler::updateModelHeuristically(lprec *lp) {
  set_break_at_value(lp, get_var_primalresult(lp, 0));
  return fixCndDepSlack();
}

bool SDCScheduler::solveLPHeuristically(lprec *lp) {
  set_break_at_first(lp, TRUE);

  do {
    if (!solveLP(lp, false))
      return false;

    set_break_at_first(lp, FALSE);
  } while (updateModelHeuristically(lp));

  return true;
}
