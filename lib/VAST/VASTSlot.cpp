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
#include "VASTExprBuilder.h"

#include "shang/VASTSlot.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTSubModules.h"
#include "shang/VASTModule.h"

#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "vast-slot"
#include "llvm/Support/Debug.h"

using namespace llvm;

VASTSlot::VASTSlot(unsigned slotNum, BasicBlock *ParentBB)
  : VASTNode(vastSlot), SlotReg(this, 0), SlotActive(this, 0),
    SlotReady(this, 0), SlotNum(slotNum) {
  Contents.ParentBB = ParentBB;
}

VASTSlot::VASTSlot(unsigned slotNum)
  : VASTNode(vastSlot), SlotReg(this, 0), SlotActive(this, 0),
    SlotReady(this, 0), SlotNum(slotNum){
  Contents.ParentBB = 0;
}

void VASTSlot::createSignals(VASTModule *VM) {
  // Create the relative signals.
  std::string SlotName = "Slot" + utostr_32(SlotNum);
  VASTRegister *R = VM->addRegister(SlotName + "r", 1, SlotNum == 0 ? 1 : 0,
                                    VASTSeqValue::Slot, SlotNum,
                                    VASTModule::DirectClkEnAttr.c_str());
  SlotReg.set(R->getValue());

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
  bool hasLoop = false;

  for (succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I) {
    VASTSlot *SuccSlot = *I;

    if (SuccSlot == this) {
      hasLoop = true;
      continue;
    }

    // Find the this slot in the PredSlot of the successor and erase it.
    pred_iterator at
      = std::find(SuccSlot->PredSlots.begin(), SuccSlot->PredSlots.end(), this);
    SuccSlot->PredSlots.erase(at);
  }

  NextSlots.clear();

  if (hasLoop) NextSlots.push_back(this);
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

VASTRegister *VASTSlot::getRegister() const {
  return cast<VASTRegister>(getValue()->getParent());
}

VASTSeqValue *VASTSlot::getValue() const {
  return cast<VASTSeqValue>(SlotReg);
}

const char *VASTSlot::getName() const {
  return getValue()->getName();
}

VASTSlotCtrl *VASTSlot::getBrToSucc(const VASTSlot *DstSlot) const {
  // Find the SeqOp that branching to DstSlot and return the condition.
  for (const_op_iterator I = op_begin(), E = op_end(); I != E; ++I)
    if (VASTSlotCtrl *SlotCtrl = dyn_cast<VASTSlotCtrl>(*I))
      if (SlotCtrl->isBranch() && SlotCtrl->getTargetSlot() == DstSlot)
          return SlotCtrl;

  return 0;
}

VASTValPtr VASTSlot::getSuccCnd(const VASTSlot *DstSlot) const {
  // Find the SeqOp that branching to DstSlot and return the condition.
  VASTSlotCtrl *SlotCtrl = getBrToSucc(DstSlot);
  assert(SlotCtrl &&  "DstSlot is not the successor of current slot!");
  return SlotCtrl->getPred();
}

void VASTSlot::addSuccSlot(VASTSlot *NextSlot) {
  // Do not add the same successor slot twice.
  if (hasNextSlot(NextSlot)) return;

  // Connect the slots.
  NextSlot->PredSlots.push_back(this);
  NextSlots.push_back(NextSlot);
}

void VASTSlot::print(raw_ostream &OS) const {
  OS << "Slot#"<< SlotNum << " Pred: ";
  for (const_pred_iterator I = pred_begin(), E = pred_end(); I != E; ++I)
    OS << "S#" << (*I)->SlotNum << ", ";

  OS << '\n';

  for (const_op_iterator I = op_begin(), E = op_end(); I != E; ++I)
    (*I)->print(OS.indent(2));

  OS << "Succ: ";

  for (const_succ_iterator I = succ_begin(), E = succ_end(); I != E; ++I)
    OS << "S#" << (*I)->SlotNum << ", ";
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
    ss << Node->getName();
    DEBUG(Node->print(ss));
    return ss.str();
  }

  static std::string getNodeAttributes(NodeTy *Node, GraphTy *Graph) {
      return "shape=Mrecord";
  }
};
}

void VASTModule::viewGraph() const {
  ViewGraph(this, getName());
}
