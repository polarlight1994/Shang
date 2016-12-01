//===------- SIRSDCScheduler.cpp ------- SDCScheduler -----------*- C++ -*-===//
//
//                          The SIR HLS framework                             //
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
#include "sir/SIRSDCScheduler.h"
#include "llvm/ADT/STLExtras.h"
#include "lp_lib.h"
#include "vast/LuaI.h"

using namespace llvm;
using namespace vast;

void SIRSDCScheduler::reset() {
  // Clear all the containers.
  ObjFn.clear();
  SU2Col.clear();
  VarWeights.clear();
  // Delete the LP model.
  delete_lp(lp);
  lp = 0;
}

unsigned SIRSDCScheduler::createLPVariable(SIRSchedUnit *U, unsigned ColNum) {
  // Set up the LP Variable for the SIRSchedUnit.
  bool inserted = SU2Col.insert(std::make_pair(U, ColNum)).second;
  assert(inserted && "ColNum already existed!");

  // Name the LP Variable for debug.
  std::string LPVar = "lpvar" + utostr_32(U->getIdx());
  set_col_name(lp, ColNum, const_cast<char *>(LPVar.c_str()));

  if (U->isCombSU())
    set_int(lp, ColNum, FALSE);
  else
    set_int(lp, ColNum, TRUE);

  // Constraint the SUnit if we can.
  if (U->isScheduled()) {
    set_lowbo(lp, ColNum, U->getSchedule());
    set_upbo(lp, ColNum, U->getSchedule());
  } else
    set_lowbo(lp, ColNum, EntrySchedule);

  // Assign a low priority to the variables, so the LPSolve can focus on
  // those difficult constraints first.
  VarWeights.push_back(256.0);

  return ++ColNum;
}

unsigned SIRSDCScheduler::createLPAndLPVariables() {
  // Initial the LP model.
  lp = make_lp(0, 0);

  // Ignore the unimportant debug output and only print
  // the CRITICAL info.
  set_verbose(lp, CRITICAL);

  // Initial the Col which represent the SUnit.
  unsigned Col = 1;

  for (iterator I = begin(), E = end(); I != E; ++I) {
    SIRSchedUnit *SU = I;

    // Create the LPVariable for this SUnit. The Col
    // will increase by 1 inside the function.
    Col = createLPVariable(SU, Col);
  }

  // Return the number of LP Variables created.
  return Col - 1;
}

void SIRSDCScheduler::addDependencyConstraints() {
  set_add_rowmode(lp, TRUE);

  // Add the constraints.
  for (SIRSchedGraph::iterator I = begin(), E = end(); I != E; ++I) {
    SIRSchedUnit *DstSU = I;

    // Get the Col of the SUnit.
    unsigned DstSUCol = getSUCol(DstSU);

    // Get the ParentBB of the SUnit.
    BasicBlock *BB = DstSU->getParentBB();
    unsigned MII = G.getMII(BB);

    typedef SIRSchedUnit::dep_iterator dep_iterator;
    for (dep_iterator DI = DstSU->dep_begin(), DE = DstSU->dep_end();	DI != DE; ++DI) {
      // Get the Src SUnit and the dependency edge.
      SIRSchedUnit *SrcSU = *DI;
      unsigned SrcSUCol = getSUCol(SrcSU);
      SIRDep DepEdge = DI.getEdge();
      REAL Coefs[] = { 1.0, -1.0 };
      int Cols[] = { DstSUCol, SrcSUCol };

      float Latency;

      // Ignore the dependency to itself.
      if (DstSU == SrcSU)
        continue;

      if (MII) {
        // Need to ignore the control back-edge.
        if (SrcSU->getIdx() >= DstSU->getIdx() && (DI.getEdgeType() == SIRDep::CtrlDep || DI.getEdgeType() == SIRDep::DelayDep))
          continue;

        Latency = DepEdge.getLatency(MII) + SrcSU->getLatency();
      } else {
          // Ignore the back-edge.
          if ((SrcSU->getIdx() >= DstSU->getIdx() || DI.getDistance() != 0) &&
              DI.getEdgeType() != SIRDep::SyncDep)
            continue;

          // We are not pipelining BB here so the II is zero.
          Latency = DepEdge.getLatency(0) + SrcSU->getLatency();
      }

      // Create the constraint according to the dependency.
      if (!add_constraintex(lp, array_lengthof(Cols), Coefs, Cols, GE, Latency))
        report_fatal_error("Dependency constraint created failed!");
    }
  }

  set_add_rowmode(lp, FALSE);
}

void SIRSDCScheduler::assignObjCoeff(SIRSchedUnit *ObjU, double weight) {
  // Ignore the Object SUnit which is already scheduled.
  if (ObjU->isScheduled()) return;

  ObjFn[getSUCol(ObjU)] = weight;
}

void SIRSDCScheduler::buildASAPObj() {
  for (iterator I = begin(), E = end(); I != E; ++I) {
    SIRSchedUnit *U = I;

    assignObjCoeff(U, -1.0);
  }

  ObjFn.setLPObj(lp);
}

void SIRSDCScheduler::LPObjFn::setLPObj(lprec *lp) const {
  std::vector<int> Indices;
  std::vector<REAL> Coefficients;

  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    Indices.push_back(I->first);
    Coefficients.push_back(I->second);
  }

  set_obj_fnex(lp, size(), Coefficients.data(), Indices.data());
  set_maxim(lp);
}

bool SIRSDCScheduler::solveLP(lprec *lp) {
  set_presolve(lp, PRESOLVE_NONE, get_presolveloops(lp));

  set_var_weights(lp, VarWeights.data());

  return interpertResult(solve(lp));
}

bool SIRSDCScheduler::interpertResult(int Result) {
  switch (Result) {
    // The LP model is feasible.
  case INFEASIBLE:
    return false;
    // The result is sub-optimal.
    //case SUBOPTIMAL:
    // The result is solved in pre-solve.
  case PRESOLVED:
    // The result is optimal.
  case OPTIMAL:
    break;
  default:
    report_fatal_error("Fail to schedule the design.");
  }

  return true;
}

bool SIRSDCScheduler::scheduleSUs() {
  unsigned TotalRows = get_Norig_rows(lp);
  unsigned Changed = 0;

  /// Debug Code
  std::string SDCResult = LuaI::GetString("SDCResult");
  std::string Error;
  raw_fd_ostream Output(SDCResult.c_str(), Error);

  for (iterator I = begin(), E = end(); I != E; ++I) {
    SIRSchedUnit *U = I;

    unsigned Col = getSUCol(U);
    REAL Result = get_var_primalresult(lp, TotalRows + Col);

    /// Debug Code
    Output << "SU#" << U->getIdx() << " scheduled to " << Result << "\n";

    // Handle the SUnits in Slot0r specially since they are
    // always scheduled to 0.
    if (!U->getParentBB() && !U->isExit())
      assert(Result == 0.0 && "Unexpected SDC result!");
    else if (U->scheduleTo(Result))
      ++Changed;
  }

  return Changed;
}

bool SIRSDCScheduler::schedule() {
  // Reset the schedule.
  G.resetSchedule();

  // Create the LP model and LP Variables for SUnits.
  createLPAndLPVariables();

  // Add constraints according to the dependencies.
  addDependencyConstraints();

  // Build the ASAP object.
  buildASAPObj();

  // Print the LP model to debug.
  write_lp(lp, "log.lp");

  // Solve the LP model.
  bool repeat = true;
  while (repeat) {
    if (!solveLP(lp)) {
      reset();

      llvm_unreachable("SDC failed!");
    }

    // Schedule the SUnit according to the result. If fail,
    // then repeat the schedule process.
    repeat = scheduleSUs();
  }


  return true;
}