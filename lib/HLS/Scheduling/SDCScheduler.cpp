//===-------- SDCScheduler.cpp ------- SDCScheduler -------------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the scheduler based on the System of Difference
// Constraints formation.
//
//===----------------------------------------------------------------------===//

#include "SDCScheduler.h"
#include "LagSDCSolver.h"

#include "vast/VASTSubModules.h"
#include "vast/Utilities.h"
#include "vast/VASTSeqValue.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/STLExtras.h"
#define DEBUG_TYPE "sdc-scheduler"
#include "llvm/Support/Debug.h"

#include "lpsolve/lp_lib.h"

using namespace llvm;

static cl::opt<unsigned> BigMMultiplier("vast-ilp-big-M-multiplier",
  cl::desc("The multiplier apply to bigM in the linear model"),
  cl::init(8));

static cl::opt<bool> UseLagSolve("vast-sdc-use-lag-solve",
  cl::desc("Solve the scheduling problem with lagrangian relaxation"),
  cl::init(true));

void SDCScheduler::LPObjFn::setLPObj(lprec *lp) const {
  std::vector<int> Indices;
  std::vector<REAL> Coefficients;

  //Build the ASAP object function.
  for(const_iterator I = begin(), E = end(); I != E; ++I) {
    Indices.push_back(I->first);
    Coefficients.push_back(I->second);
  }

  set_obj_fnex(lp, size(), Coefficients.data(), Indices.data());
  set_maxim(lp);
}

double SDCScheduler::LPObjFn::evaluateCurValue(lprec *lp) const {
  unsigned TotalRows = get_Norig_rows(lp);
  double value = 0.0;

  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    unsigned Idx = TotalRows + I->first;
    REAL Val = get_var_primalresult(lp, Idx);
    value += Val * I->second;
  }

  return value;
}

unsigned SDCScheduler::createStepVariable(const VASTSchedUnit* U, unsigned Col) {
  // Set up the step variable for the VASTSchedUnit.
  bool inserted = SUIdx.insert(std::make_pair(U, Col)).second;
  assert(inserted && "Index already existed!");
  (void) inserted;
  add_columnex(lp, 0, 0, 0);

#ifdef XDEBUG
  std::string SVName = "sv" + utostr(U->getIdx());
  set_col_name(lp, Col, const_cast<char*>(SVName.c_str()));
#endif

  set_int(lp, Col, TRUE);
  if (U->isScheduled()) {
    set_lowbo(lp, Col, U->getSchedule());
    set_upbo(lp, Col, U->getSchedule());
  } else
    set_lowbo(lp, Col, EntrySlot);

  REAL BigM = BigMMultiplier * getCriticalPathLength();
  set_upbo(lp, Col, BigM + EntrySlot );

  // The step variables are almost only used in differential constraints,
  // their are relatively easier to be solved. So we assign a lower priority
  // to these variables, so that lpsolve can focus on those ``difficult''
  // constraints first.
  LPVarWeights.push_back(256.0);
  return Col + 1;
}

unsigned SDCScheduler::createSlackVariable(unsigned Col, int UB, int LB,
                                           const Twine &Name) {
  add_columnex(lp, 0, 0, 0);

#ifdef XDEBUG
  std::string SlackName = Name.str();
  set_col_name(lp, Col, const_cast<char*>(SlackName.c_str()));
#endif

  set_int(lp, Col, TRUE);
  if (UB != LB) {
    set_upbo(lp, Col, UB);
    set_lowbo(lp, Col, LB);
  }

  return Col + 1;
}

unsigned SDCScheduler::createVarForCndDeps(unsigned Col) {
  if (LagSolver) {
    // Export the slack for the conditional edges, and we will fix these slacks
    // in the heuristical ILP driver.
    Col = createSlackVariable(Col, 0, 0, "cnd_slack" + utostr(Col));
    // These variables are even not required to be integers.
    // set_int(lp, Col - 1, FALSE);
    // This slack are not imprtant at all.
    LPVarWeights.push_back(512.0);
  } else {
    // The auxiliary variable to specify one of the conditional dependence
    // and the current SU must have the same scheduling.
    Col = createSlackVariable(Col, 1, 0, "cnd_connect" + utostr(Col));
    // These variable have the lowest variable weight for the branch and bound
    // process. Because the related constraints are the hardest ones to be
    // preserved. Hence we want to choose to make these variables integer
    // first.
    LPVarWeights.push_back(0.0);
  }

  return Col;
}

unsigned SDCScheduler::createVarForSyncDeps(unsigned Col) {
  if (LagSolver == NULL)
    return Col;

  // Export the slack for the synchronization edges, and we will fix these
  // slacks in the heuristical ILP driver.
  ObjFn[Col] = -1e-6;
  Col = createSlackVariable(Col, 0, 0, "sync_slack_pos" + utostr(Col));
  // This slack are not imprtant at all.
  LPVarWeights.push_back(512.0);

  ObjFn[Col] = -1e-6;
  Col = createSlackVariable(Col, 0, 0, "sync_slack_neg" + utostr(Col));
  LPVarWeights.push_back(512.0);

  return Col;
}

// The time function used by lpsolve.
static int __WINAPI sdc_abort(lprec *lp, void *userhandle) {
  static unsigned EarlyTimeOut = 30;
  static cl::opt<unsigned, true> X("vast-ilp-early-timeout",
    cl::location(EarlyTimeOut),
    cl::desc("The timeout value for ilp solver when it have an solution,"
             " in seconds"));

  //static unsigned BBRestart = 120;
  //static cl::opt<unsigned, true> Y("vast-ilp-bb-timeout",
  //  cl::location(BBRestart),
  //  cl::desc("The timeout value for B&B of the ilp solver, in seconds. We will"
  //           " restart the B&B process when the ilp solver spend too much time"
  //           " on B&B without finding any feasible solution"));
  // Abort the solving process if we have any solution and the elapsed time
  // is bigger than early timeout.
  if (get_solutioncount(lp) > 0) {
    if (time_elapsed(lp) > (REAL)EarlyTimeOut)
      return TRUE;
  }
  //else if (lp->bb_level > 1) {
  //  REAL TimeElapsed = timeNow() - lp->timestart;
  //  if (TimeElapsed > (REAL)BBRestart)
  //    return ACTION_RESTART;
  //}

  return FALSE;
}

unsigned SDCScheduler::createLPAndVariables() {
  lp = make_lp(0, 0);

  // Install the abort function so that we can abort the solving process early
  // when we have an solution.
  put_abortfunc(lp, sdc_abort, NULL);

  set_verbose(lp, CRITICAL);
  DEBUG(set_verbose(lp, FULL));

  unsigned Col =  1;

  for (iterator I = begin(), E = end(); I != E; ++I) {
    VASTSchedUnit* U = I;

    Col = createStepVariable(U, Col);

    bool HasCndDep = false;
    bool HasSyncDep = false;

    // Allocate slack variable and connect variable for conditional edges.
    typedef VASTSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator I = U->dep_begin(), E = U->dep_end(); I != E; ++I) {
      if (I.getEdgeType() == VASTDep::Conditional) {
        Col = createVarForCndDeps(Col);
        HasCndDep = true;
        continue;
      }

      if (I.getEdgeType() == VASTDep::Synchronize) {
        assert(((*I)->isVNode() || (*I)->isPHILatch())
                && "Unexpected dependence type for sync edge!");
        Col = createVarForSyncDeps(Col);
        HasSyncDep = true;
        continue;
      }
    }

    if (HasCndDep) {
      assert(U->isBBEntry() && "Unexpected SU type for conditional edges!");
      ConditionalSUs.push_back(U);
    } else if (HasSyncDep) {
      assert(U->isSyncJoin() && "Unexpected SU type for synchronize edges!");
      Col = createVarForSyncDeps(Col);
      SynchronizeSUs.push_back(U);
    }
  }

  typedef SoftCstrVecTy::iterator iterator;
  for (iterator I = SoftConstraints.begin(), E = SoftConstraints.end();
       I != E; ++I) {
    SoftConstraint &C = I->second;
    C.SlackIdx = Col;
    Col = createSlackVariable(Col, 0, 0, "soft_slack" + utostr(Col));

    // The slack variables of soft constraints are only used in differential
    // constraints, their are most easier to be solved and do not affect the
    // feasiblity of the model at all. So we assign a lowest priority to these
    // variables, so that lpsolve can focus on those ``difficult'' constraints
    // first.
    LPVarWeights.push_back(512.0);

    ObjFn[C.SlackIdx] = - C.Penalty;
  }

  return Col - 1;
}

SDCScheduler::SoftConstraint&
SDCScheduler::getOrCreateSoftConstraint(VASTSchedUnit *Src, VASTSchedUnit *Dst) {
  return SoftConstraints[EdgeType(Src, Dst)];
}

void SDCScheduler::addSoftConstraint(VASTSchedUnit *Src, VASTSchedUnit *Dst,
                                     unsigned C, double Penalty) {
  SoftConstraint &SC = SoftConstraints[EdgeType(Src, Dst)];
  assert(!SoftConstraints.count(EdgeType(Dst, Src)) &&
         "Unexpected conflicted soft constraint!");
  SC.Penalty = std::max<double>(SC.Penalty * 1.1, Penalty);
  if (SC.SlackIdx == 0)
    SC.LastValue = Src->getSchedule() - Dst->getSchedule() + C;
  SC.C = std::max<unsigned>(SC.C, C);
}

double SDCScheduler::getLastPenalty(VASTSchedUnit *Src,
                                    VASTSchedUnit *Dst) const {
  const SoftCstrVecTy::const_iterator I
    = SoftConstraints.find(EdgeType(Src, Dst));

  if (I == SoftConstraints.end())
    return 0.0;

  return I->second.LastValue * I->second.Penalty;
}

void SDCScheduler::addSoftConstraints() {
  typedef SoftCstrVecTy::iterator iterator;
  for (iterator I = SoftConstraints.begin(), E = SoftConstraints.end();
       I != E; ++I) {
    SoftConstraint &C = I->second;

    VASTSchedUnit *Src = I->first.first, *Dst = I->first.second;
    assert(C.SlackIdx && "Not support on the fly soft constraint creation!");

    // Add soft constraints Dst - Src >= C - Slack, i.e. Dst - Src + Slack >= C
    REAL Coefs[] = { 1.0, -1.0, 1.0 };
    int Cols[] = { getSUIdx(Dst), getSUIdx(Src), C.SlackIdx };
    if (!add_constraintex(lp, array_lengthof(Cols), Coefs, Cols, GE, C.C))
      report_fatal_error("Cannot add soft constraint!");

    nameLastRow("soft_");
  }
}

void SDCScheduler::buildASAPObject(double weight) {
  //Build the ASAP object function.
  for (iterator I = begin(), E = end(); I != E; ++I) {
    const VASTSchedUnit* U = I;

    if (U->isEntry()) continue;

    unsigned Idx = getSUIdx(U);
    // Because LPObjFn will set the objective function to maxim instead of minim,
    // we should use -1.0 instead of 1.0 as coefficient
    ObjFn[Idx] += - 1.0 * weight;
  }
}

void SDCScheduler::buildOptSlackObject(double weight) {
  //Build the Slack object function, maximize (Outdeg - Indeg) for each node.
  for (iterator I = begin(), E = end(); I != E; ++I) {
    const VASTSchedUnit* U = I;

    if (U->isEntry()) continue;

    unsigned Idx = getSUIdx(U);
    typedef VASTSchedUnit::const_dep_iterator dep_iterator;
    int NumValDeps = 0;
    for (dep_iterator I = U->dep_begin(), E = U->dep_end(); I != E; ++I) {
      if (I.getDFLatency() < 0)
        continue;

      NumValDeps += 1;
      const VASTSchedUnit *Src = *I;
      // Add the outdeg for source.
      ObjFn[getSUIdx(Src)] += 1.0 * weight;
    }

    // Minus the indeg for the current node, the outdeg will be added when we
    // visit its use.
    ObjFn[Idx] += (- NumValDeps) * weight;
  }
}

unsigned SDCScheduler::buildSchedule(lprec *lp) {
  unsigned TotalRows = get_Norig_rows(lp);
  unsigned Changed = 0;

  for (iterator I = begin(), E = end(); I != E; ++I) {
    VASTSchedUnit *U = I;

    if (U->isEntry()) continue;

    unsigned Idx = getSUIdx(U);
    REAL CurSchedule = get_var_primalresult(lp, TotalRows + Idx);
    unsigned j = CurSchedule;
    DEBUG(dbgs() << "At row:" << TotalRows + Idx
                 << " the result is:" << j << " (" << CurSchedule << ")\n");

    assert(j && double(j) == CurSchedule && "Bad result!");
    if (U->scheduleTo(j))
      ++Changed;
  }

  typedef SoftCstrVecTy::iterator iterator;
  for (iterator I = SoftConstraints.begin(), E = SoftConstraints.end();
       I != E; ++I) {
    SoftConstraint &C = I->second;
    // Ignore the eliminated soft constraints.
    if (C.SlackIdx == 0) continue;

    unsigned NegativeSlack = get_var_primalresult(lp, TotalRows + C.SlackIdx);
    assert (I->first.second->getSchedule() - I->first.first->getSchedule()
            + NegativeSlack >= C.C && "Bad soft constraint slack!");

    I->first.first->dump();
    I->first.second->dump();

    dbgs().indent(2) << C.C << " -> " << NegativeSlack << '\n';

    if (NegativeSlack != C.LastValue) {
      C.LastValue = NegativeSlack;
      ++Changed;
    }
  }

  return Changed;
}

bool SDCScheduler::lagSolveLP(lprec *lp) {
  // set_break_at_first(lp, TRUE);

  REAL LastDualObj = 0.0, MinimalObj = get_infinite(lp);
  REAL StepSizeLambda = 2.0;
  unsigned NotDecreaseSince = 0;

  unsigned MaxFeasiableiteration = 64;

  // Create
  for (unsigned iterations = 0;iterations < 10000; ++iterations) {
    // default_basis(lp);

    if (!solveLP(lp))
      return false;

    REAL DualObj = get_objective(lp);

    // set_break_at_first(lp, FALSE);

    LPObjFn CurObj(ObjFn);

    double SubGradientSqr = 0.0;
    LagSDCSolver::ResultType Result = LagSolver->update(lp, SubGradientSqr);
    switch (Result) {
    case LagSDCSolver::InFeasible:
      MaxFeasiableiteration = 64;

      if (iterations % 100 == 0) {
        dbgs() << " DualObj: " << DualObj
          << " (" << (LastDualObj - DualObj) << ") at iter: "
          << iterations << "\n";
      }

      if (MinimalObj >= DualObj)
        MinimalObj = DualObj - 0.01 * abs(DualObj);
      break;
    case LagSDCSolver::Feasible:
      dbgs() << " DualObj: " << DualObj
             << " feasiable solution at iter: "
             << iterations << "\n";
      MinimalObj = std::max<double>(MinimalObj, ObjFn.evaluateCurValue(lp));
      if (--MaxFeasiableiteration == 0)
        return true;
      break;
    case LagSDCSolver::Optimal:
      dbgs() << " DualObj: " << DualObj
             << " optimal solution at iter: "
             << iterations << "\n";
      return true;
    }

    if (DualObj < LastDualObj)
      NotDecreaseSince = 0;
    else
      ++NotDecreaseSince;

    if (NotDecreaseSince > 32) {
      StepSizeLambda /= 2.0;
      NotDecreaseSince = 0;
    }

    double StepSize = StepSizeLambda * (DualObj - MinimalObj) / SubGradientSqr;
    assert(StepSize > 0.0 && "Bad Step size factor!");

    LagSolver->updateMultipliers(CurObj, StepSize);
    CurObj.setLPObj(lp);

    LastDualObj = DualObj;
  }

  return false;
}

static cl::opt<unsigned> ILPTimeOut("vast-ilp-timeout",
  cl::desc("The timeout value for ilp solver, in seconds"),
  cl::init(10 * 60));

bool SDCScheduler::solveLP(lprec *lp) {
  set_presolve(lp, PRESOLVE_NONE, get_presolveloops(lp));

  DEBUG(write_lp(lp, "log.lp"));

  unsigned TotalRows = get_Nrows(lp), NumVars = get_Ncolumns(lp);
  DEBUG(dbgs() << "The model has " << NumVars << "x" << TotalRows
               << ", conditional nodes: " << ConditionalSUs.size()
               << ", synchronization nodes: " << SynchronizeSUs.size()
               << '\n');

  set_timeout(lp, ILPTimeOut);
  DEBUG(dbgs() << "Timeout is set to " << get_timeout(lp) << "secs.\n");

  assert(LPVarWeights.size() == unsigned(get_Ncolumns(lp)) &&
         "Broken variable weights!");
  set_var_weights(lp, LPVarWeights.data());

  return interpertResult(solve(lp));
}

bool SDCScheduler::interpertResult(int Result) {
  DEBUG(dbgs() << "ILP result is: " << get_statustext(lp, Result) << "\n");
  DEBUG(dbgs() << "Time elapsed: " << time_elapsed(lp) << "\n");
  DEBUG(dbgs() << "Object: " << get_var_primalresult(lp, 0) << "\n");

  switch (Result) {
  case INFEASIBLE:
    return false;
  case SUBOPTIMAL:
    DEBUG(dbgs() << "Note: suboptimal schedule found!\n");
  case OPTIMAL:
  case PRESOLVED:
    break;
  default:
    dbgs() << "ILPScheduler Schedule fail: "
           << get_statustext(lp, Result) << '\n';
#ifndef NDEBUG
    write_lp(lp, "fail.lp");
#endif
    report_fatal_error("Fail to schedule the design.");
  }

  return true;
}

static cl::opt<bool> CndDepTopBB("vast-sdc-cnd-dep-top-bb",
  cl::desc("Perform branch and bound on the conditional dependency slack"
            " based on topological order"),
  cl::init(false));

void SDCScheduler::addConditionalConstraints(VASTSchedUnit *SU) {
  SmallVector<int, 8> Cols;
  SmallVector<REAL, 8> Coeffs;

  BasicBlock *CurBB = SU->getParent();

  // Note that we had allocated variables for the slacks, these variables are
  // right after the step variable of SU.
  int Idx = getSUIdx(SU) + 1;
  bool HadMetEarierBranch = false;
  unsigned NumCndDeps = 0;

  typedef VASTSchedUnit::dep_iterator dep_iterator;
  for (dep_iterator I = SU->dep_begin(), E = SU->dep_end(); I != E; ++I) {
    assert(I.getEdgeType() == VASTDep::Conditional && "Unexpected edge type!");
    VASTSchedUnit *Dep = *I;

    int CurIdx = Idx++;

    BasicBlock *Predecessor = Dep->getParent();
    // Ignore the backedges
    if (DT.dominates(CurBB, Predecessor))
      continue;

    assert(I.getLatency() == 0 &&
           "Conditional dependencies must have a zero latency!");

    ++NumCndDeps;
    Cols.push_back(CurIdx);
    Coeffs.push_back(1.0);

    // First of all, export the slack for conditional edge. For conditional edge
    // we require Dst <= Src, hence we have Dst - Src + Slack = 0, Slack >= 0
    // i.e. Slack = Src - Dst
    if (LagSolver) {
      // Export the slack for the HeuristicalDriver
      REAL Coefs[] = { 1.0, -1.0, -1.0 };
      int Cols[] = { getSUIdx(Dep), getSUIdx(SU), CurIdx };
      if (!add_constraintex(lp, array_lengthof(Cols), Coefs, Cols, EQ, 0))
        report_fatal_error("Cannot export the slack of conditional edge!");
      nameLastRow("cnd_slack_");

      continue;
    }

    // Otherwise we need to require the slack to be non-negative.
    REAL SlackCoefs[] = { 1.0, -1.0 };
    int SlackCols[] = { getSUIdx(Dep), getSUIdx(SU) };
    unsigned NumCols = array_lengthof(SlackCols);
    if (!add_constraintex(lp, NumCols, SlackCoefs, SlackCols, GE, 0))
      report_fatal_error("Cannot export the slack of conditional edge!");
    nameLastRow("cnd_slack_");

    if (CndDepTopBB) {
      // Note that AuxVar is a binary variable, setting 0 to AuxVar means the
      // terminator of the predecessor block is 'connected' to the entry of
      // current BB. And the conditional dependency constraints require that
      // at least one terminator is connected to the entry of current BB. Hence,
      // as an initial solution, we can connect to the first terminator we meet
      // that has a smaller topological order number.
      if (!HadMetEarierBranch && Dep->getIdx() < SU->getIdx()) {
        set_var_branch(lp, CurIdx, BRANCH_FLOOR);
        HadMetEarierBranch = true;
      } else
        set_var_branch(lp, CurIdx, BRANCH_CEILING);
    }

    // Build constraints Slack - BigM * AuxVar <= 0,
    // i.e. Src - Dst - BigM * AuxVar <= 0
    int CurCols[] = { getSUIdx(Dep), getSUIdx(SU), CurIdx };
    REAL BigM = BigMMultiplier * getCriticalPathLength();
    REAL CurCoeffs[] = { 1.0, -1.0,  -BigM };

    if(!add_constraintex(lp, array_lengthof(CurCols), CurCoeffs, CurCols, LE, 0))
      report_fatal_error("Cannot create constraint!");

    nameLastRow("cnd_");
  }

  // No need to set a constraints for the trivial value.
  if (NumCndDeps == 1) {
    set_lowbo(lp, Cols.back(), 0);
    set_upbo(lp, Cols.back(), 0);
    return;
  }

  if (LagSolver) {
    LagSolver->addCndDep(Cols);
    return;
  }

  // Build the SOS like constraints for the conditional dependencies if we are
  // not using the heuristical driver.
  // The sum of AuxVars must be no bigger than NumCols - 1, so that at least
  // one of the AuxVars is zero. This means at least one of the slack variable
  // is zero.
  unsigned RHS = NumCndDeps - 1;
  if (!add_constraintex(lp, NumCndDeps, Coeffs.data(), Cols.data(), LE, RHS))
    report_fatal_error("Cannot create constraint!");

  nameLastRow("connectivity_");
}

void SDCScheduler::addConditionalConstraints() {
  typedef std::vector<VASTSchedUnit*>::iterator iterator;
  for (iterator I = ConditionalSUs.begin(), E = ConditionalSUs.end();
       I != E; ++I)
    addConditionalConstraints(*I);
}

static void BuildPredecessorMap(VASTSchedUnit *SU,
                                DenseMap<BasicBlock*, VASTSchedUnit*> &Map) {
  assert(SU->isBBEntry() && "Unexpected SU type!");

  typedef VASTSchedUnit::dep_iterator dep_iterator;
  for (dep_iterator I = SU->dep_begin(), E = SU->dep_end(); I != E; ++I) {
    assert(I.getEdgeType() == VASTDep::Conditional && "Unexpected edge type!");

    VASTSchedUnit *Dep = *I;
    assert(Dep->isTerminator() && "Bad Dep type of BBEntry!");
    Map.insert(std::make_pair(Dep->getParent(), Dep));
  }
}

void SDCScheduler::addSynchronizeConstraints(VASTSchedUnit *SU) {
  VASTSchedUnit *Entry = G.getEntrySU(SU->getParent());

  DenseMap<BasicBlock*, VASTSchedUnit*> PredecessorMap;
  BuildPredecessorMap(Entry, PredecessorMap);

  typedef VASTSchedUnit::dep_iterator dep_iterator;
  for (dep_iterator I = SU->dep_begin(), E = SU->dep_end(); I != E; ++I) {
    assert(I.getEdgeType() == VASTDep::Synchronize && "Unexpected edge type!");

    VASTSchedUnit *Dep = *I;
    VASTSchedUnit *PredExit = PredecessorMap.lookup(Dep->getParent());
    assert(PredExit && "Cannot find exit from predecessor block!");
    // The slack from the corresponding exit to Dep must no greater than the
    // slack from Entry to SU, :
    // Dep - Exit <= SU - Entry, i.e. Dep - Exit - SU + Entry <= 0
    int CurCols[] = { getSUIdx(Dep), getSUIdx(PredExit),
                      getSUIdx(SU), getSUIdx(Entry) };
    REAL CurCoeffs[] = { 1.0, -1.0, -1.0, 1.0,
                         0.0 /*RHS for Lagrangian constrant*/ };

    int Ty = Dep->isPHILatch() ? EQ : LE;

    unsigned NumData = array_lengthof(CurCols);
    if (!add_constraintex(lp, NumData, CurCoeffs, CurCols, Ty, 0))
      report_fatal_error("Cannot create constraint!");

    nameLastRow("sync_src_");
  }
}

unsigned SDCScheduler::createSlackPair(VASTSchedUnit *Dst, VASTSchedUnit *Src,
                                       unsigned SlackIdx) {
  // Split the slack into positive slack and negative slack.
  // The positive/negative slack is generated by the following constraints:
  // x - pos <= RH
  // x + neg >= RH
  // Given a value RH and a variable x, x is splited into two parts by RH:
  //             x <= RH                    x >= RH
  // ------------------------------RH---------------------------
  // Now if we have x = x0 fall into region x <= RH:
  //             x <= RH                    x >= RH
  // -------------------x0---------RH---------------------------
  // x - pos <= RH hold even if pos is 0, but neg need to be bigger than 0 to
  // fix constraints x0 + neg >= RH. Similarly, when x = 01 fall into region
  // x >= RH, pos will be bigger than 0 to fix constraint x - pos <= RH
  unsigned PosSlackIdx = SlackIdx;
  int PosCols[] = { getSUIdx(Dst), getSUIdx(Src), PosSlackIdx };
  REAL PosCoeffs[] = { 1.0, -1.0, -1.0 };
  if (!add_constraintex(lp, array_lengthof(PosCols), PosCoeffs, PosCols, LE, 0))
    report_fatal_error("Cannot create constraint!");
  nameLastRow(Twine(get_col_name(lp, PosSlackIdx)) + "_");
#ifndef ENABLE_GLOBAL_CODE_MOTION
  set_upbo(lp, PosSlackIdx, 0);
#endif

  unsigned NegSlackIdx = SlackIdx + 1;
  int NegCols[] = { getSUIdx(Dst), getSUIdx(Src), NegSlackIdx };
  REAL NegCoeffs[] = { 1.0, -1.0, 1.0 };
  if (!add_constraintex(lp, array_lengthof(NegCols), NegCoeffs, NegCols, GE, 0))
    report_fatal_error("Cannot create constraint!");
  nameLastRow(Twine(get_col_name(lp, NegSlackIdx)) + "_");
#ifndef ENABLE_GLOBAL_CODE_MOTION
  set_upbo(lp, NegSlackIdx, 0);
#endif

  return SlackIdx + 2;
}

void SDCScheduler::addLagSynchronizeConstraints(VASTSchedUnit *SU) {
  unsigned SlackIdxStart = getSUIdx(SU) + 1;
  unsigned SlackIdxEnd = SlackIdxStart;
  unsigned RowIdxStart = get_Nrows(lp) + 1;
  VASTSchedUnit *Entry = G.getEntrySU(SU->getParent());

  // Export the slack: SU - Entry + Slack = 0.
  SlackIdxEnd = createSlackPair(SU, Entry, SlackIdxEnd);

  DenseMap<BasicBlock*, VASTSchedUnit*> PredecessorMap;
  BuildPredecessorMap(Entry, PredecessorMap);
  typedef VASTSchedUnit::dep_iterator dep_iterator;
  for (dep_iterator I = SU->dep_begin(), E = SU->dep_end(); I != E; ++I) {
    assert(I.getEdgeType() == VASTDep::Synchronize && "Unexpected edge type!");

    VASTSchedUnit *Dep = *I;
    VASTSchedUnit *PredExit = PredecessorMap.lookup(Dep->getParent());
    assert(PredExit && "Cannot find exit from predecessor block!");
    // Export the slack: Dep - PredExit + Slack = 0.
    SlackIdxEnd = createSlackPair(Dep, PredExit, SlackIdxEnd);
  }

  unsigned RowIdxEnd = get_Nrows(lp) + 1;
  assert(RowIdxEnd - RowIdxStart == SlackIdxEnd - SlackIdxStart &&
         "Number of slack variables and constraints are not match!");
  LagSolver->addSyncDep(SlackIdxStart, SlackIdxEnd, RowIdxStart);
}

void SDCScheduler::addSynchronizeConstraints() {
  typedef std::vector<VASTSchedUnit*>::iterator iterator;
  for (iterator I = SynchronizeSUs.begin(), E = SynchronizeSUs.end();
       I != E; ++I) {
    if (LagSolver)
      addLagSynchronizeConstraints(*I);
    else
      addSynchronizeConstraints(*I);
  }
}

static
Loop *GetCommonParentLoop(BasicBlock *LHS, BasicBlock *RHS, LoopInfo &LI) {
  Loop *LHSLoop = LI.getLoopFor(LHS), *RHSLoop = LI.getLoopFor(RHS);

  if (LHSLoop == NULL || RHSLoop == NULL)
    return NULL;

  if (LHSLoop->contains(RHSLoop))
    return LHSLoop;

  while (RHSLoop && !RHSLoop->contains(LHSLoop))
    RHSLoop = RHSLoop->getParentLoop();

  return RHSLoop;
}

void
SDCScheduler::preserveAntiDependence(VASTSchedUnit *Src, VASTSchedUnit *Dst) {
  // Ignore the edge takes function arguments as source node for now since we
  // do not perform whole function pipelining.
  if (LLVM_UNLIKELY(Src->isEntry()))
    return;

  BasicBlock *DstParent = Dst->getParent();

  std::map<BasicBlock*, std::set<VASTSchedUnit*> >::iterator
    J = CFGEdges.find(DstParent);

  // No need to worry about the return block, it always exiting the loop
  if (J == CFGEdges.end())
    return;

  Loop *L = GetCommonParentLoop(Src->getParent(), DstParent, LI);

  if (L == NULL)
    return;

  std::set<VASTSchedUnit*> &Exits = J->second;

  // PHIs which are representing induction variable (or similar) need to be
  // handled in a different way.
  if (Src->isPHI() && Src->getParent() == L->getHeader()) {
    typedef VASTSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator I = Src->dep_begin(), E = Src->dep_end(); I != E; ++I) {
      assert(I.getEdgeType() == VASTDep::Synchronize && "Unexpected edge type!");
      VASTSchedUnit *Incoming = *I;
      assert(Incoming->isPHILatch() && "Unexpected type of Incoming SU of PHI!");
      // Back edge update had been handled by LoopWARDepBuilder.
      if (L->contains(Incoming->getParent()))
        continue;

      // Limit the throughput in parent loop.
      if (Loop *ParentLoop = L->getParentLoop())
        preserveAntiDependence(Src, Dst, ParentLoop, Exits);
    }

    return;
  }

  preserveAntiDependence(Src, Dst, L, Exits);
}

void
SDCScheduler::preserveAntiDependence(VASTSchedUnit *Src, VASTSchedUnit *Dst,
                                     Loop *L, std::set<VASTSchedUnit*> &Exits) {
  DEBUG(dbgs() << "Going to add throughput limitation constraints on edge:\n";
  Src->dump();
  Dst->dump();
  L->dump();
  dbgs() << '\n';);

  // Build Constraints Dst - Src <= Path Interval <= Initial Interval.
  // Where Path Interval >= DstParent Exit - Header, hence we have
  // Dst - Src <= DstParent Exit - Header <= Path Interval <= Initial, i.e.
  // Dst - Src + Header - DstParent Exit <= 0. The path interval (length) of
  // all path goes through Src and Dst is calculated by:
  // Path(Header, Src) + Path(Src, Dst Exit), because Header dominates Src and
  // Src dominates Dst, the equation can be rewritten as
  // Src - Header + Dst Exit - Src, i.e. Dst Exit - Header.
  typedef std::set<VASTSchedUnit*>::iterator iterator;

  for (iterator I = Exits.begin(), E = Exits.end(); I != E; ++I) {
    VASTSchedUnit *DstExit = *I;
    VASTSchedUnit *HeaderSU = 0;

    Loop *InclusiveLoop = L;
    while (InclusiveLoop && !InclusiveLoop->contains(DstExit->getTargetBlock()))
      InclusiveLoop = InclusiveLoop->getParentLoop();

    // No need to constraint the path if the path is not entirely included in
    // the loop.
    if (InclusiveLoop == NULL)
      continue;

    HeaderSU = G.getEntrySU(InclusiveLoop->getHeader());

    // Note: We must re-define the column numbers and coefficient in every
    // iteration, even though their are almost kept unchanged during the
    // iteration because add_constraintex modify these two arrays.
    int Cols[] = { getSUIdx(Dst), getSUIdx(Src),
                   getSUIdx(HeaderSU), getSUIdx(DstExit) };
    REAL Coeffs[] = { 1.0, -1.0, 1.0, -1.0 };

    if(!add_constraintex(lp, array_lengthof(Cols), Coeffs, Cols, LE, 0))
      report_fatal_error("Cannot create constraints!");

    nameLastRow("throughput_");
  }
}

void SDCScheduler::applyControlDependencies(VASTSchedUnit *SU) {
  // Ignore the edge takes function arguments as source node for now since we
  // do not perform whole function pipelining.
  if (LLVM_UNLIKELY(SU->isEntry() || SU->isExit()) || SU->isBBEntry())
    return;

  BasicBlock *BB = SU->getParent();

  VASTSchedUnit *SSrc = G.getEntrySU(BB);
  addDifferentialConstraint(SU, SSrc, GE, 0);
  nameLastRow("ctrl_ssrc_");

  // Control dependnecies to the teriminator of the BB for these nodes are
  // either not need (terminators), or handled by synchronization dependencies
  // (PHILatch).
  if (SU->isTerminator() || SU->isPHILatch())
    return;

  std::map<BasicBlock*, std::set<VASTSchedUnit*> >::iterator
    J = CFGEdges.find(BB);

  // No need to worry about the return block, it always exiting the loop
  if (J == CFGEdges.end())
    return;

  std::set<VASTSchedUnit*> &Exits = J->second;

  typedef std::set<VASTSchedUnit*>::iterator iterator;
  for (iterator I = Exits.begin(), E = Exits.end(); I != E; ++I) {
    VASTSchedUnit *SSnk = *I;
    addDifferentialConstraint(SSnk, SU, GE, 0);
    nameLastRow("ctrl_ssnk_");
  }
}

void SDCScheduler::addDependencyConstraints(lprec *lp) {
  for(VASTSchedGraph::iterator I = begin(), E = end(); I != E; ++I) {
    VASTSchedUnit *U = I;
    unsigned DstIdx = getSUIdx(U);

    typedef VASTSchedUnit::dep_iterator dep_iterator;
    // Build the constraint for Dst_SU_startStep - Src_SU_endStep >= Latency.
    for (dep_iterator DI = U->dep_begin(), DE = U->dep_end(); DI != DE; ++DI) {
      assert(!DI.isLoopCarried()
             && "Loop carried dependencies cannot handled by SDC scheduler!");
      VASTSchedUnit *Src = *DI;
      VASTDep Edge = DI.getEdge();

      // Conditional and synchronize edges are not handled here.
      if (Edge.getEdgeType() == VASTDep::Conditional ||
          Edge.getEdgeType() == VASTDep::Synchronize)
        continue;


      REAL Coefs[] = { 1.0, -1.0 };
      int Cols[] = { DstIdx, getSUIdx(Src) };
      int EqTy = (Edge.getEdgeType() == VASTDep::FixedTiming) ? EQ : GE;
      if (!add_constraintex(lp, array_lengthof(Cols), Coefs, Cols, EqTy, Edge.getLatency()))
        report_fatal_error("Cannot create dependence constraint!");

      nameLastRow("dep_");

#ifdef ENABLE_GLOBAL_CODE_MOTION
      // Limit throughput on edge, otherwise we may need to insert pipeline
      // register. At the same time, ignore the edge from BBEntry (representing
      // the guarding condition), because we had pipelined it (slot registers).
      if (!Src->isBBEntry() && DI.getDFLatency() > -1)
        preserveAntiDependence(Src, U);
#endif
    }

#ifndef ENABLE_GLOBAL_CODE_MOTION
    applyControlDependencies(U);
#endif
  }
}

void
SDCScheduler::addDifferentialConstraint(VASTSchedUnit *Dst, VASTSchedUnit *Src,
                                        int Ty, int RHS) {
  REAL Coefs[] = { 1.0, -1.0 };
  int Cols[] = { getSUIdx(Dst), getSUIdx(Src) };
  if (!add_constraintex(lp, array_lengthof(Cols), Coefs, Cols, Ty, RHS))
    report_fatal_error("Cannot create differential constraint!");
  nameLastRow("diff_");
}

void SDCScheduler::printVerision() const {
  int majorversion, minorversion, release, build;
  lp_solve_version(&majorversion, &minorversion, &release, &build);
  dbgs() << "Perform SDC scheduling with LPSolve "
         << majorversion << '.' << minorversion
         << '.' << release << '.' << build << '\n';
}

unsigned SDCScheduler::updateSoftConstraintPenalties() {
  return 0;
}

void SDCScheduler::addDependencyConstraints() {
  set_add_rowmode(lp, TRUE);

  // Build the constraints.
  addDependencyConstraints(lp);
  addSynchronizeConstraints();

  addConditionalConstraints();

  // Turn off the add rowmode and start to solve the model.
  set_add_rowmode(lp, FALSE);
}

bool SDCScheduler::schedule() {
#ifndef NDEBUG
  // Verify the scheduling graph before we are trying to schedule it.
  G.verify();
#endif

  ObjFn.setLPObj(lp);

  DEBUG(printVerision());

  addDependencyConstraints();
  addSoftConstraints();

  bool repeat = true;

  while (repeat) {
    if (LagSolver && !lagSolveLP(lp)) {
      reset();
      return false;
    } else if (!solveLP(lp)) {
      reset();
      return false;
    }

    // Schedule the state with the ILP result.
    repeat = buildSchedule(lp)
             && (updateSoftConstraintPenalties()
                 || resolveControlChainingHazard());
  }

  reset();
  return true;
}

void SDCScheduler::reset() {
  ObjFn.clear();
  SUIdx.clear();
  ConditionalSUs.clear();
  SynchronizeSUs.clear();
  LPVarWeights.clear();
  if (LagSolver) LagSolver->reset();
  delete_lp(lp);
  lp = 0;
}

SDCScheduler::~SDCScheduler() {
  if (LagSolver) delete LagSolver;
}

void SDCScheduler::initalizeCFGEdges() {
  Function &F =G.getFunction();

  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I) {
    BasicBlock *BB = I;

    if (!G.isBBReachable(BB))
      continue;

    VASTSchedUnit *BBEntry = G.getEntrySU(BB);

    // Collect all back-edge of the current loop
    typedef VASTSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator I = BBEntry->dep_begin(), E = BBEntry->dep_end();
         I != E; ++I) {
      if (I.getEdgeType() != VASTDep::Conditional)
        continue;

      VASTSchedUnit *Dep = *I;
      assert(Dep->isTerminator() && "Unexpected dependency type of Header!");
      BasicBlock *IncomingBB = Dep->getParent();
      CFGEdges[IncomingBB].insert(Dep);
    }
  }
}

void SDCScheduler::dumpModel() const {
  write_lp(lp, "log.lp");
}

void SDCScheduler::nameLastRow(const Twine &Name) {
#ifdef XDEBUG
  unsigned RowNo = get_Nrows(lp);
  std::string RowName = Name.str() + utostr_32(RowNo);
  set_row_name(lp, RowNo, const_cast<char*>(RowName.c_str()));
#endif
}

SDCScheduler::SDCScheduler(VASTSchedGraph &G, unsigned EntrySlot,
                           DominatorTree &DT, LoopInfo &LI)
  : SchedulerBase(G, EntrySlot), lp(0), LagSolver(0), DT(DT), LI(LI) {
  if (UseLagSolve)
    LagSolver = new LagSDCSolver();
}
