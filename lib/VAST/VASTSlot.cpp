//===--------- VASTSlot.cpp - The Time Slot in Control Path  ----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the functions related to the control path of the design.
//
//===----------------------------------------------------------------------===//
#include "LangSteam.h"

#include "shang/VASTSlot.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTSubModules.h"
#include "shang/VASTModule.h"

#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "vast-slot"
#include "llvm/Support/Debug.h"

using namespace llvm;

VASTSlot::VASTSlot(unsigned slotNum, BasicBlock *ParentBB,  VASTValPtr Pred,
                   bool IsSubGrp)
  : VASTNode(vastSlot), SlotReg(this, 0), SlotActive(this, 0),
    SlotReady(this, 0), SlotPred(this, Pred), SlotNum(slotNum),
    IsSubGrp(IsSubGrp) {
  Contents.ParentBB = ParentBB;
}

VASTSlot::VASTSlot(unsigned slotNum)
  : VASTNode(vastSlot), SlotReg(this, 0), SlotActive(this, 0),
    SlotReady(this, 0), SlotPred(this, VASTImmediate::True), SlotNum(slotNum),
    IsSubGrp(false) {
  Contents.ParentBB = 0;
}

void VASTSlot::createSignals(VASTModule *VM) {
  assert(!IsSubGrp && "Cannot create signal for virtual slots!");

  // Create the relative signals.
  std::string SlotName = "Slot" + utostr_32(SlotNum);
  VASTRegister *R = VM->addRegister(SlotName + "r", 1, SlotNum == 0 ? 1 : 0,
                                    VASTRegister::Slot, SlotNum);
  SlotReg.set(R);

  VASTWire *Ready = VM->addWire(SlotName + "Ready", 1,
                                VASTModule::DirectClkEnAttr.c_str());
  SlotReady.set(Ready);

  VASTWire *Active = VM->addWire(SlotName + "Active", 1,
                                 VASTModule::DirectClkEnAttr.c_str());
  SlotActive.set(Active);
}

void VASTSlot::copySignals(VASTSlot *S) {
  // Finish slot alias with the start slot.
  SlotReg.set(S->SlotReg);
  SlotReady.set(S->SlotReady);
  SlotActive.set(S->SlotActive);
}

BasicBlock *VASTSlot::getParent() const {
  return Contents.ParentBB;
}

bool VASTSlot::hasNextSlot(VASTSlot *NextSlot) const {
  return std::find(NextSlots.begin(), NextSlots.end(), NextSlot) != NextSlots.end();
}

void VASTSlot::unlinkSuccs() {
  for (succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
    VASTSlot *SuccSlot = *I;
    assert(SuccSlot != this && "Unexpected loop!");

    // Find the this slot in the PredSlot of the successor and erase it.
    pred_iterator at
      = std::find(SuccSlot->PredSlots.begin(), SuccSlot->PredSlots.end(), this);
    SuccSlot->PredSlots.erase(at);
  }

  NextSlots.clear();
}

VASTSlot::op_iterator VASTSlot::removeOp(op_iterator where) {
  // Clear the SeqOp's parent slot.
  VASTSeqOp *Op = *where;
  Op->clearParent();
  // Erase it from the SeqOp vector.
  return Operations.erase(where);
}

void VASTSlot::removeOp(VASTSeqOp *Op) {
  op_iterator at = std::find(op_begin(), op_end(), Op);
  assert(at != op_end() && "Op is not in this slot?");
  removeOp(at);
}

//VASTRegister *VASTSlot::getRegister() const {
//  return cast<VASTRegister>(getValue()->getParent());
//}

VASTRegister *VASTSlot::getValue() const {
  return SlotReg.unwrap().getAsLValue<VASTRegister>();
}

const char *VASTSlot::getName() const {
  return getValue()->getName();
}

void VASTSlot::addSuccSlot(VASTSlot *NextSlot) {
  // Do not add the same successor slot twice.
  if (hasNextSlot(NextSlot)) return;

  assert(NextSlot != this && "Unexpected loop!");

  // Connect the slots.
  NextSlot->PredSlots.push_back(this);
  NextSlots.push_back(NextSlot);
}

VASTSlot *VASTSlot::getSubGroup(BasicBlock *BB) const {
  VASTSlot *SubGrp = 0;
  typedef VASTSlot::const_succ_iterator iterator;
  for (iterator I = succ_begin(), E = succ_end(); I != E; ++I){
    VASTSlot *Succ = *I;

    if (!Succ->IsSubGrp || Succ->getParent() != BB) continue;

    assert(SubGrp == 0 && "Unexpected multiple subgroup with the same BB!");
    SubGrp = Succ;
  }

  return SubGrp;
}

void VASTSlot::print(raw_ostream &OS) const {
  OS << "Slot#"<< SlotNum << " Pred: ";
  for (const_pred_iterator I = pred_begin(), E = pred_end(); I != E; ++I)
    OS << "S#" << (*I)->SlotNum << ", ";

  if (BasicBlock *BB = getParent())
    OS << "BB: " << BB->getName();

  OS << '\n';

  for (const_op_iterator I = op_begin(), E = op_end(); I != E; ++I)
    (*I)->print(OS.indent(2));

  OS << "Succ: ";

  for (const_succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I)
    OS << "S#" << (*I)->SlotNum << '(' << (*I)->IsSubGrp << ")v, ";
}

//===----------------------------------------------------------------------===//
// Graph writer for the state-trasition graph.
namespace llvm {
template <>
struct GraphTraits<const VASTModule*> : public GraphTraits<const VASTSlot*> {
  typedef VASTModule::const_slot_iterator nodes_iterator;
  static nodes_iterator nodes_begin(const VASTModule *G) {
    return G->slot_begin();
  }
  static nodes_iterator nodes_end(const VASTModule *G) {
    return G->slot_end();
  }
};


template<>
struct DOTGraphTraits<const VASTModule*> : public DefaultDOTGraphTraits{
  typedef const VASTSlot NodeTy;
  typedef const VASTModule GraphTy;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  std::string getNodeLabel(NodeTy *Node, GraphTy *Graph) {
    std::string Str;
    raw_string_ostream ss(Str);
    ss << Node->SlotNum;

    if (VASTRegister *V =Node->getValue())
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

  // Print the cluster of the subregions. This groups the single basic blocks
  // and adds a different background color for each group.
  static void printSubgrpCluster(const VASTSlot *S,
                                 GraphWriter<const VASTModule*> &GW,
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

  static void addCustomGraphFeatures(const VASTModule *VM,
                                     GraphWriter<const VASTModule*> &GW) {
    // raw_ostream &O = GW.getOStream();
    // O << "\tcolorscheme = \"paired12\"\n";
    //typedef VASTModule::const_slot_iterator iterator;
    //for (iterator I = VM->slot_begin(), E = VM->slot_end(); I != E; ++I) {
    //  const VASTSlot *S = I;
    //  if (!S->IsVirtual) printSubgrpCluster(S, GW, 4);
    //}
  }
};
}

void VASTModule::viewGraph() const {
  ViewGraph(this, getName());
}
