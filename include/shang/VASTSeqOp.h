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

namespace llvm {
class VASTSeqOp;
class VASTSlot;
class VASTSeqInst;

/// VASTSeqUse - The value at used by the VASTSeqOp at a specific slot. Where
///  "used" means we assign the value to some register.
struct VASTSeqUse {
  VASTSeqOp *Op;
  unsigned No;

  VASTSeqUse(VASTSeqOp *Op, unsigned No) : Op(Op), No(No) {}

  operator VASTUse &() const;
  operator VASTValPtr () const;
  VASTUse &operator->() const;

  // Get the destination of the transaction.
  VASTSeqValue *getDst() const;

  // Forward the functions from VASTSeqOp;
  VASTSlot *getSlot() const;
  VASTUse &getPred() const;
  VASTValPtr getSlotActive() const;
};

/// VASTSeqDef - The value at produced by the VASTSeqOp at a specific slot.
struct VASTSeqDef {
  VASTSeqOp *Op;
  unsigned No;

  VASTSeqDef(VASTSeqOp *Op, unsigned No) : Op(Op), No(No) {}

  VASTSeqOp *operator->() const;
  operator VASTSeqValue *() const;

  const char *getName() const;
};

/// VASTSeqOp - Represent an operation in sequential logic, it read some value
/// and define some others.
class VASTSeqOp : public VASTOperandList, public VASTNode,
                  public ilist_node<VASTSeqOp> {
  SmallVector<VASTSeqValue*, 1> Defs;
  PointerIntPair<VASTSlot*, 1, bool> S;

  friend struct VASTSeqDef;
  friend struct VASTSeqUse;
  friend struct ilist_sentinel_traits<VASTSeqOp>;
  // Default constructor for ilist_sentinel_traits<VASTSeqOp>.
  VASTSeqOp() : VASTOperandList(0, 0), VASTNode(VASTNode::VASTTypes(-1)) {}

  VASTUse &getUseInteranal(unsigned Idx) {
    return getOperand(1 + Idx);
  }

  void operator=(const VASTSeqOp &RHS); // DO NOT IMPLEMENT
  VASTSeqOp(const VASTSeqOp &RHS); // DO NOT IMPLEMENT

protected:
  VASTSeqOp(VASTTypes T, VASTSlot *S, bool UseSlotActive,
            VASTUse *Operands, unsigned Size);
public:
  void addDefDst(VASTSeqValue *Def);
  VASTSeqDef getDef(unsigned No) { return VASTSeqDef(this, No); }
  unsigned getNumDefs() const { return Defs.size(); }

  // Active Slot accessor
  VASTSlot *getSlot() const { return S.getPointer(); }
  unsigned getSlotNum() const;
  VASTValPtr getSlotActive() const;

  // Get the underlying object.
  virtual Value *getValue() const;

  virtual void print(raw_ostream &OS) const;
  void printPredicate(raw_ostream &OS) const;

  // Get the predicate operand of the transaction.
  VASTUse &getPred() { return getOperand(0); }
  const VASTUse &getPred() const { return getOperand(0); }

  // Get the source of the transaction.
  VASTSeqUse getSrc(unsigned Idx) { return VASTSeqUse(this, Idx); };
  // Add a source value to the SeqOp.
  void addSrc(VASTValPtr Src, unsigned SrcIdx, bool IsDef, VASTSeqValue *Dst);

  // Iterate over the source value of register transaction.
  const_op_iterator src_begin() const { return Operands + 1; }
  const_op_iterator src_end() const { return op_end(); }

  op_iterator src_begin() { return Operands + 1; }
  op_iterator src_end() { return op_end(); }

  // Return the used values for the register assignments, the predicate is
  // excluded.
  unsigned getNumSrcs() const { return size() - 1; }
  bool src_empty() const { return getNumSrcs() == 0; }

  // Provide the < operator to support set of VASTSeqDef.
  bool operator<(const VASTSeqOp &RHS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSeqInst *A) { return true; }
  static inline bool classof(const VASTSeqOp *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqInst || A->getASTType() == vastSeqCtrlOp;
  }
};

/// VASTSeqInst - Represent the LLVM Instruction in the sequential logic.
class VASTSeqInst : public VASTSeqOp {
public:
  enum Type {
    Launch, // Launch the LLVM Instruction, e.g. start the memory transaction.
    Latch,  // Latch the result of the Launched LLVM Instruction.
    Alias   // Define the incoming value of a PHI node.
  };
private:
  // Represent the corresponding value in LLVM IR which is in SSA form.
  // The bit represent if this VASTSeqInst alias with other VASTSeqInsts.
  PointerIntPair<Value*, 2, Type> V;
public:
  // VASTSeqInst always use slot active, it is not a part of the control logic.
  VASTSeqInst(Value *V, VASTSlot *S, VASTUse *Operands, unsigned Size,
              VASTSeqInst::Type T);

  Value *getValue() const { return V.getPointer(); }
  VASTSeqInst::Type getSeqOpType() const { return V.getInt(); }

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
  VASTSeqCtrlOp(VASTSlot *S, bool UseSlotActive, VASTUse *Operands);

  Value *getValue() const { return 0; }

  virtual void print(raw_ostream &OS) const;
  static inline bool classof(const VASTSeqCtrlOp *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqCtrlOp;
  }
};
}

#endif