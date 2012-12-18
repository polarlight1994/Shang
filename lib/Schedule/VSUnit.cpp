//===------------ VSUnit.cpp - Translate LLVM IR to VSUnit  -----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VSUnit class, which represent the basic atom
// operation in hardware.
//
//===----------------------------------------------------------------------===//

#include "VSUnit.h"
#include "ScheduleDOT.h"

#include "vtm/VerilogBackendMCTargetDesc.h"
#include "vtm/SynSettings.h"
#include "vtm/VFInfo.h"

#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vtm-sunit"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<bool>
ScheduleDataPathALAP("vtm-schedule-datapath-alap",
                     cl::desc("Schedule datapath operaton As Last As Possible "
                              "to allow more effecient resource sharing"),
                     cl::init(true)),
// FIXME: When multi-cycles chain is disabled, also set the chaining threshold
// of all functional units to 0 to avoid chaining.
DisableMultiCyclesChain("vtm-disable-multi-cycles-chain",
                        cl::desc("Disable multi-cycles chaining "
                                 "(manually setting all chaining threshold to "
                                 "0 is need)"),
                        cl::init(false));

STATISTIC(NumSUs, "Number of scheduling units");
STATISTIC(NumEdges, "Number of edges between scheduling units");
STATISTIC(NumCPSUs, "Number of control-path scheduling units");
STATISTIC(NumCPEdges, "Number of control-path edges between scheduling units");
STATISTIC(NumDPSUs, "Number of data-path scheduling units");
STATISTIC(NumDPEdges, "Number of data-path edges between scheduling units");

//===----------------------------------------------------------------------===//
void VSchedGraph::print(raw_ostream &OS) const {
  getEntryBB()->dump();
}

void VSchedGraph::dump() const {
  print(dbgs());
}

bool VSchedGraph::rememberLoopOp(MachineInstr *MI) {
  assert(MI->getDesc().isTerminator() && "Bad instruction!");

  if (!VInstrInfo::isBrCndLike(MI->getOpcode())) return false;

  if (MI->getOperand(1).getMBB() != getEntryBB()) return false;

  // Ok, remember this instruction as self enable.
  LoopOp.setPointer(MI);
  return true;
}

bool VSchedGraph::isLoopPHIMove(MachineInstr *MI) {
  assert(MI->getOpcode() == VTM::VOpMvPhi && "Bad opcode!");

  return MI->getOperand(2).getMBB() == getEntryBB() && enablePipeLine();
}

unsigned VSchedGraph::getStepsFromEntry(const MachineInstr *DstInstr) const {
  assert(DstInstr && "DstInstr should not be null!");
  // Any Instruction can schedule to the same slot with the BBEntry if dangling
  // operation is allowed.
  if (AllowDangling) return 0;

  const MCInstrDesc &DstTID = DstInstr->getDesc();
  unsigned DstOpC = DstTID.getOpcode();

  // Do not worry about PHI Nodes, their will be eliminated at the register
  // allocation pass.
  if (DstInstr->getOpcode() == VTM::PHI) return 0;

  // Schedule datapath operation right after the first control slot.
  if (VInstrInfo::isDatapath(DstOpC)) return 0;

  // Do not schedule function unit operation to the first state at the moment
  // there may be potential resource conflict: The end slot may be at the middle
  // of a BB in a pipelined loop body, in that case, any FU can be actived by
  // the alias slot.
  if (!VInstrInfo::hasTrivialFU(DstOpC) || VInstrInfo::countNumRegUses(DstInstr))
    return 1;

  return 0;
}

void VSchedGraph::verifySUDataPath(const VSUnit *SU) const {
  for (VSUnit::const_dep_iterator DI = dp_begin(SU), DE = dp_end(SU);
       DI != DE; ++DI) {
    ++NumDPEdges;
    ++NumEdges;
  }

  if (SU->isDatapath() && cuse_empty(SU) && duse_empty(SU))
    llvm_unreachable("Unexpected dangling datapath SU!");
}

void VSchedGraph::verifySUControlPath(const VSUnit *SU) const {
  for (VSUnit::const_dep_iterator DI = cp_begin(SU), DE = cp_end(SU);
       DI != DE; ++DI) {
    ++NumCPEdges;
    ++NumEdges;
  }
}

void VSchedGraph::verify() const {
  //if (!cp_empty(getEntryRoot()) || !dp_empty(getEntryRoot()))
  //  llvm_unreachable("Entry root should not have any dependence!");
  //if (!cuse_empty(getExitRoot()) || !duse_empty(getExitRoot()) )
  //  llvm_unreachable("Exit root should not have any use!");

  for (const_iterator I = cp_begin(this), E = cp_end(this); I != E; ++I) {
    ++NumSUs;
    ++NumCPSUs;
    verifySUControlPath(*I);
    verifySUDataPath(*I);
  }

  for (const_iterator I = dp_begin(this), E = dp_end(this); I != E; ++I) {
    ++NumSUs;
    ++NumDPSUs;
    verifySUDataPath(*I);
  }
}

VSUnit *VSchedGraph::createVSUnit(InstPtrTy Ptr, unsigned fuid) {
  VSUnit *SU = new VSUnit(NextSUIdx++, fuid);

  MachineInstr *MI = Ptr;
  bool mapped = mapMI2SU(Ptr, SU, MI ? DLInfo.getStepsToFinish(MI) : 0);
  (void) mapped;
  assert(mapped && "Cannot add SU to the inst2su map!");

  if (SU->isDatapath()) DPSUs.push_back(SU);
  else                  CPSUs.push_back(SU);
  return SU;
}

VSchedGraph::iterator
VSchedGraph::mergeSUsInSubGraph(VSchedGraph &SubGraph) {
  // 1. Merge the MI2SU map.
  // Prevent the virtual exit root from being inserted to the current MI2SU map.
  SubGraph.InstToSUnits.erase(SubGraph.getExitRoot()->getRepresentativePtr());

  InstToSUnits.insert(SubGraph.InstToSUnits.begin(),
                      SubGraph.InstToSUnits.end());

  // 2. Add the SUs in subgraph to the SU list of current SU list.
  unsigned OldCPStart = num_cps(this);
  unsigned NewIdxBase = NextSUIdx;
  assert(num_sus() == NextSUIdx - FirstSUIdx && "Index mis-matched!");
  VSUnit *Terminator = SubGraph.lookUpTerminator(SubGraph.getEntryBB());
  assert(Terminator && "Terminator SU not found!");
  // We need to clear the dependencies of the terminator before we adding
  // local schedule constraint.
  Terminator->cleanCPDepAndUse();
  unsigned TerminatorSlot = Terminator->getSlot();

  for (iterator I = cp_begin(&SubGraph), E = cp_end(&SubGraph); I != E; ++I) {
    VSUnit *U = *I;

    // Ignore the virtual exit root.
    if (U == SubGraph.getExitRoot()) continue;

    // Update the index of the scheduled SU.
    CPSUs.push_back(U->updateIdx(NewIdxBase + U->getIdx() - FirstSUIdx));
    ++NextSUIdx;

    // We may need to build a fixed timing dependencies according to the
    // schedule of U.
    unsigned USlot = U->getSlot();
    U->resetSchedule();

    // All scheduled control SU has fixed a timing constraint.
    U->setHasFixedTiming();

    // Do not add loop.
    if (U == Terminator) continue;

    assert(USlot && "Unexpected unscheduled SU!");
    unsigned ScheduleOffset = TerminatorSlot - USlot;
    // We need to build the new dependencies.
    U->cleanCPDepAndUse();
    // A dependencies to constrain the SU with local schedule.
    VDEdge FixedTimingEdge = VDEdge::CreateFixTimingConstraint(ScheduleOffset);
    Terminator->addDep<true>(U, FixedTimingEdge);
  }

  for (iterator I = dp_begin(&SubGraph), E = dp_end(&SubGraph); I != E; ++I) {
    VSUnit *U = *I;

    // Update the index of the scheduled SU.
    DPSUs.push_back(U->updateIdx(NewIdxBase + U->getIdx() - FirstSUIdx));
    ++NextSUIdx;
  }

  SubGraph.CPSUs.clear();
  // Leave the exit root of the SubGraph in its CPSU list, so it will be deleted.
  SubGraph.CPSUs.push_back(getExitRoot());
  SubGraph.DPSUs.clear();

  // 3. Merge the terminator map.
  BBInfoMap.insert(BBInfoMap.end(),
                   SubGraph.BBInfoMap.begin(), SubGraph.BBInfoMap.end());

  assert(NextSUIdx == Terminator->getIdx() + 1u && "Index mis-matched!");
  // Return the iterator point to the first SU of the subgraph, and
  // we need to skip the entry node of the block, we need add 1 after OldCPStart.
  return CPSUs.begin() + OldCPStart + 1;
}

void VSchedGraph::topologicalSortCPSUs() {
  unsigned Idx = 0;
  VSUnit *Exit = getExitRoot();
  typedef po_iterator<VSUnit*, SmallPtrSet<VSUnit*, 64>, false,
                      VSUnitDepGraphTraits<true> >
          top_it;

  for (top_it I = top_it::begin(Exit), E = top_it::end(Exit); I != E; ++I)
    CPSUs[Idx++] = *I;

  assert(Idx == num_cps(this) && "Bad topological sort!");
}

VSUnit *VSchedGraph::createTerminator(const MachineBasicBlock *MBB,
                                      const MachineDominatorTree *MDT) {
  BBInfo Info;
  Info.Exit = new VSUnit(NextSUIdx++, 0);
  CPSUs.push_back(Info.Exit);
  // Initialize the rest of the BBInfo.
  Info.Entry = lookupSUnit(MBB);
  assert(Info.Entry && "Create terminator before creating entry!");
  Info.II = 0;
  Info.IDomIdx = 0;
  Info.SPDFromEntry = UINT32_MAX;

  // Set the IDomIdx if MDT is provided.
  if (MDT) {
    MachineDomTreeNode *N = MDT->getNode(const_cast<MachineBasicBlock*>(MBB));
    if (MachineDomTreeNode *IDom = N->getIDom())
      Info.IDomIdx = IDom->getBlock()->getNumber();
  }

  // Add the current BBInfo to BBInfoMap.
  BBInfoMap.push_back(Info);
  assert((int(BBInfoMap.size()) == MBB->getNumber() + 1
          || getEntryBB()->getNumber() == MBB->getNumber())
         && "BBInfoMap's index not synchronized!");
  return Info.Exit;
}

void VSchedGraph::resetCPSchedule() {
  for (iterator I = cp_begin(this), E = cp_end(this); I != E; ++I) {
    VSUnit *U = *I;
    U->resetSchedule();
  }

  getEntryRoot()->scheduledTo(EntrySlot);
}

void VSchedGraph::resetDPSchedule() {
  for (iterator I = dp_begin(this), E = dp_end(this); I != E; ++I) {
    VSUnit *U = *I;
    U->resetSchedule();
  }
}

bool VSchedGraph::scheduleLoop() {
  MachineBasicBlock *MBB = getEntryBB();
  MachineFunction *F = MBB->getParent();
  DEBUG(dbgs() << "Try to pipeline MBB#" << MBB->getNumber()
               << " MF#" << F->getFunctionNumber() << '\n');
  IterativeModuloScheduling Scheduler(*this);

  unsigned ResMII = Scheduler.computeResMII();
  Scheduler.setCriticalPathLength(ResMII);

  // Ensure us can schedule the critical path.
  while (!Scheduler.scheduleCriticalPath())
    Scheduler.lengthenCriticalPath();
  DEBUG(dbgs() << "Pipelining BB# " << MBB->getNumber()
               << " in function " << MBB->getParent()->getFunction()->getName()
               << " #" << MBB->getParent()->getFunctionNumber() << '\n');

  unsigned MII = Scheduler.computeRecMII(ResMII);

  Scheduler.setMII(MII);
  unsigned OriginalCriticalPathLength = std::max(Scheduler.getCriticalPathLength(),
                                                 Scheduler.getMII());
  // Relax the critical path constraint, so that we can focus on finding MII.
  Scheduler.setCriticalPathLength(8 * OriginalCriticalPathLength);

  DEBUG(dbgs() << "MII: " << Scheduler.getMII() << "...");
  while (!Scheduler.scheduleCriticalPath()) {
    // Make sure MII smaller than the critical path length.
    if (2 * Scheduler.getMII() < Scheduler.getCriticalPathLength())
      Scheduler.increaseMII();
    else
      Scheduler.lengthenCriticalPath();
  }

  assert(Scheduler.getMII() <= Scheduler.getCriticalPathLength()
         && "MII bigger then Critical path length!");

  for (;;) {
    switch (Scheduler.scheduleLoop()) {
    case IterativeModuloScheduling::Success:{
      // Fail to pipeline the BB if the II is not small enough.
      if (7 * Scheduler.getMII() >= 8 * Scheduler->getTotalSlot(MBB))
        return false;

      DEBUG(dbgs() << "SchedII: " << Scheduler.getMII()
        << " Latency: " << getTotalSlot(MBB) << '\n');
      assert(getLoopOp()->getSlot() - EntrySlot == Scheduler.getMII()
        && "LoopOp was not scheduled to the right slot!");
      assert(getLoopOp()->getSlot() <= getEndSlot(MBB)
        && "Expect MII is not bigger then critical path length!");

      BBInfo &Info = getBBInfo(MBB);
      assert(Info.II == 0 && "MBB already pipelined?");
      Info.II = Scheduler.getMII();
      return true;
    }
    case IterativeModuloScheduling::MIITooSmall:{
      Scheduler.increaseMII();
      // Make sure MII smaller than the critical path length.
      if (Scheduler.getMII() <= Scheduler.getCriticalPathLength())
        continue;
      break;
    }
    case IterativeModuloScheduling::Unknown:{
      if (Scheduler.getMII() < Scheduler.getCriticalPathLength()) {
        Scheduler.increaseMII();
        continue;
      }
      break;
    }
    }

    Scheduler.lengthenCriticalPath();
  }

  llvm_unreachable("Control-flow will never reach here!");
  return false;
}

void VSchedGraph::viewCPGraph() {
  VSchedGraphWrapper<true> G(this);
  ViewGraph(G, "Control-path-Dependencies-Graph");
}

void VSchedGraph::viewDPGraph() {
  VSchedGraphWrapper<false> G(this);
  ViewGraph(G, "Data-path-Dependencies-Graph");
}

void VSchedGraph::clearDanglingFlagForTree(VSUnit *Root) {
  // Perform depth first search to find node that reachable from Root.
  std::vector<std::pair<VSUnit*, VSUnit::dep_iterator> > WorkStack;
  WorkStack.push_back(std::make_pair(Root, dp_begin(Root)));
  Root->setIsDangling(false);
  MachineBasicBlock *RootBB = Root->getParentBB();

  while (!WorkStack.empty()) {
    VSUnit *U = WorkStack.back().first;
    VSUnit::dep_iterator ChildIt = WorkStack.back().second;

    if (ChildIt == U->dep_end<false>()) {
      WorkStack.pop_back();
      continue;
    }

    VSUnit *ChildNode = *ChildIt;
    ++WorkStack.back().second;

    if (!ChildNode->isDangling() || ChildNode->getParentBB() != RootBB) continue;

    // If the node is reachable from exit, then it is not dangling.
    ChildNode->setIsDangling(false);
    WorkStack.push_back(std::make_pair(ChildNode, dp_begin(ChildNode)));
  }
}

void VSchedGraph::addSoftConstraintsToBreakChains(SDCSchedulingBase &S) {}

void VSchedGraph::scheduleControlPath() {
  SDCScheduler<true> Scheduler(*this);

  Scheduler.buildTimeFrameAndResetSchedule(true);
  BasicLinearOrderGenerator::addLinOrdEdge(Scheduler);
  // Build the step variables, and no need to schedule at all if all SUs have
  // been scheduled.
  if (!Scheduler.createLPAndVariables()) return;

  Scheduler.buildASAPObject(1.0);
  //Scheduler.buildOptSlackObject(0.0);

  bool success = Scheduler.schedule();
  assert(success && "SDCScheduler fail!");
  (void) success;
}

void VSchedGraph::scheduleDatapath() {
  SDCScheduler<false> Scheduler(*this);

  Scheduler.buildTimeFrame();

  if (Scheduler.createLPAndVariables()) {
    // Add soft constraints to break the chain.
    //addSoftConstraintsToBreakChains(Scheduler);
    //Scheduler.addSoftConstraintsPenalties(1.0);

    // Schedule them ALAP.
    Scheduler.buildASAPObject(-1.0);
    bool succ = Scheduler.schedule();
    assert(succ && "Cannot schedule the data-path!");
    (void) succ;
  }
}

//===----------------------------------------------------------------------===//

void VSUnit::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

void VDEdge::print(raw_ostream &OS) const {}

void VSUnit::EdgeBundle::addEdge(VDEdge NewEdge) {
  assert(NewEdge.getEdgeType() != VDEdge::FixedTiming
         && "Fixed timing constraint cannot be added!");
  unsigned InsertBefore = 0, Size = Edges.size();
  bool NeedToInsert = true;

  while (InsertBefore < Size) {
    VDEdge &CurEdge = Edges[InsertBefore];
    // Keep the edges in ascending order.
    if (CurEdge.getDistance() > NewEdge.getDistance())
      break;

    if (CurEdge.getDistance() == NewEdge.getDistance()) {
      // Update the edge with the tighter constraint.
      if (CurEdge.getLatency() < NewEdge.getLatency()) {
        NeedToInsert = false;
        CurEdge = NewEdge;
        break;
      }

      //assert(NewEdge.getEdgeType() != VDEdge::ChainSupporting
      //       && "Supporting edge lost!");
      return;
    }

    // Now we have NewEdge.getDistance() > CurEdge.getDistance(), NewEdge is
    // masked by CurEdge if NewEdge has a smaller latency than CurEdge.
    if (NewEdge.getLatency() <= CurEdge.getLatency())
      return;
    
    ++InsertBefore;
  }

  // Insert the new edge right before the edge with bigger iterative distance.
  if (NeedToInsert) Edges.insert(Edges.begin() + InsertBefore, NewEdge);

  for (unsigned i = Edges.size() - 1, e = InsertBefore; i > e; --i) {
    VDEdge &CurEdge = Edges[i];
    // Now we have NewEdge.getDistance() < CurEdge.getDistance(), CurEdge is
    // masked by NewEdge if CurEdge has a smaller latency.
    if (CurEdge.getLatency() <= NewEdge.getLatency())
      // CurEdge is masked by NewEdge
      Edges.erase(Edges.begin() + i);
  }
}

VDEdge &VSUnit::EdgeBundle::getEdge(unsigned II /* = 0 */) {
  assert(Edges.size() && "Unexpected empty edge bundle!");
  VDEdge *CurEdge = &Edges.front();
  int Latency = CurEdge->getLatency(II);

  // Zero II means we should ignore the loop-carried dependencies.
  if (II == 0 && CurEdge->getDistance() == 0)
    return *CurEdge;

  for (unsigned i = 1, e = Edges.size(); i != e; ++i) {
    VDEdge &Edge = Edges[i];
    if (II == 0 && Edge.getDistance() == 0) return Edge;

    // Find the edge with biggest latency.
    int NewLatency = Edge.getLatency();
    if (NewLatency > Latency) {
      Latency = NewLatency;
      CurEdge = &Edge;
    }
  }

  return *CurEdge;
}

VSUnit::VSUnit(unsigned short Idx, uint16_t FUNum)
  : SchedSlot(0), IsDangling(true), HasFixedTiming(false), InstIdx(Idx),
    FUNum(FUNum) {
  assert(Idx > VSchedGraph::NullSUIdx && "Bad index!");
}

VSUnit::VSUnit(MachineBasicBlock *MBB, uint16_t Idx)
  : SchedSlot(0), IsDangling(true), HasFixedTiming(false), InstIdx(Idx),
    FUNum(0) {
  assert(Idx > VSchedGraph::NullSUIdx && "Bad index!");
  Instrs.push_back(MBB);
  latencies.push_back(0);
}

VSUnit *VSUnit::updateIdx(unsigned short Idx) {
  assert(Idx > VSchedGraph::NullSUIdx && "Bad index!");

  InstIdx = Idx;
  return this;
}

unsigned VSUnit::countValDeps() const {
  unsigned DepCounter = 0;

  //for(const_dep_iterator I = cpdep_begin(), E = cpdep_end(); I != E; ++I) {
  //  if(I.getEdgeType() != VDEdge::ValDep) continue;

  //  ++DepCounter;
  //}

  return DepCounter;
}

unsigned VSUnit::countValUses() const {
  unsigned DepCounter = 0;

  //for(const_use_iterator I = cpuse_begin(), E = cpuse_end(); I != E; ++I){
  //  const VSUnit* V =*I;
  //  if(V->getCPEdgeFrom(this).getEdgeType() != VDEdge::ValDep) continue;

  //  ++DepCounter;
  //}

  return DepCounter;
}

unsigned VSUnit::getOpcode() const {
  if (MachineInstr *I = getRepresentativePtr())
    return I->getOpcode();

  return VTM::INSTRUCTION_LIST_END;
}

void VSUnit::scheduledTo(unsigned slot) {
  assert(slot && "Can not schedule to slot 0!");
  SchedSlot = slot;
  // TODO: Assert the schedule is not locked?
}

VFUs::FUTypes VSUnit::getFUType() const {
  if (MachineInstr *Instr = getRepresentativePtr())
    return VInstrInfo::getFUType(Instr->getOpcode());

  return VFUs::Trivial;
}

bool VSUnit::isDatapath() const {
  if (MachineInstr *Instr = getRepresentativePtr())
    return VInstrInfo::isDatapath(Instr->getOpcode());

  return false;
}

int8_t VSUnit::getLatencyFor(MachineInstr *MI) const {
  const_instr_iterator at = std::find(instr_begin(), instr_end(), MI);
  assert(at != instr_end() && "Instruction not exist!");
  return getLatencyAt(at - instr_begin());
}

int VSUnit::getLatencyFrom(MachineInstr *SrcMI, int SrcLatency) const{
  int Latency = SrcLatency;
  if (SrcMI != getRepresentativePtr()) {
    Latency += getLatencyFor(SrcMI);
  }

  return Latency;
}

void VSUnit::print(raw_ostream &OS) const {
  OS << "[" << getIdx() << "] MBB#" << getParentBB()->getNumber() << ' ';

  for (unsigned i = 0, e = num_instrs(); i < e; ++i) {
    InstPtrTy Ptr = getPtrAt(i);
    if (MachineInstr *Instr = Ptr.dyn_cast_mi()) {
      const TargetInstrInfo *TII = Instr->getParent()->getParent()->getTarget()
                                         .getInstrInfo();
      OS << TII->getName(Instr->getDesc().getOpcode()) << ' ';
      if (!Instr->isPseudo())
        OS << *VInstrInfo::getTraceOperand(Instr);
      if (i) OS << ' ' << int(getLatencyAt(i));
      OS << '\n';
      DEBUG(OS << *Instr << '\n');
    }
  }

  OS << getFUId() << "\nAt slot: " << getSlot() << " latency: " << getLatency();
  if (isDangling()) OS << " <Dangling>";
}
