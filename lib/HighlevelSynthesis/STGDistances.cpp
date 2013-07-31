//===--- STGDistances.cpp - Calculate the distances in the STG ---*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the BBLandingSlot pass. The BBLandingSlot pass compute
// the landing slot, i.e. the first slot that is reachable by all predecessor of
// the BasicBlock. Sometimes there is more than 1 landing slots for a specified
// BasicBlock because of the CFG folding in Scheduling.
//
//===----------------------------------------------------------------------===//

#include "shang/Passes.h"
#include "shang/VASTModule.h"
#include "shang/STGDistances.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CFG.h"
#define DEBUG_TYPE "vast-stg-shortest-path"
#include "llvm/Support/Debug.h"

using namespace llvm;

STATISTIC(NumSTPIterations,
          "Number of iterations in the shortest path algorithm");

namespace llvm {
template<typename SubClass>
struct STGDistanceImpl : public STGDistanceBase {
  void run(VASTModule &VM);
};

struct ShortestPathImpl : public STGDistanceImpl<ShortestPathImpl> {
  bool updateDistance(unsigned DistanceSrcThuDst,
                      unsigned DstSlot, unsigned SrcSlot);

};
}

void STGDistanceBase::initialize(VASTModule &VM) {
  // Initialize the neighbor weight.
  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator I = VM.slot_begin(), E = VM.slot_end(); I != E; ++I) {
    VASTSlot *Src = I;

    typedef VASTSlot::succ_iterator succ_iterator;
    for (succ_iterator SI = Src->succ_begin(), SE = Src->succ_end();
         SI != SE; ++SI) {
      VASTSlot::EdgePtr Dst = *SI;
      assert(Src != Dst && "Unexpected loop!");
      DistanceMatrix[Dst->SlotNum][Src->SlotNum] = Dst.getDistance();
    }
  }
}

unsigned STGDistanceBase::getDistance(unsigned From, unsigned To) const  {
  DenseMap<unsigned, DenseMap<unsigned, unsigned> >::const_iterator
    to_at = DistanceMatrix.find(To);

  if (to_at == DistanceMatrix.end()) return STGDistances::Inf;

  DenseMap<unsigned, unsigned>::const_iterator from_at = to_at->second.find(From);

  if (from_at == to_at->second.end()) return STGDistances::Inf;

  return from_at->second;
}

void STGDistanceBase::print(raw_ostream &OS, VASTModule &VM) const {
  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator I = VM.slot_begin(), IE = VM.slot_end(); I != IE; ++I) {
    for (slot_iterator J = VM.slot_begin(), JE = VM.slot_end(); J != JE; ++J) {
      OS << '[' << I->SlotNum << ',' << J->SlotNum << "] = ";
      unsigned Distance = getDistance(I->SlotNum, J->SlotNum);
      if (Distance == STGDistances::Inf) OS << "Inf";
      else                                  OS << Distance;
      OS << ",\t";
    }
    OS << '\n';
  }
}

//===----------------------------------------------------------------------===//

template<typename SubClass>
void STGDistanceImpl<SubClass>::run(VASTModule &VM) {
  reinterpret_cast<SubClass*>(this)->initialize(VM);

  // Visit the slots in topological order.
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >
    RPO(VM.getStartSlot());

  typedef
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >::rpo_iterator
  slot_top_iterator;

  bool changed = true;

  while (changed) {
    changed = false;
    ++NumSTPIterations;

    // Use the Floyd Warshal algorithm to compute the shortest path.
    for (slot_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
      VASTSlot *Dst = *I;
      unsigned DstSlot = Dst->SlotNum;

      typedef VASTSlot::pred_iterator pred_iterator;
      for (pred_iterator PI = Dst->pred_begin(), PE = Dst->pred_end();
           PI != PE; ++PI) {
        VASTSlot::EdgePtr Thu = *PI;
        unsigned EdgeDistance = Thu.getDistance();

        DenseMap<unsigned, unsigned> &Srcs = DistanceMatrix[Thu->SlotNum];
        typedef DenseMap<unsigned, unsigned>::iterator from_iterator;
        for (from_iterator FI = Srcs.begin(), FE = Srcs.end(); FI != FE; ++FI) {
          //D[i][j] = min( D[i][j], D[i][k] + D[k][j]
          unsigned SrcSlot = FI->first;
          unsigned DistanceSrcThu = FI->second;
          unsigned DistanceSrcThuDst = DistanceSrcThu + EdgeDistance;
          changed |=
            reinterpret_cast<SubClass*>(this)->updateDistance(DistanceSrcThuDst,
                                                              DstSlot, SrcSlot);
        }
      }
    }
  }
}

//===----------------------------------------------------------------------===//
bool ShortestPathImpl::updateDistance(unsigned DistanceSrcThuDst,
                                      unsigned DstSlot, unsigned SrcSlot) {
  unsigned DistanceSrcDst = getDistance(SrcSlot, DstSlot);
  if (DistanceSrcThuDst < DistanceSrcDst) {
    DistanceMatrix[DstSlot][SrcSlot] = DistanceSrcThuDst;
    return true;
  }

  return false;
}

//===----------------------------------------------------------------------===//
char STGDistances::ID = 0;
char &llvm::STGDistancesID = STGDistances::ID;
const unsigned STGDistances::Inf = UINT16_MAX;

INITIALIZE_PASS(STGDistances, "vast-stg-distances",
                "Compute the distances in the STG", false, true)

STGDistances::STGDistances() : VASTModulePass(ID), SPImpl(0), VM(0) {
  initializeSTGDistancesPass(*PassRegistry::getPassRegistry());
}

void STGDistances::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.setPreservesAll();
}

template<typename T>
static void DeleteAndReset(T *&Ptr) {
  if (Ptr) {
    delete Ptr;
    Ptr = 0;
  }
}

void STGDistances::releaseMemory() {
  DeleteAndReset(SPImpl);
  LandingMap.clear();
  VM = 0;
}

static void setLandingSlots(VASTSlot *S, SparseBitVector<> &Landings) {
  // Perform depth first search to check if we can reach RHS from LHS with
  // 0-distance edges.
  SmallPtrSet<VASTSlot*, 8> Visited;
  SmallVector<std::pair<VASTSlot*, VASTSlot::succ_iterator>, 4> WorkStack;
  WorkStack.push_back(std::make_pair(S, S->succ_begin()));

  while (!WorkStack.empty()) {
    VASTSlot *S = WorkStack.back().first;
    VASTSlot::succ_iterator ChildIt = WorkStack.back().second;

    if (ChildIt == S->succ_end()) {
      WorkStack.pop_back();
      continue;
    }

    VASTSlot::EdgePtr Edge = *ChildIt;
    ++WorkStack.back().second;
    VASTSlot *Child = Edge;

    // Now we land with the 1-distance edge.
    if (Edge.getDistance()) {
      Landings.set(Child->SlotNum);
      // Skip the children of the current node as we had already reach the leave.
      continue;
    }

    // Do not visit a node twice.
    if (!Visited.insert(Child)) continue;

    WorkStack.push_back(std::make_pair(Child, Child->succ_begin()));
  }

  DEBUG(dbgs() << "Slot #" << S->SlotNum << " landing: ";
  ::dump(Landings, dbgs()));
}

bool STGDistances::runOnVASTModule(VASTModule &VM) {
  releaseMemory();

  // Setup the landing slot map.
  typedef VASTModule::slot_iterator slot_iterator;
  for (slot_iterator I = VM.slot_begin(), E = VM.slot_end(); I != E; ++I) {
    VASTSlot *S = I;
    setLandingSlots(S, LandingMap[S->SlotNum]);
  }

  this->VM = &VM;
  SPImpl = new ShortestPathImpl();

  return false;
}

void STGDistances::print(raw_ostream &OS) const {
  assert(SPImpl && "Print after releaseMemory?");
  SPImpl->print(OS, *VM);
}

unsigned STGDistances::getShortestPath(unsigned From, unsigned To) const {
  assert(SPImpl && "Get shortest path after releaseMemory?");
  // Calculate the distances on the fly.
  if (SPImpl->empty()) SPImpl->run(*VM);

  return SPImpl->getDistance(From, To);
}

STGDistanceBase STGDistanceBase::CalculateShortestPathDistance(VASTModule &VM){
  ShortestPathImpl Impl;
  Impl.run(VM);
  return Impl;
}

unsigned STGDistances::getIntervalFromDef(const VASTLatch &L, VASTSlot *ReadSlot,
                                          unsigned ReadSlotNum) const {
  unsigned PathInterval = STGDistances::Inf;

  std::map<unsigned, SparseBitVector<> >::const_iterator J
    = LandingMap.find(L.getSlot()->SlotNum);

  assert(J != LandingMap.end() && "Landing slots cache not built?");
  const SparseBitVector<> &Landings = J->second;
  typedef SparseBitVector<>::iterator iterator;
  for (iterator LI = Landings.begin(), LE = Landings.end(); LI != LE; ++LI) {
    unsigned LandingSlotNum = *LI;

    // Directly read at the landing slot, the interval is 1.
    if (LandingSlotNum == ReadSlotNum)
      return 1;

    unsigned CurInterval = getShortestPath(LandingSlotNum, ReadSlotNum);

    PathInterval = std::min(PathInterval, CurInterval);
  }

  // The is 1 extra cycle from the definition to landing.
  return std::min(PathInterval + 1, STGDistances::Inf);;
}

unsigned
STGDistances::getIntervalFromDef(const VASTSeqValue *V, VASTSlot *ReadSlot) const {
  // Assume the paths from FUOutput are always single cycle paths.
  if (V->isFUOutput()) return 1;

  unsigned PathInterval = STGDistances::Inf;
  unsigned ReadSlotNum = ReadSlot->SlotNum;

  typedef VASTSeqValue::const_fanin_iterator fanin_iterator;
  for (fanin_iterator I = V->fanin_begin(), E = V->fanin_end(); I != E; ++I) {
    const VASTLatch &L = *I;

    if (V->getSelector()->isTrivialFannin(L)) continue;

    unsigned CurInterval = getIntervalFromDef(L, ReadSlot, ReadSlotNum);
    PathInterval = std::min(PathInterval, CurInterval);
  }

  //assert(IntervalFromLanding < STGDistances::Inf && "No live-in?");
  return PathInterval;
}

unsigned STGDistances::getIntervalFromDef(const VASTSelector *Sel,
                                          VASTSlot *ReadSlot) const {
  // Assume the paths from FUOutput are always single cycle paths.
  if (Sel->isFUOutput()) return 1;

  unsigned PathInterval = STGDistances::Inf;
  unsigned ReadSlotNum = ReadSlot->SlotNum;

  typedef VASTSelector::const_iterator iterator;
  for (iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    const VASTLatch &L = *I;

    if (Sel->isTrivialFannin(L)) continue;

    unsigned CurInterval = getIntervalFromDef(L, ReadSlot, ReadSlotNum);
    PathInterval = std::min(PathInterval, CurInterval);
  }

  //assert(IntervalFromLanding < STGDistances::Inf && "No live-in?");
  return PathInterval;
}
