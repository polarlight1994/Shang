//===---------- VASTSlot.h - The Time Slot in Control PATH ------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the VASTSlot, which represent the state in the
// state-transition graph of the control path.
//
//===----------------------------------------------------------------------===//
#ifndef VTM_VAST_CONTROL_PATH_NODES_H
#define VTM_VAST_CONTROL_PATH_NODES_H

#include "shang/VASTNodeBases.h"
#include "shang/VASTDatapathNodes.h"
#include "shang/Utilities.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/PointerUnion.h"

namespace llvm {
class VASTRegister;
class VASTExprBuilder;
class vlang_raw_ostream;
class VASTSlot;
class VASTSeqOp;
class VASTSlotCtrl;

class VASTSlot : public VASTNode, public ilist_node<VASTSlot> {
public:
  typedef SmallVector<VASTSlot*, 4> SuccVecTy;
  typedef SuccVecTy::iterator succ_iterator;
  typedef SuccVecTy::const_iterator const_succ_iterator;

  typedef SmallVector<VASTSlot*, 4> PredVecTy;
  typedef PredVecTy::iterator pred_iterator;
  typedef PredVecTy::const_iterator const_pred_iterator;

private:
  // The relative signal of the slot: Slot register, Slot active and Slot ready.
  VASTUse SlotReg;
  VASTUse SlotActive;
  VASTUse SlotReady;
  VASTUse SlotPred;

  // The link to other slots.
  PredVecTy PredSlots;
  SuccVecTy NextSlots;

  // The definitions in the current slot.
  typedef std::vector<VASTSeqOp*> OpVector;
  OpVector Operations;

  void dropUses() {
    assert(0 && "Function not implemented!");
  }

  friend class VASTModule;
  VASTSlot(unsigned slotNum, BasicBlock *ParentBB, VASTValPtr Pred,
           bool IsVirtual);

  VASTSlot() : VASTNode(vastSlot), SlotReg(this), SlotActive(this),
    SlotReady(this), SlotPred(this), SlotNum(0), IsVirtual(true) {}

  friend struct ilist_sentinel_traits<VASTSlot>;
public:
  // Create the finish slot.
  explicit VASTSlot(unsigned slotNum);

  const uint16_t SlotNum : 15;
  const bool     IsVirtual : 1;

  void createSignals(VASTModule *VM);
  void copySignals(VASTSlot *S);

  BasicBlock *getParent() const;

  void print(raw_ostream &OS) const;

  VASTSeqValue *getValue() const;
  const char *getName() const;
  // Getting the relative signals.
  VASTRegister *getRegister() const;
  VASTValue *getReady() const { return cast<VASTValue>(SlotReady); }
  VASTValue *getActive() const { return cast<VASTValue>(SlotActive); }
  VASTUse &getPred() { return SlotPred; }

  void addOperation(VASTSeqOp *D) { Operations.push_back(D); }
  typedef OpVector::const_iterator const_op_iterator;
  const_op_iterator op_begin() const { return Operations.begin(); }
  const_op_iterator op_end() const { return Operations.end(); }
  typedef OpVector::iterator op_iterator;
  op_iterator op_begin() { return Operations.begin(); }
  op_iterator op_end() { return Operations.end(); }

  // Remove a VASTSeqOp pointed by where in this slot.
  op_iterator removeOp(op_iterator where);
  void removeOp(VASTSeqOp *Op);

  bool hasNextSlot(VASTSlot *NextSlot) const;

  void addSuccSlot(VASTSlot *NextSlot);

  bool isSynthesized() const { return !SlotReg.isInvalid(); }

  // Successor slots of this slot.
  succ_iterator succ_begin() {
    return NextSlots.begin();
  }

  const_succ_iterator succ_begin() const {
    return NextSlots.begin();
  }

  succ_iterator succ_end() {
    return NextSlots.end();
  }

  const_succ_iterator succ_end() const {
    return NextSlots.end();
  }

  bool succ_empty() const { return NextSlots.empty(); }
  unsigned succ_size() const { return NextSlots.size(); }

  // Predecessor slots of this slot.
  pred_iterator pred_begin() { return PredSlots.begin(); }
  pred_iterator pred_end() { return PredSlots.end(); }
  const_pred_iterator pred_begin() const { return PredSlots.begin(); }
  const_pred_iterator pred_end() const { return PredSlots.end(); }
  unsigned pred_size() const { return PredSlots.size(); }

  void unlinkSuccs();

  bool operator<(const VASTSlot &RHS) const {
    return SlotNum < RHS.SlotNum;
  }

  static inline bool classof(const VASTSlot *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSlot;
  }
};

template<> struct GraphTraits<VASTSlot*> {
  typedef VASTSlot NodeType;
  typedef NodeType::succ_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->succ_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->succ_end();
  }
};

template<> struct GraphTraits<const VASTSlot*> {
  typedef const VASTSlot NodeType;
  typedef NodeType::const_succ_iterator ChildIteratorType;
  static NodeType *getEntryNode(NodeType* N) { return N; }
  static inline ChildIteratorType child_begin(NodeType *N) {
    return N->succ_begin();
  }
  static inline ChildIteratorType child_end(NodeType *N) {
    return N->succ_end();
  }
};
} // end namespace

#endif
