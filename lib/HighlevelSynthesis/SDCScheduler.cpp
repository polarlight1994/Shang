//===-------- SDCScheduler.cpp ------- SDCScheduler -------------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
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
#include "shang/VASTSubModules.h"
#include "shang/Utilities.h"
#include "shang/VASTSeqValue.h"

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

static cl::opt<unsigned> ILPTimeOut("vast-ilp-timeout",
  cl::desc("The timeout value for ilp solver, in seconds"),
  cl::init(5 * 60));

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
  DEBUG(write_lp(lp, "log.lp"));
}

namespace {
struct ConstraintHelper {
  int SrcSlot, DstSlot;
  unsigned SrcIdx, DstIdx;

  ConstraintHelper()
    : SrcSlot(0), DstSlot(0), SrcIdx(0), DstIdx(0) {}

  void resetSrc(const VASTSchedUnit *Src, const SDCScheduler *S) {
    SrcSlot = Src->getSchedule();
    SrcIdx = SrcSlot == 0 ? S->getSUIdx(Src) : 0;
  }

  void resetDst(const VASTSchedUnit *Dst, const SDCScheduler *S) {
    DstSlot = Dst->getSchedule();
    DstIdx = DstSlot == 0 ? S->getSUIdx(Dst) : 0;
  }

  void addConstraintToLP(VASTDep Edge, lprec *lp, int ExtraCycles) {
    SmallVector<int, 2> Col;
    SmallVector<REAL, 2> Coeff;

    int RHS = Edge.getLatency() - DstSlot + SrcSlot + ExtraCycles;

    // Both SU is scheduled.
    if (SrcSlot && DstSlot) {
      assert(0 >= RHS && "Bad schedule!");
      return;
    }

    // Build the constraint.
    if (SrcSlot == 0) {
      assert(SrcIdx && "Bad SrcIdx!");
      Col.push_back(SrcIdx);
      Coeff.push_back(-1.0);
    }

    if (DstSlot == 0) {
      assert(DstIdx && "Bad DstIdx!");
      Col.push_back(DstIdx);
      Coeff.push_back(1.0);
    }

    assert(Edge.getEdgeType() != VASTDep::Conditional &&
           "Unexpected conditional edge!");
    int EqTy = (Edge.getEdgeType() == VASTDep::FixedTiming) ? EQ : GE;

    if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), EqTy, RHS))
      report_fatal_error("SDCScheduler: Can NOT add dependency constraints"
                         " at VASTSchedUnit " + utostr_32(DstIdx));

    DEBUG(std::string RowName = utostr_32(SrcIdx) + " -> " + utostr_32(DstIdx);
          unsigned NRow = get_Nrows(lp);
          set_row_name(lp, NRow, const_cast<char*>(RowName.c_str()));
    );
  }
};
}

unsigned SDCScheduler::createStepVariable(const VASTSchedUnit* U, unsigned Col) {
  // Set up the step variable for the VASTSchedUnit.
  bool inserted = SUIdx.insert(std::make_pair(U, Col)).second;
  assert(inserted && "Index already existed!");
  (void) inserted;
  add_columnex(lp, 0, 0,0);
  DEBUG(std::string SVStart;
  if (U->isBBEntry())
    SVStart = "entry_" + ShangMangle(U->getParent()->getName());
  else if (U->isTerminator()) {
    SVStart = "br_" + ShangMangle(U->getParent()->getName()) + "_";
    if (BasicBlock *TargetBB = U->getTargetBlock())
      SVStart += ShangMangle(TargetBB->getName());
    else
      SVStart += "exit";
  } else
    SVStart = "sv" + utostr_32(U->getIdx());

  dbgs() <<"Col#" << Col << " name: " <<SVStart << "\n";
  set_col_name(lp, Col, const_cast<char*>(SVStart.c_str())););
  set_int(lp, Col, TRUE);
  set_lowbo(lp, Col, EntrySlot);
  return Col + 1;
}

unsigned SDCScheduler::createSlackVariable(unsigned Col, int UB, int LB) {
  add_columnex(lp, 0, 0,0);
  DEBUG(std::string SlackName = "slack" + utostr_32(Col);
  dbgs() <<"Col#" << Col << " name: " << SlackName << "\n";
  set_col_name(lp, Col, const_cast<char*>(SlackName.c_str())););
  set_int(lp, Col, TRUE);

  if (UB != LB) {
    set_upbo(lp, Col, UB);
    set_lowbo(lp, Col, LB);
  }

  return Col + 1;
}

unsigned SDCScheduler::createVarForCndDeps(unsigned Col) {
  // Create the slack variable for the edge.
  Col = createSlackVariable(Col, 0, 0);
  // The auxiliary variable to specify one of the conditional dependence
  // and the current SU must have the same scheduling.
  Col = createSlackVariable(Col, 1, 0);

  return Col;
}

unsigned SDCScheduler::createVarForSyncDeps(unsigned Col) {
  int Bound = getCriticalPathLength() * BigMMultiplier;

  return createSlackVariable(Col, Bound, -Bound);
}

unsigned SDCScheduler::createLPAndVariables() {
  lp = make_lp(0, 0);
  unsigned Col =  1;
  for (iterator I =begin(), E = end(); I != E; ++I) {
    VASTSchedUnit* U = I;
    if (U->isScheduled())
      continue;

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
        assert((*I)->isSyncBarrier() && "Unexpected dependence type for sync edge!");
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
    I->second.SlackIdx = Col;
    Col = createSlackVariable(Col, 0, 0);
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

void
SDCScheduler::addConstraint(lprec *lp, VASTSchedUnit *Dst, VASTSchedUnit *Src,
                            int C, unsigned SlackIdx, int EqTy) {
  unsigned DstIdx = 0;
  int DstSlot = Dst->getSchedule();
  if (DstSlot == 0)
    DstIdx = getSUIdx(Dst);

  unsigned SrcIdx = 0;
  int SrcSlot = Src->getSchedule();
  if (SrcSlot == 0)
    SrcIdx = getSUIdx(Src);

  // Build constraint: Dst - Src >= C - V
  // Compute the constant by trying to move all the variable to RHS.
  int RHS = C - DstSlot + SrcSlot;

  // Both SU is scheduled.
  assert(!(SrcSlot && DstSlot) &&
         "Soft constraint cannot be apply to a fixed edge!");

  SmallVector<int, 3> Col;
  SmallVector<REAL, 3> Coeff;

  // Build the constraint.
  if (SrcSlot == 0) {
    Col.push_back(SrcIdx);
    Coeff.push_back(-1.0);
  }

  if (DstSlot == 0) {
    Col.push_back(DstIdx);
    Coeff.push_back(1.0);
  }

  // Add the slack variable.
  Col.push_back(SlackIdx);
  Coeff.push_back(1.0);

  if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), EqTy, RHS))
    report_fatal_error("SDCScheduler: Can NOT create soft Constraints"
                       " SlackIdx:" + utostr_32(SlackIdx));
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
    addConstraint(lp, Dst, Src, C.C, C.SlackIdx, GE);

    ObjFn[C.SlackIdx] = - C.Penalty;
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
    unsigned j = get_var_primalresult(lp, TotalRows + Idx);
    DEBUG(dbgs() << "At row:" << TotalRows + Idx
                 << " the result is:" << j << "\n");

    assert(j && "Bad result!");
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

    if (NegativeSlack != C.LastValue) {
      C.LastValue = NegativeSlack;
      ++Changed;
    }
  }

  return Changed;
}

bool SDCScheduler::solveLP(lprec *lp, bool PreSolve) {
  set_verbose(lp, NORMAL);
  DEBUG(set_verbose(lp, FULL));

  if (PreSolve) {
    set_presolve(lp, PRESOLVE_ROWS | PRESOLVE_COLS | PRESOLVE_LINDEP
                     | PRESOLVE_IMPLIEDFREE | PRESOLVE_REDUCEGCD
                     | PRESOLVE_PROBEFIX | PRESOLVE_PROBEREDUCE
                     | PRESOLVE_ROWDOMINATE /*| PRESOLVE_COLDOMINATE lpsolve bug*/
                     | PRESOLVE_MERGEROWS
                     | PRESOLVE_BOUNDS,
                 get_presolveloops(lp));
  } else
    set_presolve(lp, PRESOLVE_NONE, get_presolveloops(lp));

  DEBUG(write_lp(lp, "log.lp"));

  unsigned TotalRows = get_Nrows(lp), NumVars = get_Ncolumns(lp);
  dbgs() << "The model has " << NumVars << "x" << TotalRows
         << ", conditional nodes: " << ConditionalSUs.size()
         << ", synchronization nodes: " << SynchronizeSUs.size() << '\n';

  DEBUG(dbgs() << "Timeout is set to " << get_timeout(lp) << "secs.\n");

  set_timeout(lp, ILPTimeOut);

  int result = solve(lp);

  DEBUG(dbgs() << "ILP result is: "<< get_statustext(lp, result) << "\n");
  dbgs() << "Time elapsed: " << time_elapsed(lp) << "\n";
  DEBUG(dbgs() << "Object: " << get_var_primalresult(lp, 0) << "\n");

  switch (result) {
  case INFEASIBLE:
    return false;
  case SUBOPTIMAL:
    DEBUG(dbgs() << "Note: suboptimal schedule found!\n");
  case OPTIMAL:
  case PRESOLVED:
    break;
  default:
    report_fatal_error(Twine("ILPScheduler Schedule fail: ")
                       + Twine(get_statustext(lp, result)));
  }

  return true;
}

void SDCScheduler::addConditionalConstraints(VASTSchedUnit *SU) {
  SmallVector<int, 8> Cols;
  SmallVector<REAL, 8> Coeffs;

  // Note that we had allocated variables for the slacks, these variables are
  // right after the step variable of SU.
  int CurSlackIdx = getSUIdx(SU) + 1;
  typedef VASTSchedUnit::dep_iterator dep_iterator;
  for (dep_iterator I = SU->dep_begin(), E = SU->dep_end(); I != E; ++I) {
    assert(I.getEdgeType() == VASTDep::Conditional && "Unexpected edge type!");

    assert(I.getLatency() == 0 &&
           "Conditional dependencies must have a zero latency!");
    // First of all, export the slack for conditional edge. For conditional edge
    // we require Dst <= Src, hence we have Dst - Src + Slack = 0, Slack >= 0
    addConstraint(lp, SU, *I, 0, CurSlackIdx, EQ);

    int AuxVar = CurSlackIdx + 1;

    // Build constraints -Slack + BigM * AuxVar >= 0
    int CurCols[] = { CurSlackIdx, AuxVar };
    REAL CurCoeffs[] = { -1.0, BigMMultiplier * getCriticalPathLength() };

    if(!add_constraintex(lp, array_lengthof(CurCols), CurCoeffs, CurCols, GE, 0))
      report_fatal_error("Cannot create constraints!");

    Cols.push_back(AuxVar);
    Coeffs.push_back(1.0);
    CurSlackIdx += 2;
  }

  // The sum of AuxVars must be no bigger than NumCols - 1, so that at least
  // one of the AuxVars is zero. This means at least one of the slack variable
  // is zero.
  unsigned NumCols = Cols.size();
  unsigned RHS = NumCols - 1;
  if(!add_constraintex(lp, NumCols, Coeffs.data(), Cols.data(), LE, RHS))
    report_fatal_error("Cannot create constraints!");
}

void SDCScheduler::addConditionalConstraints() {
  typedef std::vector<VASTSchedUnit*>::iterator iterator;
  for (iterator I = ConditionalSUs.begin(), E = ConditionalSUs.end(); I != E; ++I)
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
  unsigned SrcIdx = getSUIdx(SU);

  // Note that we had allocated variables for the slacks, these variables are
  // right after the step variable of SU.
  unsigned SlackIdx = SrcIdx + 1;
  VASTSchedUnit *Entry = G.getEntrySU(SU->getParent());
  // Calculate the slack from Entry to SU: Slack = SU - Entry.
  addConstraint(lp, Entry, SU, 0, SlackIdx, EQ);

  DenseMap<BasicBlock*, VASTSchedUnit*> PredecessorMap;
  BuildPredecessorMap(Entry, PredecessorMap);

  typedef VASTSchedUnit::dep_iterator dep_iterator;
  for (dep_iterator I = SU->dep_begin(), E = SU->dep_end(); I != E; ++I) {
    assert(I.getEdgeType() == VASTDep::Synchronize && "Unexpected edge type!");

    VASTSchedUnit *Dep = *I;
    VASTSchedUnit *PredExit = PredecessorMap.lookup(Dep->getParent());
    assert(PredExit && "Cannot find exit from predecessor block!");
    // The slack from the corresponding exit to Dep must no greater than the
    // slack from Entry to SU, i.e.
    // Dep - Exit <= SU - Entry
    addConstraint(lp, PredExit, Dep, 0, SlackIdx, GE);
  }
}

void SDCScheduler::addSynchronizeConstraints() {
  typedef std::vector<VASTSchedUnit*>::iterator iterator;
  for (iterator I = SynchronizeSUs.begin(), E = SynchronizeSUs.end(); I != E; ++I)
    addSynchronizeConstraints(*I);
}

void SDCScheduler::addDependencyConstraints(lprec *lp) {
  for(VASTSchedGraph::iterator I = begin(), E = end(); I != E; ++I) {
    VASTSchedUnit *U = I;
    BasicBlock *CurBB = U->isBBEntry() ? U->getParent() : NULL;

    ConstraintHelper H;
    H.resetDst(U, this);

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

      H.resetSrc(Src, this);
      H.addConstraintToLP(Edge, lp, 0);
    }
  }
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
  DEBUG(printVerision());

  addDependencyConstraints();
  addSoftConstraints();

  bool changed = true;

  ObjFn.setLPObj(lp);

  if (!solveLP(lp, true))
    return false;

  // Schedule the state with the ILP result.
  changed |= (buildSchedule(lp) != 0);
  changed |= (updateSoftConstraintPenalties() != 0);

  ObjFn.clear();
  SUIdx.clear();
  ConditionalSUs.clear();
  delete_lp(lp);
  lp = 0;
  return true;
}

SDCScheduler::~SDCScheduler() {
}
