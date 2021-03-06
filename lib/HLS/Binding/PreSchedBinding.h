//===-- PreSchedBinding.h - Perform the Schedule Independent Binding ------===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the interface of PreSchedBinding pass. PreScheduleBinding
// perform the schedule independent binding.
//
//===----------------------------------------------------------------------===//
#ifndef PRE_SCHEDULE_BINDING_H
#define PRE_SCHEDULE_BINDING_H

#include "CompGraph.h"

#include "vast/FUInfo.h"
#include "vast/VASTModulePass.h"

namespace vast {
using namespace llvm;

// The PreSchedule Compatibility Graph.
class PSBCompNode : public CompGraphNode {
  std::set<VASTSeqOp*> KillOps;
public:
  PSBCompNode() : CompGraphNode() {}

  PSBCompNode(VFUs::FUTypes FUType, unsigned FUCost, unsigned Idx,
              DataflowInst Inst, ArrayRef<VASTSelector*> Sels)
    : CompGraphNode(FUType, FUCost, Idx, Inst, Sels) {}

  virtual ~PSBCompNode() {}

  void setKillOps(const std::set<VASTSeqOp*> &KillOps,
                  const std::set<VASTSeqOp*> &DefKillOps);

  bool isKillIntersect(const PSBCompNode *RHS) const;
  bool isSingleBlock() const;

  virtual bool isCompatibleWith(const CompGraphNode *RHS) const;

  typedef std::set<VASTSeqOp*>::const_iterator kill_iterator;
  kill_iterator kill_begin() const { return KillOps.begin(); }
  kill_iterator kill_end() const { return KillOps.end(); }

  void increaseSchedulingCost(PSBCompNode *Succ, float Cost);
};


class PreSchedBinding : public VASTModulePass {
  CompGraphBase *PSBCG;
public:
  PreSchedBinding();

  CompGraphBase *operator->() const { return PSBCG; }

  static char ID;
  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
};
}

#endif