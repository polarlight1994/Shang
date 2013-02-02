//===----- VASTScheduling.cpp - Scheduling Graph on VAST  -------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VASTSUnit class, which represents the elemental
// scheduling unit in VAST.
//
//===----------------------------------------------------------------------===//
//

#include "VASTScheduling.h"
#include "shang/VASTModule.h"
#include "shang/VASTSeqValue.h"
#define DEBUG_TYPE "vast-scheduling-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;


//===----------------------------------------------------------------------===//
void VASTSchedGraph::schedule(VASTModule *VM) {
  buildSchedGraph(VM);
  schedule();
}

void VASTSchedGraph::buildSchedGraph(VASTModule *VM) {
  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator I = VM->slot_begin(), E = VM->slot_end(); I != E; ++I)
    buildSchedGraph(I);

  // Build the scheduling units for the branch operations.

  for (slot_iterator I = VM->slot_begin(), E = VM->slot_end(); I != E; ++I)
    buildDepEdges(I);
}

void VASTSchedGraph::buildSchedGraph(VASTSlot *S) {
  // Build the scheduling node for the entry of this slot.
  buildSlotEntry(S);

  // Build the scheduling node for each SeqOp in the slot.
  typedef VASTSlot::op_iterator op_iterator;
  for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I)
    buildSeqOpSU(*I);

  // Allocate the indices for the branch operations.
  InstIdx += S->succ_size();
}

VASTSUnit *VASTSchedGraph::buildSlotEntry(VASTSlot *S) {
  return buildSUnit(S);
}

VASTSUnit *VASTSchedGraph::buildSeqOpSU(VASTSeqOp *SeqOp) {
  return buildSUnit(SeqOp);
}

VASTSUnit *VASTSchedGraph::buildSUnit(VASTNode *N) {
  VASTSUnit *U = new VASTSUnit(N, ++InstIdx);
  bool inserted = N2SUMap.insert(std::make_pair(N, U)).second;
  assert(inserted && "SU already created!");
  (void) inserted;

  SUnits.push_back(U);

  return U;
}

void VASTSchedGraph::buildDepEdges(VASTSlot *S) {
  typedef VASTSlot::op_iterator op_iterator;
  for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I)
    buildDepEdges(*I);
}

void VASTSchedGraph::buildDatapathEdge(VASTSeqOp *SeqOp) {
  std::set<VASTSeqValue*> SeqDeps;

  typedef VASTOperandList::const_op_iterator use_itetator;
  for (use_itetator UI = SeqOp->op_begin(), UE = SeqOp->op_end(); UI != UE; ++UI)
    if (VASTValue *V = UI->unwrap().get())
      V->extractSupporingSeqVal(SeqDeps);

  VASTSUnit *CurSU = getSUnit(SeqOp);

  typedef std::set<VASTSeqValue*>::iterator dep_iterator;
  for (dep_iterator DI = SeqDeps.begin(), DE = SeqDeps.end(); DI != DE; ++DI) {
    VASTSeqValue *V = *DI;
    typedef VASTSeqValue::itertor itertor;
    for (itertor I = V->begin(), E = V->end(); I != E; ++I) {
    }
  }
  
}
