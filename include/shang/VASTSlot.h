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

class VASTSlot : public VASTNode, public ilist_node<VASTSlot> {
public:
  typedef std::map<VASTSlot*, VASTValPtr> SuccVecTy;
  typedef SuccVecTy::iterator succ_cnd_iterator;
  typedef SuccVecTy::const_iterator const_succ_cnd_iterator;

  // Use mapped_iterator which is a simple iterator adapter that causes a
  // function to be dereferenced whenever operator* is invoked on the iterator.
  typedef
  std::pointer_to_unary_function<std::pair<VASTSlot*, VASTValPtr>, VASTSlot*>
  slot_getter;

  typedef mapped_iterator<succ_cnd_iterator, slot_getter> succ_iterator;
  typedef mapped_iterator<const_succ_cnd_iterator, slot_getter>
          const_succ_iterator;

  typedef SmallVector<VASTSlot*, 4> PredVecTy;
  typedef PredVecTy::iterator pred_iterator;
  typedef PredVecTy::const_iterator const_pred_iterator;

private:
  // The relative signal of the slot: Slot register, Slot active and Slot ready.
  VASTUse SlotReg;
  VASTUse SlotActive;
  VASTUse SlotReady;

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
  VASTSlot(unsigned slotNum, BasicBlock *ParentBB, VASTModule *VM);
  // Create the finish slot.
  VASTSlot(unsigned slotNum, VASTSlot *StartSlot);

  VASTSlot() : VASTNode(vastSlot), SlotReg(this), SlotActive(this),
    SlotReady(this), SlotNum(0) {}

  friend struct ilist_sentinel_traits<VASTSlot>;
public:
  const uint16_t SlotNum;

  BasicBlock *getParent() const;

  void print(raw_ostream &OS) const;

  VASTSeqValue *getValue() const;
  const char *getName() const;
  // Getting the relative signals.
  VASTRegister *getRegister() const;
  VASTValue *getReady() const { return cast<VASTValue>(SlotReady); }
  VASTValue *getActive() const { return cast<VASTValue>(SlotActive); }

  void addOperation(VASTSeqOp *D) { Operations.push_back(D); }
  typedef OpVector::const_iterator const_op_iterator;
  const_op_iterator op_begin() const { return Operations.begin(); }
  const_op_iterator op_end() const { return Operations.end(); }
  typedef OpVector::iterator op_iterator;
  op_iterator op_begin() { return Operations.begin(); }
  op_iterator op_end() { return Operations.end(); }

  bool hasNextSlot(VASTSlot *NextSlot) const;

  VASTValPtr getSuccCnd(const VASTSlot *DstSlot) const {
    const_succ_cnd_iterator at = NextSlots.find(const_cast<VASTSlot*>(DstSlot));
    assert(at != NextSlots.end() && "DstSlot is not the successor!");
    return at->second;
  }

  VASTValPtr &getOrCreateSuccCnd(VASTSlot *DstSlot);

  // Next VASTSlot iterator.
  succ_iterator succ_begin() {
    return map_iterator(NextSlots.begin(),
                        slot_getter(pair_first<VASTSlot*, VASTValPtr>));
  }

  const_succ_iterator succ_begin() const {
    return map_iterator(NextSlots.begin(),
                        slot_getter(pair_first<VASTSlot*, VASTValPtr>));
  }

  succ_iterator succ_end() {
    return map_iterator(NextSlots.end(),
                        slot_getter(pair_first<VASTSlot*, VASTValPtr>));
  }

  const_succ_iterator succ_end() const {
    return map_iterator(NextSlots.end(),
                        slot_getter(pair_first<VASTSlot*, VASTValPtr>));
  }

  // Successor slots of this slot.
  succ_cnd_iterator succ_cnd_begin() { return NextSlots.begin(); }
  succ_cnd_iterator succ_cnd_end() { return NextSlots.end(); }

  const_succ_cnd_iterator succ_cnd_begin() const { return NextSlots.begin(); }
  const_succ_cnd_iterator succ_cnd_end() const { return NextSlots.end(); }
  bool succ_empty() const { return NextSlots.empty(); }
  unsigned succ_size() const { return NextSlots.size(); }

  // Predecessor slots of this slot.
  pred_iterator pred_begin() { return PredSlots.begin(); }
  pred_iterator pred_end() { return PredSlots.end(); }
  const_pred_iterator pred_begin() const { return PredSlots.begin(); }
  const_pred_iterator pred_end() const { return PredSlots.end(); }
  unsigned pred_size() const { return PredSlots.size(); }

  bool operator<(const VASTSlot &RHS) const {
    return SlotNum < RHS.SlotNum;
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