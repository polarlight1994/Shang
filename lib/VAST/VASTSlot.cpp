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

VASTSlot::VASTSlot(unsigned slotNum, BasicBlock *ParentBB, VASTModule *VM)
  : VASTNode(vastSlot), SlotReg(this, 0), SlotActive(this, 0),
    SlotReady(this, 0), SlotNum(slotNum) {
  Contents.ParentBB = ParentBB;

  // Create the relative signals.
  std::string SlotName = "Slot" + utostr_32(slotNum);
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

VASTSlot::VASTSlot(unsigned slotNum, VASTSlot *StartSlot)
  : VASTNode(vastSlot), SlotReg(this, 0), SlotActive(this, 0),
    SlotReady(this, 0), SlotNum(slotNum){
  Contents.ParentBB = 0;
  // Finish slot alias with the start slot.
  SlotReg.set(StartSlot->SlotReg);
  SlotReady.set(StartSlot->SlotReady);
  SlotActive.set(StartSlot->SlotActive);
}

BasicBlock *VASTSlot::getParent() const {
  return Contents.ParentBB;
}

bool VASTSlot::hasNextSlot(VASTSlot *NextSlot) const {
  return std::find(NextSlots.begin(), NextSlots.end(), NextSlot) != NextSlots.end();
}

VASTSlot::op_iterator VASTSlot::removeOp(op_iterator where) {
  // Clear the SeqOp's parent slot.
  VASTSeqOp *Op = *where;
  Op->clearParent();
  // Erase it from the SeqOp vector.
  return Operations.erase(where);
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

VASTSeqSlotCtrl *VASTSlot::getBrToSucc(const VASTSlot *DstSlot) const {
  // Find the SeqOp that branching to DstSlot and return the condition.
  for (const_op_iterator I = op_begin(), E = op_end(); I != E; ++I)
    if (VASTSeqSlotCtrl *SlotCtrl = dyn_cast<VASTSeqSlotCtrl>(*I))
      if (SlotCtrl->getCtrlType() == VASTSeqSlotCtrl::SlotBr)
        if (SlotCtrl->getCtrlSignal() == DstSlot->getValue())
          return SlotCtrl;

  return 0;
}

VASTValPtr VASTSlot::getSuccCnd(const VASTSlot *DstSlot) const {
  // Find the SeqOp that branching to DstSlot and return the condition.
  VASTSeqSlotCtrl *SlotCtrl = getBrToSucc(DstSlot);
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
