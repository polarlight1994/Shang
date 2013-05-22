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

#include "STGDistances.h"

#include "shang/Passes.h"
#include "shang/VASTModule.h"

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

struct LongestPathImpl : public STGDistanceImpl<LongestPathImpl> {
  bool updateDistance(unsigned DistanceSrcThuDst,
                      unsigned DstSlot, unsigned SrcSlot);

  void initialize(VASTModule &VM);
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
void LongestPathImpl::initialize(VASTModule &VM) {
  STGDistanceBase::initialize(VM);

  // Handle the backedges.
  std::set<VASTSlot*> Visited;

  // Visit the slots in topological order.
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >
    RPO(VM.getStartSlot());

  typedef
  ReversePostOrderTraversal<VASTSlot*, GraphTraits<VASTSlot*> >::rpo_iterator
  slot_top_iterator;

  for (slot_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    VASTSlot *Dst = *I;
    Visited.insert(Dst);

    typedef VASTSlot::pred_iterator pred_iterator;
    for (pred_iterator PI = Dst->pred_begin(), PE = Dst->pred_end();
         PI != PE; ++PI) {
      VASTSlot *Src = *PI;

      if (Visited.count(Src)) continue;

      // Now we get a back-edge, for each backedge Src -> Dst, there must be
      // a path from Dst to Src, set the distance of this path to Infinite.
      // Because there is a loop: Src->Dst->Src, and the longest path distance
      // will be infinite because of the loop.
      DistanceMatrix[Src->SlotNum][Dst->SlotNum] = STGDistances::Inf;
    }
  }
}

bool LongestPathImpl::updateDistance(unsigned DistanceSrcThuDst,
                                     unsigned DstSlot, unsigned SrcSlot) {
  // Saturate the longest path distance at Infinite.
  DistanceSrcThuDst = std::min(STGDistances::Inf, DistanceSrcThuDst);
  unsigned DistanceSrcDst = getDistance(SrcSlot, DstSlot);
  if (DistanceSrcThuDst > DistanceSrcDst) {
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

STGDistances::STGDistances() : VASTModulePass(ID), SPImpl(0), LPImpl(0), VM(0) {
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
  DeleteAndReset(LPImpl);
  VM = 0;
}

bool STGDistances::runOnVASTModule(VASTModule &VM) {
  releaseMemory();

  this->VM = &VM;
  SPImpl = new ShortestPathImpl();
  LPImpl = new LongestPathImpl();

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

unsigned STGDistances::getLongestPath(unsigned From, unsigned To) const {
  assert(LPImpl && "Get longest path after releaseMemory?");
  // Calculate the distances on the fly.
  if (LPImpl->empty()) LPImpl->run(*VM);

  return LPImpl->getDistance(From, To);
}

STGDistanceBase STGDistanceBase::CalculateShortestPathDistance(VASTModule &VM){
  ShortestPathImpl Impl;
  Impl.run(VM);
  return Impl;
}
