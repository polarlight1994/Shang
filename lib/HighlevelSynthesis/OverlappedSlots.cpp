//===---- OverlappedSlots.cpp - Identify the overlapped slots ----*-C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the OverlapSlots analysis. The analysis identifies the
// non-mutually exclusive slots with overlapped timeframes. This can happened
// after we relax the control dependencies from/to the boundaries of the basic
// blocks.
//
//===----------------------------------------------------------------------===//

#include "OverlappedSlots.h"
#include "STGShortestPath.h"

#include "shang/VASTSlot.h"
#include "shang/VASTModule.h"
#include "shang/Passes.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "vast-overlapped-slot"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace llvm {
struct TimeFrame {
  typedef VASTSlot::SlotNumTy SlotNumTy;
  SlotNumTy LB, UB;
  TimeFrame(SlotNumTy LB = 0, SlotNumTy UB = 0) : LB(LB), UB(UB) {}

  void print(raw_ostream &OS) const {
    OS << '[' << LB << ',' << UB << ']';
  }

  void dump() const {
    print(dbgs());
    dbgs() << '\n';
  }
};

inline raw_ostream &operator<<(raw_ostream &OS, const TimeFrame &TF) {
  TF.print(OS);
  return OS;
}

struct OverlappedSlotsImpl {
  typedef VASTSlot::SlotNumTy SlotNumTy;
  VASTModule &VM;
  STGShortestPath &STP;

  typedef DenseMap<unsigned, TimeFrame>  TFMapTy;
  TFMapTy TFMap;

  std::map<BasicBlock*, std::vector<VASTSlot*> > BBMap;

  OverlappedSlotsImpl(VASTModule &VM, STGShortestPath &STP)
    : VM(VM), STP(STP) {}

  void initilizeBBMap();
  void buildTimeFrame();
  void viewGraph();

  TimeFrame getTimeFrame(const VASTSlot *S) const {
    return TFMap.lookup(S->SlotNum);
  }
};

// Graph writer for the time frames.
template <>
struct GraphTraits<OverlappedSlotsImpl*>
  : public GraphTraits<const VASTSlot*> {

  typedef VASTModule::const_slot_iterator nodes_iterator;
  static nodes_iterator nodes_begin(const OverlappedSlotsImpl *G) {
    return G->VM.slot_begin();
  }
  static nodes_iterator nodes_end(const OverlappedSlotsImpl *G) {
    return G->VM.slot_end();
  }
};


template<>
struct DOTGraphTraits<OverlappedSlotsImpl*> : public DefaultDOTGraphTraits{
  typedef const VASTSlot NodeTy;
  typedef const OverlappedSlotsImpl GraphTy;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  std::string getNodeLabel(NodeTy *Node, GraphTy *Graph) {
    std::string Str;
    raw_string_ostream ss(Str);
    ss << Node->SlotNum;

    if (VASTSeqValue *V =Node->getValue())
      ss << " [" << V->getName() << ']';

    if (BasicBlock *BB = Node->getParent())
      ss << " (" << BB->getName() << ')';

    const TimeFrame &TF = Graph->getTimeFrame(Node);
    ss << '\n' << TF;

    DEBUG(Node->print(ss));
    return ss.str();
  }

  static std::string getNodeAttributes(NodeTy *Node, GraphTy *Graph) {
    std::string Attr = "shape=Mrecord";

    if (Node->IsVirtual) Attr += ", style=dotted";

    return Attr;
  }
};
}

void OverlappedSlotsImpl::buildTimeFrame() {
  const VASTSlot *Entry = VM.getStartSlot();
  TFMap.insert(std::make_pair(Entry->SlotNum, TimeFrame()));

  typedef
  ReversePostOrderTraversal<const VASTSlot*, GraphTraits<const VASTSlot*> >
  RPOT;

  RPOT RPO(Entry);

  typedef RPOT::rpo_iterator slot_top_iterator;

  for (slot_top_iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
    const VASTSlot *S = *I;
    if (S == Entry) continue;

    TimeFrame &TF = TFMap[S->SlotNum];
    TF.LB = STP.getShortestPath(Entry->SlotNum, S->SlotNum);
    assert(TF.LB != STGShortestPath::Inf && "Slot not reachable?");

    TF.UB = TF.LB;
    // The distance only increase when the current slot is not virual.
    SlotNumTy Inc = S->IsVirtual ? 0 : 1;
    typedef VASTSlot::const_pred_iterator iterator;
    for (iterator PI = S->pred_begin(), PE = S->pred_end(); PI != PE; ++PI)
      TF.UB = std::max<SlotNumTy>(TF.UB, TFMap[(*PI)->SlotNum].UB + Inc);
  }
}

void OverlappedSlotsImpl::viewGraph() {
  ViewGraph(this, "Graph");
}

char OverlappedSlots::ID = 0;

INITIALIZE_PASS_BEGIN(OverlappedSlots, "vast-overlapped-slot",
                      "Identify the timeframe overlapped slots",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(STGShortestPath)
INITIALIZE_PASS_END(OverlappedSlots, "vast-overlapped-slot",
                    "Identify the timeframe overlapped slots",
                    false, true)

OverlappedSlots::OverlappedSlots() : VASTModulePass(ID) {
  initializeOverlappedSlotsPass(*PassRegistry::getPassRegistry());
}

void OverlappedSlots::getAnalysisUsage(AnalysisUsage &AU) const {
  VASTModulePass::getAnalysisUsage(AU);
  AU.addRequired<STGShortestPath>();
  AU.setPreservesAll();
}

bool OverlappedSlots::runOnVASTModule(VASTModule &VM) {
  STGShortestPath &STP = getAnalysis<STGShortestPath>();
  OverlappedSlotsImpl Impl(VM, STP);

  Impl.buildTimeFrame();
  Impl.viewGraph();

  return false;
}

void OverlappedSlots::releaseMemory() {

}

void OverlappedSlots::print(raw_ostream &OS) const {

}
