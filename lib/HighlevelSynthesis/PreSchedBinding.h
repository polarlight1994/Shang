//===-- PreSchedBinding.h - Perform the Schedule Independent Binding ------===//
//
//                      The Shang HLS frameowrk                               //
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

#include "shang/FUInfo.h"
#include "shang/VASTModulePass.h"

namespace llvm {
// The PreSchedule Compatibility Graph.
class PSBCompNode : public CompGraphNode {
  std::set<VASTSeqOp*> KillOps;
public:
  PSBCompNode() : CompGraphNode() {}

  PSBCompNode(VFUs::FUTypes FUType, unsigned FUCost, unsigned Idx,
              DataflowInst Inst, ArrayRef<VASTSelector*> Sels)
    : CompGraphNode(FUType, FUCost, Idx, Inst, Sels) {}

  void setKillOps(const std::set<VASTSeqOp*> &KillOps,
                  const std::set<VASTSeqOp*> &DefKillOps);

  bool isKillIntersect(const PSBCompNode *RHS) const;
  
  typedef std::set<VASTSeqOp*>::const_iterator kill_iterator;
  kill_iterator kill_begin() const { return KillOps.begin(); }
  kill_iterator kill_end() const { return KillOps.end(); }

  void increaseSchedulingCost(PSBCompNode *Succ);
  static void IncreaseSchedulingCost(PSBCompNode *LHS, PSBCompNode *RHS);
};

class PSBCompGraph : public CompGraphBase {
  bool isCompatible(CompGraphNode *Src, CompGraphNode *Dst) const;
public:
  PSBCompGraph(DominatorTree &DT, CachedStrashTable &CST)
    : CompGraphBase(DT, CST) {}

  CompGraphNode *createNode(VFUs::FUTypes FUType, unsigned FUCost, unsigned Idx,
                            DataflowInst Inst, ArrayRef<VASTSelector*> Sels)
                            const {
    return new PSBCompNode(FUType, FUCost, Idx, Inst, Sels);
  }

  float compuateCommonFIBenefit(VASTSelector *Sel) const;

  float getEdgeConsistencyBenefit(EdgeType Edge, EdgeType FIEdge) const;

  float computeCost(CompGraphNode *Src, unsigned SrcBinding,
                    CompGraphNode *Dst, unsigned DstBinding) const;
};

class PreSchedBinding : public VASTModulePass {
  PSBCompGraph *PSBCG;
  typedef std::vector<std::vector<PSBCompNode*> > ClusterVectors;
  ClusterVectors Clusters;
public:
  PreSchedBinding();

  typedef ClusterVectors::const_iterator cluster_iterator;
  cluster_iterator cluster_begin() const { return Clusters.begin(); }
  cluster_iterator cluster_end() const { return Clusters.end(); }

  void performBinding();

  static char ID;
  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
};
}

#endif