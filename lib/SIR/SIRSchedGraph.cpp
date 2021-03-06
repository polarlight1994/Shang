//===------- SIRScheduling.cpp - Scheduling Graph on SIR  ------*- C++ -*-===//
//
//                      The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the SIRSchedUnit and SIRSchedGraph. With these class we
// perform scheduling in High-level Synthesis. Please note that the scheduling
// is based on LLVM IR. After scheduling we will annotate the schedule of the
// LLVM Instructions in form of metadata. And we will rebuild the SIR according
// to the schedule.
//
//===----------------------------------------------------------------------===//

#include "sir/SIRSchedGraph.h"
#include "sir/SIRTimingAnalysis.h"
#include "sir/Passes.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

static int Num = 0;
static int num = 0;

SIRSchedUnit::SIRSchedUnit(unsigned Idx, Type T, BasicBlock *BB)
  : II(0), Schedule(0.0), Idx(Idx), IsScheduled(false),
  T(T), BB(BB), SeqOps(0), CombOp(0) {
  assert(T == SIRSchedUnit::Entry || T == SIRSchedUnit::Exit ||
         T == SIRSchedUnit::BlockEntry && "Unexpected Type for Virtual SUnit!");

  this->Latency = 0.0;
}

SIRSchedUnit::SIRSchedUnit(unsigned Idx, Type T, BasicBlock *BB,
                           SmallVector<SIRSeqOp *, 4> SeqOps)
  : II(0), Schedule(0), Idx(Idx), IsScheduled(false), T(T), BB(BB), SeqOps(SeqOps) {
  this->Latency = 1;
}

SIRSchedUnit::SIRSchedUnit(unsigned Idx, Type T, BasicBlock *BB, SIRSeqOp *SeqOp)
  : II(0), Schedule(0.0), Idx(Idx), IsScheduled(false), T(T), BB(BB), CombOp(0) {
  assert(T == SIRSchedUnit::SlotTransition || T == SIRSchedUnit::SeqSU ||
         T == SIRSchedUnit::PHI && "Unexpected Type for SeqOp SUnit!");

  this->SeqOps.push_back(SeqOp);
  this->Latency = 1.0;
}

SIRSchedUnit::SIRSchedUnit(unsigned Idx, Type T, BasicBlock *BB, Instruction *CombOp)
  : II(0), Schedule(0.0), Idx(Idx), IsScheduled(false), T(T), BB(BB), SeqOps(0),
  CombOp(CombOp) {
  assert(T == SIRSchedUnit::CombSU && "Unexpected Type for CombOp SUnit!");

  this->Latency = 0.0;
}

SIRSchedUnit::SIRSchedUnit() : Idx(0), T(Invalid), BB(0), SeqOps(0) {}

void SIRSchedUnit::EdgeBundle::addEdge(SIRDep NewEdge) {
  // When we add a new edge here, we should choose a appropriate
  // place to insert. The principle is
  // 1) keep the distance of edges in ascending order
  // 2) replace the old edge if new edge's latency is bigger
  unsigned InsertBefore = 0, Size = Edges.size();
  bool NeedToInsert = true;

  SIRDep::Types NewEdgeType = NewEdge.getEdgeType();
  unsigned NewDistance = NewEdge.getDistance();
  float NewLatency = NewEdge.getLatency();

  while (InsertBefore < Size) {
    SIRDep &CurEdge = Edges[InsertBefore];

    // Keep the edges in ascending order.
    if (CurEdge.getDistance() > NewEdge.getDistance())
      break;

    SIRDep::Types CurEdgeType = CurEdge.getEdgeType();
    unsigned CurDistance = CurEdge.getDistance();
    float CurLatency = CurEdge.getLatency();

    // Update the edge with the tighter constraint.
    if (CurDistance == NewDistance && CurLatency < NewLatency) {
      if (NewEdgeType == CurEdgeType) {
        NeedToInsert = false;
        CurEdge = NewEdge;
      }

      break;
    }

    // Now we have CurDistance < NewDistance, NewEdge is masked by
    // CurEdge if NewEdge has a smaller latency than CurEdge.
    if (NewLatency <= CurLatency && NewEdgeType == CurEdgeType)
      return;

    ++InsertBefore;
  }

  assert((InsertBefore <= Edges.size() ||
          (Edges[InsertBefore].getLatency() <= NewEdge.getLatency() &&
           Edges[InsertBefore].getDistance() >= NewEdge.getDistance()))
         && "Bad insert position!");

  // Insert the new edge right before the edge with bigger iterative distance.
  if (NeedToInsert)
    Edges.insert(Edges.begin() + InsertBefore, NewEdge);
}

SIRDep SIRSchedUnit::EdgeBundle::getEdge(unsigned II) const {
  assert(!Edges.empty() && "Unexpected empty edge bundle!");

  SIRDep CurEdge = Edges.front();
  float CurLatency = CurEdge.getLatency(II);

  for (unsigned I = 1, E = Edges.size(); I != E; ++I) {
    SIRDep NewEdge = Edges[I];

    // Find the edge of II with biggest latency.
    float NewLatency = NewEdge.getLatency(II);
    if (NewLatency > CurLatency) {
      CurLatency = NewLatency;
      CurEdge = NewEdge;
    }
  }

  return CurEdge;
}

BasicBlock *SIRSchedUnit::getParentBB() const {
  return BB;
}

void SIRSchedUnit::replaceAllDepWith(SIRSchedUnit *NewSUnit) {
  for (dep_iterator DI = dep_begin(), DE = dep_end(); DI != DE;) {
    SIRSchedUnit *DepSU = *DI;
    ++DI;

    NewSUnit->addDep(DepSU, getEdgeFrom(DepSU));
    removeDep(DepSU);
  }

  for (use_iterator UI = use_begin(), UE = use_end(); UI != UE;) {
    SIRSchedUnit *UseSU = *UI;
    ++UI;

    UseSU->addDep(NewSUnit, UseSU->getEdgeFrom(this));
    UseSU->removeDep(this);
  }
}

void SIRSchedUnit::print(raw_ostream &OS) const {
  switch (T) {
  case SIRSchedUnit::Entry:
    OS << "Entry\n";
    break;
  case SIRSchedUnit::Exit:
    OS << "Exit\n";
    break;
  case SIRSchedUnit::BlockEntry:
    OS << "BBEntry\n";
    break;
  case SIRSchedUnit::PHI:
    OS << "PHI\n";
    break;
  case SIRSchedUnit::SlotTransition: {
    SIRSlotTransition *SST = dyn_cast<SIRSlotTransition>(getSeqOp());
    OS << "SlotTransition\n";
    OS << "Slot transition from Slot#" << SST->getSrcSlot()->getSlotNum()
      << " to Slot#" << SST->getDstSlot()->getSlotNum() << "\n";
    break;
  }
  case SIRSchedUnit::CombSU: {
    Instruction *CombOp = getCombOp();
    OS << "CombOp\n";
    OS << "CombOp contained: ";
    CombOp->print(OS);
    OS << "\n";
    break;
  }
  case SIRSchedUnit::SeqSU: {
    SIRSeqOp *SeqOp = getSeqOp();
    OS << "SeqOp\n";
    OS << "SeqOp contained: assign Value [" << getSeqOp()->getSrc()
       << "] to Reg [" << getSeqOp()->getDst()->getName() << "] in"
       << " Slot#" << getSeqOp()->getSlot()->getSlotNum() << "\n";
    break;
  }
  case SIRSchedUnit::PHIPack: {
    ArrayRef<SIRSeqOp *> SeqOps = getSeqOps();
    OS << "PHIPack\n";
    for (unsigned i = 0; i < SeqOps.size(); ++i) {
      SIRSeqOp *SeqOp = SeqOps[i];

      OS << "SeqOp contained: assign Value [" << SeqOp->getSrc()
         << "] to Reg [" << SeqOp->getDst()->getName() << "] in"
         << " Slot#" << SeqOp->getSlot()->getSlotNum() << "\n";
    }
    break;
  }
  case SIRSchedUnit::MemoryPack: {
    ArrayRef<SIRSeqOp *> SeqOps = getSeqOps();
    OS << "MemoryPack\n";
    for (unsigned i = 0; i < SeqOps.size(); ++i) {
      SIRSeqOp *SeqOp = SeqOps[i];

      OS << "SeqOp contained: assign Value [" << SeqOp->getSrc()
        << "] to Reg [" << SeqOp->getDst()->getName() << "] in"
        << " Slot#" << SeqOp->getSlot()->getSlotNum() << "\n";
    }
    break;
  }
  case SIRSchedUnit::ExitSlotPack: {
    ArrayRef<SIRSeqOp *> SeqOps = getSeqOps();
    OS << "ExitSlotPack\n";
    for (unsigned i = 0; i < SeqOps.size(); ++i) {
      SIRSeqOp *SeqOp = SeqOps[i];

      OS << "SeqOp contained: assign Value [" << SeqOp->getSrc()
        << "] to Reg [" << SeqOp->getDst()->getName() << "] in"
        << " Slot#" << SeqOp->getSlot()->getSlotNum() << "\n";
    }
    break;
  }
  case SIRSchedUnit::Invalid:
    break;
  }

  OS << "Scheduled to " << Schedule;
}

void SIRSchedUnit::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

SIRSchedGraph::SIRSchedGraph(Function &F) : F(F), TotalSUs(2) {
  // Create the entry SU.
  SUnits.push_back(new SIRSchedUnit(0, SIRSchedUnit::Entry, 0));
  // Create the exit SU.
  SUnits.push_back(new SIRSchedUnit(-1, SIRSchedUnit::Exit, 0));
}

SIRSchedGraph::~SIRSchedGraph() {}

SIRSchedUnit *SIRSchedGraph::lookupSU(SIRSeqOp *SeqOp) const {
  SeqOp2SUMapTy::const_iterator at = SeqOp2SUMap.find(SeqOp);

  if (at == SeqOp2SUMap.end())
    return NULL;

  return at->second;
}

ArrayRef<SIRSchedUnit *> SIRSchedGraph::lookupSUs(Value *V) const {
  IR2SUMapTy::const_iterator at = IR2SUMap.find(V);

  if (at == IR2SUMap.end())
    return ArrayRef<SIRSchedUnit *>();

  return at->second;
}

bool SIRSchedGraph::indexSU2IR(SIRSchedUnit *SU, Value *V) {
  // If there are already a map, then we just add the SU into it.
  if (hasSU(V)) {
    IR2SUMap[V].push_back(SU);
    return true;
  }

  // Or we need to create a new map.
  SmallVector<SIRSchedUnit *, 4> SUs;
  SUs.push_back(SU);
  return IR2SUMap.insert(std::make_pair(V, SUs)).second;
}

ArrayRef<SIRSchedUnit *> SIRSchedGraph::lookupSUs(SIRSlot *S) const {
  Slot2SUMapTy::const_iterator at = Slot2SUMap.find(S);

  if (at == Slot2SUMap.end())
    return ArrayRef<SIRSchedUnit *>();

  return at->second;
}

bool SIRSchedGraph::indexSU2Slot(SIRSchedUnit *SU, SIRSlot *S) {
  // If there are already a map, then we just add the SU into it.
  if (hasSU(S)) {
    Slot2SUMap[S].push_back(SU);

    return true;
  }

  // Or we need to create a new map.
  SmallVector<SIRSchedUnit *, 4> SUs;
  SUs.push_back(SU);
  return Slot2SUMap.insert(std::make_pair(S, SUs)).second;
}

SIRSchedUnit *SIRSchedGraph::getLoopSU(BasicBlock *BB) {
  LoopBB2LoopSUMapTy::const_iterator at = LoopBB2LoopSUMap.find(BB);

  if (at == LoopBB2LoopSUMap.end())
    return NULL;

  return at->second;
}

bool SIRSchedGraph::indexLoopSU2LoopBB(SIRSchedUnit *SU, BasicBlock *BB) {
  assert(!hasLoopSU(BB) && "Loop SUnit already existed!");

  return LoopBB2LoopSUMap.insert(std::make_pair(BB, SU)).second;
}

unsigned SIRSchedGraph::getMII(BasicBlock *BB) {
  PipelinedBB2MIIMapTy::const_iterator at = PipelineBB2MIIMap.find(BB);

  if (at == PipelineBB2MIIMap.end())
    return NULL;

  return at->second;
}

bool SIRSchedGraph::indexPipelinedBB2MII(BasicBlock *PipelinedBB, unsigned MII) {
  assert(!hasMII(PipelinedBB) && "MII already existed!");

  return PipelineBB2MIIMap.insert(std::make_pair(PipelinedBB, MII)).second;
}

SIRMemoryBank *SIRSchedGraph::getMemoryBank(SIRSchedUnit *SU) {
  SU2SMBMapTy::const_iterator at = SU2SMBMap.find(SU);

  if (at == SU2SMBMap.end())
    return NULL;

  return at->second;
}

bool SIRSchedGraph::indexMemoryBank2SUnit(SIRMemoryBank *Bank, SIRSchedUnit *SU) {
  assert(!hasMemoryBank(SU) && "Memory Bank already existed!");

  return SU2SMBMap.insert(std::make_pair(SU, Bank)).second;
}

void SIRSchedGraph::toposortCone(SIRSchedUnit *Root,
                                 std::set<SIRSchedUnit *> &Visited,
                                 BasicBlock *BB) {
  if (!Visited.insert(Root).second) return;

  typedef SIRSchedUnit::dep_iterator ChildIt;
  std::vector<std::pair<SIRSchedUnit *, ChildIt> > WorkStack;

  WorkStack.push_back(std::make_pair(Root, Root->dep_begin()));

  while (!WorkStack.empty()) {
    SIRSchedUnit *U = WorkStack.back().first;
    ChildIt I = WorkStack.back().second;

    // Visit the current node if all its dependencies are visited.
    if (U->isBBEntry() || I == U->dep_end()) {
      WorkStack.pop_back();
      SUnits.splice(SUnits.end(), SUnits, U);
      continue;
    }

    ++WorkStack.back().second;

    SIRSchedUnit *Child = *I;

    // The SyncDep in created only to constraint the PHI and ExitSlot-
    // Transition into last step, we don't want it to affect the
    // sort order.
    if (I.getEdgeType() == SIRDep::SyncDep)
      continue;

    // We have reach the top SUnit.
    if (Child->isEntry() || Child->getParentBB() != BB)
      continue;

    // Ignore the ExitSlotPack SUnit in this BB, since this dependency
    // will be a back-edge formed a loop.
    if (Child->isExitSlotPack())
      continue;

    // Ignore the PHI SUnit in this BB when it is also a back-edge
    // data dependency. To be noted that, the dependency from PHI
    // to ExitSlotPack is not back-edge.
    if (Child->isPHI() || Child->isPHIPack())
      if (!U->isExitSlotPack())
        continue;

    // Do not visit the same node twice!
    if (!Visited.insert(Child).second) continue;

    WorkStack.push_back(std::make_pair(Child, Child->dep_begin()));
  }
}

void SIRSchedGraph::topologicalSortSUs() {
  SIRSchedUnit *Entry = getEntry(), *Exit = getExit();
  assert(Entry->isEntry() && Exit->isExit() && "Bad order!");

  // Ensure the Entry is the first.
  SUnits.splice(SUnits.end(), SUnits, Entry);

  // Handle the SUnits located in Slot0r specially since they have
  // no ParentBB.
  bb_iterator at = BBMap.find(NULL);
  if (at != BBMap.end()) {
    MutableArrayRef<SIRSchedUnit *> SUsInSlot0r(at->second);
    for (unsigned i = 0; i < SUsInSlot0r.size(); ++i) {
      SIRSchedUnit *SUnitInSlot0r = SUsInSlot0r[i];

      // Ensure all SUnit in Slot0r is in the front of others.
      SUnits.splice(SUnits.end(), SUnits, SUnitInSlot0r);
    }
  }

  std::set<SIRSchedUnit *> Visited;

  ReversePostOrderTraversal<BasicBlock*> RPO(&F.getEntryBlock());
  typedef ReversePostOrderTraversal<BasicBlock*>::rpo_iterator bb_top_iterator;

  for (bb_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    Visited.clear();
    BasicBlock *BB = *I;

    bb_iterator at = BBMap.find(BB);
    if (at == BBMap.end())
      continue;

    MutableArrayRef<SIRSchedUnit *> SUs(at->second);
    for (unsigned i = 0; i < SUs.size(); ++i)
      toposortCone(SUs[i], Visited, BB);
  }

  // Ensure the Exit is the last.
  SUnits.splice(SUnits.end(), SUnits, Exit);

  unsigned Idx = 0;
  for (iterator I = begin(), E = end(); I != E; ++I)
    I->Idx = Idx++;

  assert(Idx == size() && "Topological sort is not applied to all SU?");
  assert(getEntry()->isEntry() && getExit()->isExit() && "Broken TopSort!");
}

ArrayRef<SIRSchedUnit *> SIRSchedGraph::getSUsInBB(BasicBlock *BB) const {
  const_bb_iterator at = BBMap.find(BB);

  assert(at != BBMap.end() && "BB not found!");

  return at->second;
}

SIRSchedUnit *SIRSchedGraph::createSUnit(BasicBlock *ParentBB, SIRSchedUnit::Type T) {
  assert(T == SIRSchedUnit::BlockEntry && "Unexpected Type of SUnit!");

  SIRSchedUnit *U = new SIRSchedUnit(TotalSUs++, T, ParentBB);
  // Insert the newly create SU before the exit.
  SUnits.insert(SUnits.back(), U);
  // Index the SUnit to the corresponding BB.
  BBMap[ParentBB].push_back(U);

  return U;
}

SIRSchedUnit *SIRSchedGraph::createSUnit(BasicBlock *ParentBB, SIRSchedUnit::Type T,
                                         SIRSeqOp *SeqOp) {
  assert(T == SIRSchedUnit::PHI || T == SIRSchedUnit::SlotTransition ||
         T == SIRSchedUnit::SeqSU && "Unexpected Type of SUnit!");

  SIRSchedUnit *U = new SIRSchedUnit(TotalSUs++, T, ParentBB, SeqOp);
  // Insert the newly create SU before the exit.
  SUnits.insert(SUnits.back(), U);
  // Index the SUnit to the corresponding BB.
  BBMap[ParentBB].push_back(U);
  // Index the SUnit to the corresponding SeqOp.
  SeqOp2SUMap.insert(std::make_pair(SeqOp, U));

  return U;
}

SIRSchedUnit *SIRSchedGraph::createSUnit(BasicBlock *ParentBB, SIRSchedUnit::Type T,
                                         SmallVector<SIRSeqOp *, 4> SeqOps) {
  assert(T == SIRSchedUnit::PHIPack || T == SIRSchedUnit::MemoryPack ||
         T == SIRSchedUnit::ExitSlotPack && "Unexpected Type of SUnit!");

  SIRSchedUnit *U = new SIRSchedUnit(TotalSUs++, T, ParentBB, SeqOps);
  // Insert the newly create SU before the exit.
  SUnits.insert(SUnits.back(), U);

  return U;
}

SIRSchedUnit *SIRSchedGraph::createSUnit(BasicBlock *ParentBB, SIRSchedUnit::Type T,
                                         Instruction *CombOp) {
  assert(T == SIRSchedUnit::CombSU && "Unexpected Type of SUnit!");

  SIRSchedUnit *U = new SIRSchedUnit(TotalSUs++, T, ParentBB, CombOp);
  // Insert the newly create SU before the exit.
  SUnits.insert(SUnits.back(), U);
  // Index the SUnit to the corresponding BB.
  BBMap[ParentBB].push_back(U);

  return U;
}

void SIRSchedGraph::replaceAllUseWith(SIRSchedUnit *OldSU, SIRSchedUnit *NewSU) {
  OldSU->replaceAllDepWith(NewSU);

  assert(OldSU->dep_empty() && OldSU->use_empty() && "Dependencies not clean!");

  BasicBlock *BB = OldSU->getParentBB();
  SIRSeqOp *SeqOp = OldSU->getSeqOp();
  SIRSlot *S = SeqOp->getSlot();
  Value *V = SeqOp->getLLVMValue();

  SeqOp2SUMap[SeqOp] = NewSU;

  typedef SmallVector<SIRSchedUnit *, 4>::iterator iterator;

  SmallVector<SIRSchedUnit *, 4> &SlotSUs = Slot2SUMap[S];
  for (unsigned i = 0; i < SlotSUs.size(); ++i) {
    if (OldSU == SlotSUs[i]) {
      SlotSUs[i] = NewSU;
      break;
    }
  }

  SmallVector<SIRSchedUnit *, 4> &ValueSUs = IR2SUMap[V];
  for (unsigned i = 0; i < ValueSUs.size(); ++i) {
    if (OldSU == ValueSUs[i]) {
      ValueSUs[i] = NewSU;
      break;
    }
  }

  SmallVector<SIRSchedUnit *, 4> &BBSUs = BBMap[BB];
  bool AlreadyExist = false;;
  for (unsigned i = 0; i < BBSUs.size(); ++i) {
    if (NewSU == BBSUs[i]) {
      AlreadyExist = true;
      break;
    }
  }
  for (unsigned i = 0; i < BBSUs.size(); ++i) {
    if (OldSU == BBSUs[i]) {
      if (!AlreadyExist)
        BBSUs[i] = NewSU;
      else
        BBSUs.erase(BBSUs.begin() + i);

      break;
    }
  }

  Num++;
}

void SIRSchedGraph::deleteUselessSUnit(SIRSchedUnit *U) {
  SUnits.erase(U);

  TotalSUs--;
}

void SIRSchedGraph::gc() {
  // Delete all the useless SUnit.
  for (iterator I = begin(), E = end(); I != E;) {
    SIRSchedUnit *U = I++;

    if (U->dep_empty() && U->use_empty()) {
      num++;
      deleteUselessSUnit(U);
    }
  }

  assert(Num == num && "Still some useless SUnits not deleted!");
}

void SIRSchedGraph::resetSchedule() {
  // Reset all SUnits in graph.
  for (iterator I = begin(), E = end(); I != E; ++I) {
    SIRSchedUnit *SUnit = I;
    I->resetSchedule();
  }
}

