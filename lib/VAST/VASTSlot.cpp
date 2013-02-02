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

VASTValPtr &VASTSlot::getOrCreateSuccCnd(VASTSlot *DstSlot) {
  assert(DstSlot && "Bad DstSlot!");
  VASTValPtr &U = NextSlots[DstSlot];
  // If we are adding a new succ slot, link the DstSlot to current slot as well.
  if (!U) DstSlot->PredSlots.push_back(this);

  return U;
}

bool VASTSlot::hasNextSlot(VASTSlot *NextSlot) const {
  if (NextSlots.empty()) return NextSlot->SlotNum == SlotNum + 1;

  return NextSlots.count(NextSlot);
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

void VASTSlot::print(raw_ostream &OS) const {
  OS << "Slot#"<< SlotNum << " Pred: ";
  for (const_pred_iterator I = pred_begin(), E = pred_end(); I != E; ++I)
    OS << "S#" << (*I)->SlotNum << ", ";

  OS << '\n';

  for (const_op_iterator I = op_begin(), E = op_end(); I != E; ++I)
    (*I)->print(OS.indent(2));

  OS << "Succ: ";

  for (const_succ_cnd_iterator I = succ_cnd_begin(), E = succ_cnd_end();
       I != E; ++I) {
    OS << "S#" << I->first->SlotNum << " (";
    I->second.printAsOperand(OS);
    OS << "), ";
  }
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
