//===- SDCScheduler.cpp ------- SDCScheduler --------------------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//
//
//===----------------------------------------------------------------------===//

#include "SchedulerBase.h"
#include "shang/VASTSubModules.h"

#include "llvm/IR/Function.h"
#include "llvm/ADT/StringExtras.h"
#include "lpsolve/lp_lib.h"
#define DEBUG_TYPE "sdc-scheduler"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
/// Generate the linear order to resolve the
class BasicLinearOrderGenerator {
protected:
  SchedulerBase &G;

  typedef std::vector<VASTSchedUnit*> SUVecTy;
  typedef std::map<VASTNode*, SUVecTy> ConflictListTy;

  typedef std::map<BasicBlock*, ConflictListTy> LiveOutMapTy;
  LiveOutMapTy LiveOutFUs;
  void buildPipelineConflictMap(const VASTSchedUnit *Terminator);

  const SUVecTy *getLiveOuts(BasicBlock *MBB, VASTNode *Node) const {
    LiveOutMapTy::const_iterator at = LiveOutFUs.find(MBB);
    // There is no live-outs in this MBB, all SU only use trivial FUs.
    if (at == LiveOutFUs.end()) return 0;

    ConflictListTy::const_iterator su_at = at->second.find(Node);

    // Such FU are not used in the MBB.
    return su_at == at->second.end() ? 0 : &su_at->second;
  }

  void addLinOrdEdge(ConflictListTy &ConflictList);
  // Add the linear ordering edges to the SUs in the vector and return the first
  // SU.
  void addLinOrdEdge(SUVecTy &SUs);

  explicit BasicLinearOrderGenerator(SchedulerBase &G) : G(G) {}

  virtual void addLinOrdEdge();
public:

  static void addLinOrdEdge(SchedulerBase &S) {
    BasicLinearOrderGenerator(S).addLinOrdEdge();
  }
};

class SDCScheduler : public SchedulerBase {
  struct SoftConstraint {
    double Penalty;
    const VASTSchedUnit *Src, *Dst;
    unsigned SlackIdx, Slack;
  };
public:

  typedef VASTSchedGraph::iterator iterator;
  // Set the variables' name in the model.
  unsigned createLPAndVariables(iterator I, iterator E);
  unsigned addSoftConstraint(const VASTSchedUnit *Src, const VASTSchedUnit *Dst,
                             unsigned Slack, double Penalty);

  // Build the schedule object function.
  void buildASAPObject(iterator I, iterator E, double weight);
  void buildOptSlackObject(iterator I, iterator E, double weight);
  void addSoftConstraintsPenalties(double weight);

  void addObjectCoeff(const VASTSchedUnit *U, double Value) {
    // Ignore the constants.
    if (U->isScheduled()) return;

    ObjFn[getSUIdx(U)] += Value;
  }

  unsigned getSUIdx(const VASTSchedUnit* U) const {
    SUIdxIt at = SUIdx.find(U);
    assert(at != SUIdx.end() && "Idx not existed!");
    return at->second;
  }

private:
  lprec *lp;

  // Helper class to build the object function for lp.
  struct LPObjFn : public std::map<unsigned, double> {
    LPObjFn &operator*=(double val) {
      for (iterator I = begin(), E = end(); I != E; ++I)
        I->second *= val;

      return *this;
    }

    LPObjFn &operator+=(const LPObjFn &Other) {
      for (const_iterator I = Other.begin(), E = Other.end(); I != E; ++I)
        (*this)[I->first] += I->second;

      return *this;
    }

    void setLPObj(lprec *lp) const;
  };

  LPObjFn ObjFn;

  // The table of the index of the VSUnits and the column number in LP.
  typedef std::map<const VASTSchedUnit*, unsigned> SUI2IdxMapTy;
  typedef SUI2IdxMapTy::const_iterator SUIdxIt;
  SUI2IdxMapTy SUIdx;

  typedef std::vector<SoftConstraint> SoftCstrVecTy;
  SoftCstrVecTy SoftCstrs;

  // Create step variables, which represent the c-step that the VSUnits are
  // scheduled to.
  unsigned createStepVariable(const VASTSchedUnit *U, unsigned Col);

  void addSoftConstraints(lprec *lp);

  bool solveLP(lprec *lp);

  // Build the schedule form the result of ILP.
  void buildSchedule(lprec *lp, unsigned TotalRows, iterator I, iterator E);

  // The schedule should satisfy the dependences.
  void addDependencyConstraints(lprec *lp);

public:
  SDCScheduler(VASTSchedGraph &G, unsigned EntrySlot)
    : SchedulerBase(G, EntrySlot), lp(0) {}

  unsigned createLPAndVariables() {
    return createLPAndVariables(begin(), end());
  }

  void buildASAPObject(double weight) {
    buildASAPObject(begin(), end(), weight);
  }

  bool schedule();
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

/*  typedef VASTSchedGraph::iterator iterator;
  BasicBlock *PrevBB = G->getEntryBB();

  for (iterator I = G.begin(), E = G.end(); I != E; ++I) {
    VASTSchedUnit *U = *I;
    // No need to assign the linear order for the SU which already has a fixed
    // timing constraint.
    if (U->hasFixedTiming()) {
      if (U->isTerminator()) buildPipelineConflictMap(U);
      continue;
    }

    BasicBlock *BB = U->getParentBB();
    if (BB != PrevBB) {
      addLinOrdEdge(ConflictList);
      ConflictList.clear();
      PrevBB = MBB;
    }

    FuncUnitId Id = U->getFUId();

    // FIXME: Detect mutually exclusive predicate condition.
    if (!Id.isBound()) continue;

    ConflictList[Id].push_back(U);
  }

  addLinOrdEdge(ConflictList);

  G->topologicalSortCPSUs();
  */
}

void BasicLinearOrderGenerator::buildPipelineConflictMap(const VASTSchedUnit *U) {
  //assert(U->isTerminator() && "Bad SU type!");

  //MachineBasicBlock *ParentBB = U->getParentBB();
  //unsigned II = G->getII(ParentBB);
  // There is no FU conflict if the block is not pipelined.
  //if (II == 0) return;

  llvm_unreachable("buildSuccConfilictMap is not supported yet!");
}

void BasicLinearOrderGenerator::addLinOrdEdge(ConflictListTy &List) {
  /*
  typedef ConflictListTy::iterator iterator;
  for (iterator I = List.begin(), E = List.end(); I != E; ++I) {
    std::vector<VASTSchedUnit*> &SUs = I->second;
    VASTNode *Node = I->first;

    VASTSchedUnit *FirstSU = 0, *LastSU = 0;

    if (!SUs.empty()) {
      std::sort(SUs.begin(), SUs.end(), alap_less(G));
      FirstSU = SUs.front();
      LastSU = SUs.back();
      addLinOrdEdge(SUs);
    }

    BasicBlock *CurBB = FirstSU->getParentBB();
    SUVecTy LiveOuts;

    if (LastSU) LiveOuts.push_back(LastSU);

    typedef BasicBlock::pred_iterator pred_iterator;
    for (pred_iterator PI = CurBB->pred_begin(), PE = CurBB->pred_end();
         PI != PE; ++PI) {
      BasicBlock *PredBB = *PI;

      // Ignore the backward edges.
      // if (PredBB->getNumber() >= CurBB->getNumber()) continue;

      const SUVecTy *PredLiveOuts = getLiveOuts(PredBB, Id);

      if (PredLiveOuts == 0) continue;

      if (FirstSU) {
        typedef SUVecTy::const_iterator su_iterator;
        for (su_iterator SI = PredLiveOuts->begin(), SE = PredLiveOuts->end();
             SI != SE; ++SI) {
          VASTSchedUnit *PredSU = *SI;
          // Add the dependencies between the liveouts from pred SU to the first
          // SU of the current BB.
          unsigned IntialInterval = 1;
          VASTDep Edge = VASTDep::CreateDep<VASTDep::LinearOrder>(IntialInterval);
          FirstSU->addDep<true>(PredSU, Edge);
        }

        continue;
      }

      // Else forward the live out of pred BB to the current BB.
      LiveOuts.insert(LiveOuts.end(), PredLiveOuts->begin(), PredLiveOuts->end());
    }

    // Build the live out vector for current BB.
    LiveOutFUs[CurBB][Id] = LiveOuts;
  }
  */
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
  std::string SVStart = "sv" + utostr_32(U->getIdx());
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

bool SDCScheduler::schedule() {
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

//===----------------------------------------------------------------------===//
void VASTSchedGraph::scheduleSDC() {
  SDCScheduler Scheduler(*this, 1);

  Scheduler.buildTimeFrameAndResetSchedule(true);
  BasicLinearOrderGenerator::addLinOrdEdge(Scheduler);

  // Build the step variables, and no need to schedule at all if all SUs have
  // been scheduled.
  if (Scheduler.createLPAndVariables()) {
    Scheduler.addObjectCoeff(getExit(), -1.0);

    bool success = Scheduler.schedule();
    assert(success && "SDCScheduler fail!");
    (void) success;
  }

  DEBUG(viewGraph());
}
