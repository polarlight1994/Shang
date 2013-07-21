//===-------- VASTSeqOp.h - Operations in the Control-path ------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the classes for the operations in the control-path of the
// design.
//
//===----------------------------------------------------------------------===//
#ifndef SHANG_VAST_SEQ_OP_H
#define SHANG_VAST_SEQ_OP_H

#include "shang/VASTNodeBases.h"
#include "llvm/IR/Value.h"
#include "llvm/ADT/PointerUnion.h"

namespace llvm {
class VASTSeqOp;
class VASTSlot;
class VASTSeqInst;

/// VASTLatch - The value at latch by the VASTSeqOp at a specific slot. Where
///  "latch" means we assign the value to some register.
struct VASTLatch {
  VASTSeqOp *Op;
  unsigned No;

  VASTLatch(VASTSeqOp *Op = 0, unsigned No = 0) : Op(Op), No(No) {}

  operator bool() const { return Op; }
  operator VASTUse &() const;
  operator VASTValPtr () const;
  VASTUse &operator->() const;
  void replaceUsedBy(VASTValPtr V) const;
  void replacePredBy(VASTValPtr V, bool UseSlotActive = true) const;
  void removeFromParent();
  void eraseOperand();

  // Get the destination of the transaction.
  VASTSeqValue *getDst() const;
  VASTSelector *getSelector() const;

  // Forward the functions from VASTSeqOp;
  VASTSlot *getSlot() const;
  unsigned getSlotNum() const;
  VASTUse &getGuard() const;
  VASTValPtr getSlotActive() const;

  bool operator==(const VASTLatch &RHS) const {
    return Op == RHS.Op && No == RHS.No;
  }

  bool operator<(const VASTLatch &RHS) const {
    if (Op != RHS.Op) return Op < RHS.Op;

    return No < RHS.No;
  }
};

/// VASTSeqOp - Represent an operation in sequential logic, it read some value
/// and define some others.
class VASTSeqOp : public VASTOperandList, public VASTNode,
                  public ilist_node<VASTSeqOp> {
  SmallVector<VASTSeqValue*, 1> Defs;
  PointerIntPair<VASTSlot*, 1, bool> S;

  friend struct VASTLatch;
  friend struct ilist_sentinel_traits<VASTSeqOp>;
  // Default constructor for ilist_sentinel_traits<VASTSeqOp>.
  VASTSeqOp() : VASTOperandList(0), VASTNode(VASTNode::VASTTypes(-1)) {}

  VASTUse &getUseInteranal(unsigned Idx) {
    return getOperand(1 + Idx);
  }

  void eraseOperand(unsigned Idx);

  void operator=(const VASTSeqOp &RHS); // DO NOT IMPLEMENT
  VASTSeqOp(const VASTSeqOp &RHS); // DO NOT IMPLEMENT

protected:
  VASTSeqOp(VASTTypes T, VASTSlot *S, bool UseSlotActive, unsigned Size);
public:
  VASTSeqValue *getDef(unsigned No);
  unsigned getNumDefs() const { return Defs.size(); }

  // Active Slot accessor
  VASTSlot *getSlot() const { return S.getPointer(); }

  // Set the parent slot to 0.
  void clearParent() { S.setPointer(0); }
  void removeFromParent();

  unsigned getSlotNum() const;

  // Get the underlying object.
  Value *getValue() const;
  void annotateValue(Value *V);

  VASTValPtr getSlotActive() const;
  bool guardedBySlotActive() const { return S.getInt(); }

  virtual void print(raw_ostream &OS) const;
  void printGuard(raw_ostream &OS) const;

  // Get the predicate operand of the transaction.
  VASTUse &getGuard() { return getOperand(0); }
  VASTValPtr getGuard() const { return getOperand(0); }
  void replaceGuardBy(VASTValPtr V, bool UseSlotActive = true);

  // Get the source of the transaction.
  VASTLatch getSrc(unsigned Idx) { return VASTLatch(this, Idx); };

  // Add a source value to the SeqOp.
  void addSrc(VASTValPtr Src, unsigned SrcIdx, VASTSelector *Sel,
              VASTSeqValue *Dst);
  void addSrc(VASTValPtr Src, unsigned SrcIdx, VASTSelector *Sel);
  void addSrc(VASTValPtr Src, unsigned SrcIdx, VASTSeqValue *Dst);

  // Iterate over the source value of register transaction.
  const_op_iterator src_begin() const { return Operands + 1; }
  const_op_iterator src_end() const { return op_end(); }

  op_iterator src_begin() { return Operands + 1; }
  op_iterator src_end() { return op_end(); }

  // Return the used values for the register assignments, the predicate is
  // excluded.
  unsigned num_srcs() const { return size() - 1; }
  bool src_empty() const { return num_srcs() == 0; }

  // Provide the < operator to support set of VASTSeqDef.
  bool operator<(const VASTSeqOp &RHS) const;

  virtual void dropUses();

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSeqInst *A) { return true; }
  static inline bool classof(const VASTSeqOp *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqInst || A->getASTType() == vastSeqCtrlOp
           || A->getASTType() == vastSlotCtrl;
  }
};

/// VASTSeqInst - Represent the LLVM Instruction in the sequential logic.
class VASTSeqInst : public VASTSeqOp {
  unsigned IsLatch: 1;
  unsigned Data   : 15;
public:
  // VASTSeqInst always use slot active, it is not a part of the control logic.
  VASTSeqInst(Value *V, VASTSlot *S, unsigned Size, bool IsLatch);

  bool isLatch() const { return IsLatch; }
  bool isLaunch() const { return !IsLatch; }

  unsigned getCyclesFromLaunch() const {
    assert(isLatch() && "Call getCyclesFromLaunch on the wrong type!");
    return Data;
  }

  void setCyclesFromLaunch(unsigned Cycles) {
    assert(isLatch() && "Call setCyclesFromLaunch on the wrong type!");
    Data = Cycles;
  }

  virtual void print(raw_ostream &OS) const;
  static inline bool classof(const VASTSeqInst *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqInst;
  }
};

/// VASTSeqCtrlOp - Represent assignment to the control signal of the sequential
/// logic.
class VASTSeqCtrlOp : public VASTSeqOp {
public:
  // VASTSeqCtrlOp may not use slot active, it is a part of the control logic.
  // VASTSeqCtrlOp only read one source value and assign it to a register.
  VASTSeqCtrlOp(VASTSlot *S, bool UseSlotActive);

  Value *getValue() const { return 0; }

  virtual void print(raw_ostream &OS) const;
  static inline bool classof(const VASTSeqCtrlOp *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqCtrlOp;
  }
};

/// VASTSeqSlotCtrl - Represent the assignment to the control signal which may
/// conflict with other assignment. These conflicts will be resolved in the
/// ControlLogicSynthesis pass.
/// Please note that UseSlotActive does not make sense in VASTSeqSlotCtrl.
class VASTSlotCtrl : public VASTSeqOp {
  VASTSlot *TargetSlot;
public:
  // VASTSeqSlotCtrl may not use slot active, it is a part of the control logic.
  // VASTSeqSlotCtrl only assign 1 or 0 to the destination VASTSeqValue.
  VASTSlotCtrl(VASTSlot *S, VASTNode *N);

  bool isBranch() const;
  VASTSlot *getTargetSlot() const;
  VASTValue *getWaitingSignal() const;
  VASTNode *getNode() const;

  virtual void print(raw_ostream &OS) const;
  static inline bool classof(const VASTSlotCtrl *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSlotCtrl;
  }
};
}

#endif