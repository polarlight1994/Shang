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

#include "llvm/ADT/StringExtras.h"
#include "lpsolve/lp_lib.h"
#define DEBUG_TYPE "sdc-scheduler"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
/// Generate the linear order to resolve the
struct BasicLinearOrderGenerator {
  SchedulerBase &G;

  typedef std::vector<VASTSchedUnit*> SUVecTy;
  typedef std::map<VASTNode*, SUVecTy> ConflictListTy;

  void addLinOrdEdge(ConflictListTy &ConflictList);
  // Add the linear ordering edges to the SUs in the vector and return the first
  // SU.
  void addLinOrdEdge(SUVecTy &SUs);

  explicit BasicLinearOrderGenerator(SchedulerBase &G) : G(G) {}

  virtual void addLinOrdEdge();
};

struct alap_less {
  SchedulerBase &S;
  alap_less(SchedulerBase &s) : S(s) {}
  bool operator() (const VASTSchedUnit *LHS, const VASTSchedUnit *RHS) const {
    // Ascending order using ALAP.
    if (S.getALAPStep(LHS) < S.getALAPStep(RHS)) return true;
    if (S.getALAPStep(LHS) > S.getALAPStep(RHS)) return false;

    // Tie breaker 1: ASAP.
    if (S.getASAPStep(LHS) < S.getASAPStep(RHS)) return true;
    if (S.getASAPStep(LHS) > S.getASAPStep(RHS)) return false;

    // Tie breaker 2: Original topological order.
    return LHS->getIdx() < RHS->getIdx();
  }
};
}

void BasicLinearOrderGenerator::addLinOrdEdge() {
  ConflictListTy ConflictList;

  typedef VASTSchedGraph::bb_iterator bb_iterator;
  for (bb_iterator I = G->bb_begin(), E = G->bb_end(); I != E; ++I) {
    MutableArrayRef<VASTSchedUnit*> SUs(I->second);
    ConflictList.clear();

    // Iterate the scheduling units in the same BB to assign linear order.
    for (unsigned i = 0; i < SUs.size(); ++i) {
      VASTSchedUnit *SU = SUs[i];

      VASTSeqOp *Op = SU->getSeqOp();

      // Ignore the trivial operations.
      if (Op == 0 || Op->getNumSrcs() == 0) continue;

      // Ignore the Latch, they will not cause a resource conflict.
      if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(Op))
        if (SeqInst->getSeqOpType() == VASTSeqInst::Latch) continue;

      VASTSelector *Sel = Op->getSrc(Op->getNumSrcs() - 1).getSelector();

      // Ignore the common resource.
      if (isa<VASTRegister>(Sel->getParent())) continue;

      // Assign the linear order.
      ConflictList[Sel].push_back(SU);
    }

    addLinOrdEdge(ConflictList);
  }

  G->topologicalSortSUs();
}

void BasicLinearOrderGenerator::addLinOrdEdge(ConflictListTy &List) {
  typedef ConflictListTy::iterator iterator;
  for (iterator I = List.begin(), E = List.end(); I != E; ++I) {
    std::vector<VASTSchedUnit*> &SUs = I->second;

    if (!SUs.empty()) {
      std::sort(SUs.begin(), SUs.end(), alap_less(G));
      addLinOrdEdge(SUs);
    }
  }
}

void BasicLinearOrderGenerator::addLinOrdEdge(SUVecTy &SUs) {
  VASTSchedUnit *LaterSU = SUs.back();
  SUs.pop_back();

  while (!SUs.empty()) {
    VASTSchedUnit *EalierSU = SUs.back();
    SUs.pop_back();

    // Build a dependence edge from EalierSU to LaterSU.
    // TODO: Add an new kind of edge: Constraint Edge, and there should be
    // hard constraint and soft constraint.
    unsigned IntialInterval = 1;
    VASTDep Edge = VASTDep::CreateDep<VASTDep::LinearOrder>(IntialInterval);
    LaterSU->addDep(EalierSU, Edge);

    LaterSU = EalierSU;
  }
}

void SDCScheduler::addLinOrdEdge() {
  buildTimeFrameAndResetSchedule(true);
  BasicLinearOrderGenerator(*this).addLinOrdEdge();
}

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

  void addConstraintToLP(VASTDep Edge, lprec *lp, int ExtraLatency) {
    SmallVector<int, 2> Col;
    SmallVector<REAL, 2> Coeff;

    int RHS = Edge.getLatency() - DstSlot + SrcSlot + ExtraLatency;

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

    int EqTy = (Edge.getEdgeType() == VASTDep::FixedTiming) ? EQ :
               (Edge.getEdgeType() == VASTDep::Conditional) ? LE :
               GE;

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
  std::string SVStart;
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

  DEBUG(dbgs() <<"Col#" << Col << " name: " <<SVStart << "\n");
  set_col_name(lp, Col, const_cast<char*>(SVStart.c_str()));
  set_int(lp, Col, TRUE);
  set_lowbo(lp, Col, EntrySlot);
  return Col + 1;
}

unsigned SDCScheduler::createLPAndVariables(iterator I, iterator E) {
  lp = make_lp(0, 0);
  unsigned Col =  1;
  while (I != E) {
    const VASTSchedUnit* U = I++;
    if (U->isScheduled()) continue;

    Col = createStepVariable(U, Col);
  }

  return Col - 1;
}

unsigned SDCScheduler::addSoftConstraint(const VASTSchedUnit *Src, const VASTSchedUnit *Dst,
                                               unsigned Slack, double Penalty) {
  if (Src->isScheduled() && Dst->isScheduled()) return 0;

  unsigned NextCol = get_Ncolumns(lp) + 1;
  SoftConstraint C = { Penalty, Src, Dst, NextCol, Slack };
  SoftCstrs.push_back(C);
  std::string SlackName = "slack" + utostr_32(NextCol);
  DEBUG(dbgs() <<"Col#" << NextCol << " name: " <<SlackName << "\n");
  set_col_name(lp, NextCol, const_cast<char*>(SlackName.c_str()));
  set_int(lp, NextCol, TRUE);
  return NextCol;
}

void SDCScheduler::addSoftConstraints(lprec *lp) {
  typedef SoftCstrVecTy::iterator iterator;
  SmallVector<int, 3> Col;
  SmallVector<REAL, 3> Coeff;

  // Build the constraint Dst - Src <= Latency - Slack
  // FIXME: Use ConstraintHelper.
  for (iterator I = SoftCstrs.begin(), E = SoftCstrs.end(); I != E; ++I) {
    SoftConstraint &C = *I;

    unsigned DstIdx = 0;
    int DstSlot = C.Dst->getSchedule();
    if (DstSlot == 0) DstIdx = getSUIdx(C.Dst);

    unsigned SrcIdx = 0;
    int SrcSlot = C.Src->getSchedule();
    if (SrcSlot == 0) SrcIdx = getSUIdx(C.Src);

    int RHS = C.Slack - DstSlot + SrcSlot;

    // Both SU is scheduled.
    if (SrcSlot && DstSlot) continue;

    Col.clear();
    Coeff.clear();

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
    Col.push_back(C.SlackIdx);
    Coeff.push_back(1.0);

    if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), GE, RHS))
      report_fatal_error("SDCScheduler: Can NOT step soft Constraints"
                         " SlackIdx:" + utostr_32(C.SlackIdx));
  }
}

void SDCScheduler::addSoftConstraintsPenalties(double weight) {
  typedef SoftCstrVecTy::iterator iterator;
  for (iterator I = SoftCstrs.begin(), E = SoftCstrs.end(); I != E; ++I) {
    const SoftConstraint &C = *I;
    // Ignore the eliminated soft constraints.
    if (C.SlackIdx == 0) continue;

    ObjFn[C.SlackIdx] -= C.Penalty * weight;
  }
}

void SDCScheduler::buildASAPObject(iterator I, iterator E, double weight) {
  //Build the ASAP object function.
  while (I != E) {
    const VASTSchedUnit* U = I++;

    if (U->isScheduled()) continue;

    unsigned Idx = getSUIdx(U);
    // Because LPObjFn will set the objective function to maxim instead of minim,
    // we should use -1.0 instead of 1.0 as coefficient
    ObjFn[Idx] += - 1.0 * weight;
  }
}

void SDCScheduler::buildOptSlackObject(iterator I, iterator E, double weight) {
  llvm_unreachable("Not implemented!");

  while (I != E) {
    const VASTSchedUnit* U = I++;

    if (U->isScheduled()) continue;

    int Indeg = 0; // U->countValDeps();
    int Outdeg = 0; //U->countValUses();
    unsigned Idx = getSUIdx(U);
    ObjFn[Idx] += (Outdeg - Indeg) * weight;
  }
}

void SDCScheduler::buildSchedule(lprec *lp, unsigned TotalRows,
                                      iterator I, iterator E) {
  while (I != E) {
    VASTSchedUnit *U = I++;

    if (U->isScheduled()) continue;

    unsigned Idx = getSUIdx(U);
    unsigned j = get_var_primalresult(lp, TotalRows + Idx);
    DEBUG(dbgs() << "At row:" << TotalRows + Idx
                 << " the result is:" << j << "\n");

    assert(j && "Bad result!");
    U->scheduleTo(j);
  }
}

// Helper function
static const char *transSolveResult(int result) {
  if (result == -2) return "NOMEMORY";
  else if (result > 13) return "Unknown result!";

  static const char *ResultTable[] = {
    "OPTIMAL",
    "SUBOPTIMAL",
    "INFEASIBLE",
    "UNBOUNDED",
    "DEGENERATE",
    "NUMFAILURE",
    "USERABORT",
    "TIMEOUT",
    "PRESOLVED",
    "PROCFAIL",
    "PROCBREAK",
    "FEASFOUND",
    "NOFEASFOUND"
  };

  return ResultTable[result];
}

bool SDCScheduler::solveLP(lprec *lp) {
  set_verbose(lp, CRITICAL);
  DEBUG(set_verbose(lp, FULL));

  set_presolve(lp, PRESOLVE_ROWS | PRESOLVE_COLS | PRESOLVE_LINDEP
                   | PRESOLVE_IMPLIEDFREE | PRESOLVE_REDUCEGCD
                   | PRESOLVE_PROBEFIX | PRESOLVE_PROBEREDUCE
                   | PRESOLVE_ROWDOMINATE /*| PRESOLVE_COLDOMINATE lpsolve bug*/
                   | PRESOLVE_MERGEROWS
                   | PRESOLVE_BOUNDS,
               get_presolveloops(lp));

  DEBUG(write_lp(lp, "log.lp"));

  unsigned TotalRows = get_Nrows(lp), NumVars = get_Ncolumns(lp);
  DEBUG(dbgs() << "The model has " << NumVars
               << "x" << TotalRows << '\n');

  DEBUG(dbgs() << "Timeout is set to " << get_timeout(lp) << "secs.\n");

  int result = solve(lp);

  DEBUG(dbgs() << "ILP result is: "<< transSolveResult(result) << "\n");
  DEBUG(dbgs() << "Time elapsed: " << time_elapsed(lp) << "\n");

  switch (result) {
  case INFEASIBLE:
    delete_lp(lp);
    return false;
  case SUBOPTIMAL:
    DEBUG(dbgs() << "Note: suboptimal schedule found!\n");
  case OPTIMAL:
  case PRESOLVED:
    break;
  default:
    report_fatal_error(Twine("ILPScheduler Schedule fail: ")
                       + Twine(transSolveResult(result)));
  }

  return true;
}

void SDCScheduler::addDependencyConstraints(lprec *lp) {
  for(VASTSchedGraph::const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTSchedUnit *U = I;

    ConstraintHelper H;
    H.resetDst(U, this);

    typedef VASTSchedUnit::const_dep_iterator dep_iterator;
    // Build the constraint for Dst_SU_startStep - Src_SU_endStep >= Latency.
    for (dep_iterator DI = U->dep_begin(), DE = U->dep_end(); DI != DE; ++DI) {
      assert(!DI.isLoopCarried()
        && "Loop carried dependencies cannot handled by SDC scheduler!");
      const VASTSchedUnit *Src = *DI;
      VASTDep Edge = DI.getEdge();

      // Ignore the control-dependency edges between BBs.
      // if (Edge.getEdgeType() == VASTDep::Conditional) continue;

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

bool SDCScheduler::schedule() {
  DEBUG(printVerision());

  ObjFn.setLPObj(lp);

  set_add_rowmode(lp, TRUE);

  // Build the constraints.
  addDependencyConstraints(lp);
  addSoftConstraints(lp);

  // Turn off the add rowmode and start to solve the model.
  set_add_rowmode(lp, FALSE);
  unsigned TotalRows = get_Nrows(lp);

  if (!solveLP(lp)) return false;

  // Schedule the state with the ILP result.
  buildSchedule(lp, TotalRows, begin(), end());

  delete_lp(lp);
  lp = 0;
  SUIdx.clear();
  ObjFn.clear();
  return true;
}
