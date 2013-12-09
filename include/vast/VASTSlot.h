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

#include "vast/VASTNodeBases.h"
#include "vast/VASTDatapathNodes.h"
#include "vast/Utilities.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/DepthFirstIterator.h"

namespace llvm {
class VASTRegister;
class VASTExprBuilder;
class vlang_raw_ostream;
class VASTSlot;
class VASTSeqOp;
class VASTSlotCtrl;

class VASTSlot : public VASTNode, public ilist_node<VASTSlot> {
public:
  // The types of the edges in the STG, the lsb representing the timing distance
  // of the edge, only the successor edge represents a real state transition
  // which have a timing distance of 1.
  enum EdgeType {
    SubGrp = 0,
    Sucessor = 1,
    ImplicitFlow = 2
  };

  // The pointer to successor which is also encoded with the distance.
  struct EdgePtr : public PointerIntPair<VASTSlot*, 2, EdgeType> {
  private:
    typedef PointerIntPair<VASTSlot*, 2, EdgeType> _Base;

    // Hide the function getInt from PointerIntPair.
    void getInt() const { }
  public:
    operator VASTSlot*() const { return getPointer(); }
    VASTSlot *operator->() const { return getPointer(); }
    EdgePtr(VASTSlot *S, EdgeType T) : _Base(S, T) {}

    EdgeType getType() const { return _Base::getInt(); }
    unsigned getDistance() const {
      return _Base::getInt() == Sucessor ? 1 : 0;
    }
  };

  typedef SmallVector<EdgePtr, 4> SuccVecTy;
  typedef SuccVecTy::iterator succ_iterator;
  typedef SuccVecTy::const_iterator const_succ_iterator;

  typedef SmallVector<EdgePtr, 4> PredVecTy;
  typedef PredVecTy::iterator pred_iterator;
  typedef PredVecTy::const_iterator const_pred_iterator;

private:
  // The relative signal of the slot: Slot register, Slot active and Slot ready.
  VASTUse SlotReg;
  VASTUse SlotActive;
  VASTUse SlotGuard;

  // The link to other slots.
  PredVecTy PredSlots;
  SuccVecTy NextSlots;

  // The definitions in the current slot.
  typedef std::vector<VASTSeqOp*> OpVector;
  OpVector Operations;

  void dropUses() {
    assert(0 && "Function not implemented!");
  }

  friend class VASTCtrlRgn;
  VASTSlot(unsigned slotNum, BasicBlock *ParentBB, VASTValPtr Pred,
           bool IsSubGrp, unsigned Schedule);

  VASTSlot() : VASTNode(vastSlot), SlotReg(this), SlotActive(this),
    SlotGuard(this), SlotNum(0), IsSubGrp(true), Schedule(0) {}

  friend struct ilist_sentinel_traits<VASTSlot>;

public:
  template<bool IsConst>
  class SubGrpIterator
    : public std::iterator<std::forward_iterator_tag,
                           typename conditional<IsConst,
                                                const VASTSlot*,
                                                VASTSlot*>::type,
                           ptrdiff_t> {
    typedef SubGrpIterator<IsConst> _Self;
    typedef std::iterator<std::forward_iterator_tag,
                          typename conditional<IsConst,
                                               const VASTSlot*,
                                               VASTSlot*>::type,
                          ptrdiff_t> supper;
    typedef typename supper::value_type value_type;
    typedef df_iterator<value_type> iterator_impl;
    iterator_impl IT;
  public:
    // Begin iterator.
    SubGrpIterator(value_type V) : IT(iterator_impl::begin(V)) {}
    // End iterator.
    SubGrpIterator() : IT(iterator_impl::end(0)) {}

    bool operator== (const _Self &RHS) const { return IT == RHS.IT; }
    bool operator!= (const _Self &RHS) const { return IT != RHS.IT; }

    value_type operator*() const { return *IT; }
    value_type operator->() const { return operator*(); }

    _Self &operator++ () {
      ++IT;

      // Skip the boundaries of the current slot.
      while (IT != iterator_impl::end(0)) {
        value_type NextSlot = *IT;

        // Skip all children of next slot if it is not a subgroup.
        if (NextSlot->IsSubGrp) break;

        IT.skipChildren();
      }

      return *this;
    }

    _Self operator++ (int) {
      _Self tmp = *this; ++*this; return tmp;
    }
  };

  // Create the finish slot.
  explicit VASTSlot(unsigned SlotNum);
  ~VASTSlot();

  typedef uint16_t SlotNumTy;
  const SlotNumTy SlotNum : 15;
  const bool      IsSubGrp : 1;
  const SlotNumTy Schedule;

  void createSignals(VASTModule *VM);
  void copySignals(VASTSlot *S);

  BasicBlock *getParent() const;

  void print(raw_ostream &OS) const;

  VASTSeqValue *getValue() const;
  const char *getName() const;
  // Getting the relative signals.
  VASTRegister *getRegister() const;
  VASTValPtr getActive() const { return SlotActive.unwrap(); }
  VASTUse &getActive() { return SlotActive; }
  VASTUse &getGuard() { return SlotGuard; }

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

  VASTSlot *getSubGroup(BasicBlock *BB) const;
  VASTSlot *getParentState();
  VASTSlot *getParentGroup() const;

  void addSuccSlot(VASTSlot *NextSlot, EdgeType T);

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

  typedef SubGrpIterator<false> subgrp_iterator;
  inline subgrp_iterator subgrp_begin();
  inline subgrp_iterator subgrp_end();

  typedef SubGrpIterator<true> const_subgrp_iterator;
  inline const_subgrp_iterator subgrp_begin() const;
  inline const_subgrp_iterator subgrp_end() const;

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

VASTSlot::subgrp_iterator VASTSlot::subgrp_begin() {
  return subgrp_iterator(this);
}

VASTSlot::subgrp_iterator VASTSlot::subgrp_end() {
  return subgrp_iterator();
}

VASTSlot::const_subgrp_iterator VASTSlot::subgrp_begin() const {
  return const_subgrp_iterator(this);
}

VASTSlot::const_subgrp_iterator VASTSlot::subgrp_end() const {
  return const_subgrp_iterator();
}

} // end namespace

#endif
