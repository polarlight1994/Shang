//===----- VASTCtrlRgn.cpp - Control Region in VASTModule -------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the control regions in a VASTModule, it may corresponds
// to the control flow of a function, or a loop body, etc. It is a subclass of
// VASTSubmoduleBase, because it is something like a submodule: it has fanins
// (live-in values), fanouts (live-out values). But there is no start/fin signal
// because the STG of VASTCtrlRgn is directly connected to its parent control
// region. Unlike VASTModule, the VASTCtrlRgn to not own other VASTNodes except
// the VASTSlots and VASTSeqOps.
//
//===----------------------------------------------------------------------===//

#include "vast/VASTCtrlRgn.h"
#include "vast/VASTSeqOp.h"
#include "vast/VASTSeqValue.h"

#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "vast-ctrl-region"
#include "llvm/Support/Debug.h"

using namespace llvm;

bool VASTCtrlRgn::gcImpl() {
  bool Changed = false;

  for (seqop_iterator I = seqop_begin(); I != seqop_end(); /*++I*/) {
    VASTSeqOp *Op = I++;
    if (Op->getGuard() == VASTImmediate::False) {
      DEBUG(dbgs() << "Removing SeqOp whose predicate is always false:\n";
            Op->dump(););

      Op->eraseFromParent();

      Changed |= true;
    }
  }

  return Changed;
}

VASTModule *VASTCtrlRgn::getParentModule() const {
  return Parent;
}

Function *VASTCtrlRgn::getFunction() const {
  return F;
}

BasicBlock *VASTCtrlRgn::getEntryBlock() const {
  return &getFunction()->getEntryBlock();
}

void VASTCtrlRgn::print(raw_ostream &OS) const {
  for (const_slot_iterator SI = slot_begin(), SE = slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    S->print(OS);

    OS << '\n';
  }
}

namespace {
  struct SlotNumEq {
    unsigned SlotNum;
    SlotNumEq(unsigned SlotNum) : SlotNum(SlotNum) {}

    bool operator()(const VASTSlot &S) const {
      return S.SlotNum == SlotNum;
    }
  };
}

VASTSlot *
VASTCtrlRgn::createSlot(unsigned SlotNum, BasicBlock *ParentBB,
                        unsigned Schedule, VASTValPtr Pred, bool IsVirtual) {
  assert(!std::count_if(Slots.begin(), Slots.end(), SlotNumEq(SlotNum))
         && "The same slot had already been created!");

  VASTSlot *Slot = new VASTSlot(SlotNum, *this, ParentBB,
                                Pred, IsVirtual, Schedule);
  // Insert the newly created slot before the finish slot.
  Slots.insert(Slots.back(), Slot);

  return Slot;
}

VASTSlot *VASTCtrlRgn::createLandingSlot() {
  BasicBlock *Entry = NULL/*getEntryBlock()*/;
  VASTSlot *Landing = new VASTSlot(0, *this, Entry,
                                   VASTImmediate::True, false, 0);
  Slots.push_back(Landing);
  // Also create the finish slot.
  Slots.push_back(new VASTSlot(-1, *this));
  return Landing;
}

VASTSlot *VASTCtrlRgn::getLandingSlot() {
  return &Slots.front();
}

VASTSlot *VASTCtrlRgn::getStartSlot() {
  return &Slots.front();
}

VASTSlot *VASTCtrlRgn::getFinishSlot() {
  return &Slots.back();
}

VASTCtrlRgn::VASTCtrlRgn(VASTModule &Parent, Function &F, const char *Name)
  : VASTSubModuleBase(vastCtrlRegion, Name, 0), Parent(&Parent), F(&F) {
  createLandingSlot();
}

VASTCtrlRgn::VASTCtrlRgn()
  : VASTSubModuleBase(vastCtrlRegion, NULL, 0), Parent(NULL), F(NULL) {
  createLandingSlot();
}

VASTSeqInst *
VASTCtrlRgn::latchValue(VASTSeqValue *SeqVal, VASTValPtr Src, VASTSlot *Slot,
                       VASTValPtr GuardCnd, Value *V, unsigned Latency) {
  assert(Src && "Bad assignment source!");
  VASTSeqInst *Inst = lauchInst(Slot, GuardCnd, 1, V, true);
  Inst->addSrc(Src, 0, SeqVal);
  Inst->setCyclesFromLaunch(Latency);

  return Inst;
}

VASTSeqInst *
VASTCtrlRgn::lauchInst(VASTSlot *Slot, VASTValPtr Pred, unsigned NumOps, Value *V,
                      bool IsLatch) {
  // Create the uses in the list.
  VASTSeqInst *SeqInst = new VASTSeqInst(V, Slot, NumOps, IsLatch);
  // Create the predicate operand.
  new (SeqInst->Operands) VASTUse(SeqInst, Pred);

  // Add the SeqOp to the the all SeqOp list.
  Ops.push_back(SeqInst);

  return SeqInst;
}

VASTSeqCtrlOp *VASTCtrlRgn::createCtrlLogic(VASTValPtr Src, VASTSlot *Slot,
                                           VASTValPtr GuardCnd,
                                           bool UseSlotActive) {
  VASTSeqCtrlOp *CtrlOp = new VASTSeqCtrlOp(Slot, UseSlotActive);
  // Create the predicate operand.
  new (CtrlOp->Operands) VASTUse(CtrlOp, GuardCnd);

  // Add the SeqOp to the the all SeqOp list.
  Ops.push_back(CtrlOp);
  return CtrlOp;

}

VASTSeqCtrlOp *VASTCtrlRgn::assignCtrlLogic(VASTSeqValue *SeqVal, VASTValPtr Src,
                                            VASTSlot *Slot, VASTValPtr GuardCnd,
                                            bool UseSlotActive) {
  VASTSeqCtrlOp *CtrlOp = createCtrlLogic(Src, Slot, GuardCnd, UseSlotActive);
  // Create the source of the assignment
  CtrlOp->addSrc(Src, 0, SeqVal);
  return CtrlOp;
}

VASTSeqCtrlOp *VASTCtrlRgn::assignCtrlLogic(VASTSelector *Selector, VASTValPtr Src,
                                            VASTSlot *Slot, VASTValPtr GuardCnd,
                                            bool UseSlotActive) {
  VASTSeqCtrlOp *CtrlOp = createCtrlLogic(Src, Slot, GuardCnd, UseSlotActive);
  // Create the source of the assignment
  CtrlOp->addSrc(Src, 0, Selector);
  return CtrlOp;
}

VASTSlotCtrl *VASTCtrlRgn::createStateTransition(VASTNode *N, VASTSlot *Slot,
                                                 VASTValPtr Guard) {
  VASTSlotCtrl *CtrlOp = new VASTSlotCtrl(Slot, N);
  // Create the predicate operand.
  new (CtrlOp->Operands) VASTUse(CtrlOp, Guard);

  // Add the SeqOp to the the all SeqOp list.
  Ops.push_back(CtrlOp);

  return CtrlOp;
}

void VASTCtrlRgn::verify() const {
  for (const_slot_iterator I = slot_begin(), E = slot_end(); I != E; ++I)
    I->verify();
}

void VASTCtrlRgn::finalize() {
  // Delete all slot, this also implicitly delete all operation, and drop the
  // operands of the operation.
  Slots.clear();
  assert(Ops.empty() && "Operations are not deleted with its parent slots?");
}

VASTCtrlRgn::~VASTCtrlRgn() {
  finalize();
}

//===----------------------------------------------------------------------===//
// Graph writer for the state-trasition graph.
namespace llvm {
template <>
struct GraphTraits<const VASTCtrlRgn*> : public GraphTraits<const VASTSlot*> {
  typedef VASTCtrlRgn::const_slot_iterator nodes_iterator;
  static nodes_iterator nodes_begin(const VASTCtrlRgn *G) {
    return G->slot_begin();
  }
  static nodes_iterator nodes_end(const VASTCtrlRgn *G) {
    return G->slot_end();
  }
};


template<>
struct DOTGraphTraits<const VASTCtrlRgn*> : public DefaultDOTGraphTraits{
  typedef const VASTSlot NodeTy;
  typedef const VASTCtrlRgn GraphTy;
  typedef GraphTraits<NodeTy*>::ChildIteratorType ChildIteratorType;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  std::string getNodeLabel(NodeTy *Node, GraphTy *Graph) {
    std::string Str;
    raw_string_ostream ss(Str);
    ss << Node->SlotNum;

    if (VASTSeqValue *V =Node->getValue())
      ss << " [" << V->getName() << ']';

    if (BasicBlock *BB = Node->getParent())
      ss << " (" << BB->getName() << ')';

    DEBUG(Node->print(ss));
    return ss.str();
  }

  static std::string getNodeAttributes(NodeTy *Node, GraphTy *Graph) {
    std::string Attr = "shape=Mrecord";

    if (Node->IsSubGrp) Attr += ", style=dotted";

    return Attr;
  }

  /// If you want to override the dot attributes printed for a particular
  /// edge, override this method.
  static std::string getEdgeAttributes(NodeTy *,
                                       ChildIteratorType EI,
                                       GraphTy *) {
    switch ((*EI).getType()) {
    case VASTSlot::Sucessor:        return "";
    case VASTSlot::SubGrp:          return "style=dashed";
    case VASTSlot::ImplicitFlow:    return "color=blue,style=dashed";
    }

    llvm_unreachable("Unexpected edge type!");
    return "";
  }

  // Print the cluster of the subregions. This groups the single basic blocks
  // and adds a different background color for each group.
  static void printSubgrpCluster(const VASTSlot *S,
                                 GraphWriter<const VASTCtrlRgn*> &GW,
                                 unsigned depth = 0) {
    raw_ostream &O = GW.getOStream();
    O.indent(2 * depth) << "subgraph cluster_" << static_cast<const void*>(S)
                        << " {\n";
    O.indent(2 * (depth + 1)) << "label = \"\";\n";

    O.indent(2 * (depth + 1)) << "style = solid;\n";
    //O.indent(2 * (depth + 1)) << "style = filled;\n";
    //O.indent(2 * (depth + 1)) << "color = "
    //  << ((R->getDepth() * 2 % 12) + 1) << "\n";

    typedef VASTSlot::const_subgrp_iterator iterator;
    for (iterator I = S->subgrp_begin(), E = S->subgrp_end(); I != E; ++I)
      O.indent(2 * (depth + 1)) << "Node" << static_cast<const void*>(*I)
                                << ";\n";

    O.indent(2 * depth) << "}\n";
  }

  static void addCustomGraphFeatures(const VASTCtrlRgn *R,
                                     GraphWriter<const VASTCtrlRgn*> &GW) {
    // raw_ostream &O = GW.getOStream();
    // O << "\tcolorscheme = \"paired12\"\n";
    //typedef VASTModule::const_slot_iterator iterator;
    //for (iterator I = R->slot_begin(), E = R->slot_end(); I != E; ++I) {
    //  const VASTSlot *S = I;
    //  if (!S->IsVirtual) printSubgrpCluster(S, GW, 4);
    //}
  }
};
}

void VASTCtrlRgn::viewGraph() const {
  ViewGraph(this, F->getName());
}
