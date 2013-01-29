//===- VASTControlpathNodes.h - Control path Nodes in VerilogAST -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the Control-path Nodes in the Verilog Abstract Syntax Tree.
//
//===----------------------------------------------------------------------===//
#ifndef VTM_VAST_CONTROL_PATH_NODES_H
#define VTM_VAST_CONTROL_PATH_NODES_H

#include "vtm/VASTNodeBases.h"
#include "vtm/VASTDatapathNodes.h"
#include "vtm/Utilities.h"

#include "llvm/ADT/STLExtras.h"

namespace llvm {
class VASTRegister;
class MachineBasicBlock;
class VASTExprBuilder;
class vlang_raw_ostream;
class VASTSlot;
class VASTSeqOp;

// The value at used by the VASTSeqOp at a specific slot. Where "used" means we
// assign the value to some register.
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

// The value at produced by the VASTSeqOp at a specific slot.
struct VASTSeqDef {
  VASTSeqOp *Op;
  unsigned No;

  VASTSeqDef(VASTSeqOp *Op, unsigned No) : Op(Op), No(No) {}

  VASTSeqOp *operator->() const;
  operator VASTSeqValue *() const;

  const char *getName() const;
};

// Represent an operation in seqential logic, it read some value and define some
// others.
class VASTSeqOp : public VASTOperandList, public VASTNode,
                  public ilist_node<VASTSeqOp> {
  SmallVector<VASTSeqValue*, 1> Defs;
  PointerIntPair<VASTSlot*, 1, bool> S;
  PointerIntPair<MachineInstr*, 1, bool> DefMI;

  friend struct VASTSeqDef;
  friend struct VASTSeqUse;
  friend struct ilist_sentinel_traits<VASTSeqOp>;
  // Default constructor for ilist_sentinel_traits<VASTSeqOp>.
  VASTSeqOp() : VASTOperandList(0, 0), VASTNode(vastSeqOp) {}

  VASTUse &getUseInteranal(unsigned Idx) {
    return getOperand(1 + Idx);
  }

  void operator=(const VASTSeqOp &RHS); // DO NOT IMPLEMENT
  VASTSeqOp(const VASTSeqOp &RHS); // DO NOT IMPLEMENT
public:
  VASTSeqOp(VASTSlot *S, bool UseSlotActive, MachineInstr *DefMI,
            VASTUse *Operands, unsigned Size, bool IsVirtual = false);

  void addDefDst(VASTSeqValue *Def);
  VASTSeqDef getDef(unsigned No) { return VASTSeqDef(this, No); }
  unsigned getNumDefs() const { return Defs.size(); }

  // Active Slot accessor
  VASTSlot *getSlot() const { return S.getPointer(); }
  unsigned getSlotNum() const;
  VASTValPtr getSlotActive() const;

  //
  MachineInstr *getDefMI() const { return DefMI.getPointer(); }
  bool isVirtual() const { return DefMI.getInt(); }

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
  static inline bool classof(const VASTSeqOp *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqOp;
  }
};

class VASTSlot : public VASTNode {
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
  // Slot ranges of alias slot.
  uint16_t StartSlot;
  uint16_t EndSlot;
  uint16_t II;

  // The definitions in the current slot.
  typedef std::vector<VASTSeqOp*> OpVector;
  OpVector Operations;

  void dropUses() {
    assert(0 && "Function not implemented!");
  }

  friend class VASTModule;
  VASTSlot(unsigned slotNum, MachineBasicBlock *ParentBB, VASTModule *VM);
  // Create the finish slot.
  VASTSlot(unsigned slotNum, VASTSlot *StartSlot);
public:
  const uint16_t SlotNum;

  MachineBasicBlock *getParentBB() const;

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

  // Predecessor slots of this slot.
  pred_iterator pred_begin() { return PredSlots.begin(); }
  pred_iterator pred_end() { return PredSlots.end(); }
  const_pred_iterator pred_begin() const { return PredSlots.begin(); }
  const_pred_iterator pred_end() const { return PredSlots.end(); }
  unsigned pred_size() const { return PredSlots.size(); }

  // This slots alias with this slot, this happened in a pipelined loop.
  // The slots from difference stage of the loop may active at the same time,
  // and these slot called "alias".
  void setAliasSlots(unsigned startSlot, unsigned endSlot, unsigned ii) {
    StartSlot = startSlot ;
    EndSlot = endSlot;
    II = ii;
  }

  // Is the current slot the first slot of its alias slots?
  bool isLeaderSlot() const { return StartSlot == SlotNum; }
  // Iterates over all alias slot
  unsigned alias_start() const { return StartSlot; }
  unsigned alias_end() const { return EndSlot; }
  bool hasAliasSlot() const { return alias_start() != alias_end(); }
  unsigned alias_ii() const {
    assert(hasAliasSlot() && "Dont have II!");
    return II;
  }

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

// Represent value in the sequential logic.
class VASTSeqValue : public VASTSignal, public ilist_node<VASTSeqValue> {
public:
  typedef ArrayRef<VASTValPtr> AndCndVec;

private:
  // For common registers, the Idx is the corresponding register number in the
  // MachineFunction. With this register number we can get the define/use/kill
  // information of transaction to this local storage.
  const unsigned T    : 2;
  const unsigned Idx  : 30;

  // Map the transaction condition to transaction value.
  typedef std::vector<VASTSeqUse> AssignmentVector;
  AssignmentVector Assigns;

  VASTNode &Parent;

  bool buildCSEMap(std::map<VASTValPtr,
                            std::vector<const VASTSeqOp*> >
                   &CSEMap) const;

  friend struct ilist_sentinel_traits<VASTSeqValue>;
  // Default constructor for ilist_sentinel_traits<VASTSeqOp>.
  VASTSeqValue()
    : VASTSignal(vastSeqValue, 0, 0), T(VASTNode::IO), Idx(0), Parent(*this) {}

public:
  VASTSeqValue(const char *Name, unsigned Bitwidth, VASTNode::SeqValType T,
               unsigned Idx, VASTNode &Parent)
    : VASTSignal(vastSeqValue, Name, Bitwidth), T(T), Idx(Idx),
      Parent(Parent) {}

  VASTNode::SeqValType getValType() const { return VASTNode::SeqValType(T); }

  unsigned getDataRegNum() const {
    assert((getValType() == Data) && "Wrong accessor!");
    return Idx;
  }

  unsigned getSlotNum() const {
    assert(getValType() == Slot && "Wrong accessor!");
    return Idx;
  }

  VASTNode *getParent() { return &Parent; }
  const VASTNode *getParent() const { return &Parent; }

  void addAssignment(VASTSeqOp *Op, unsigned SrcNo, bool IsDef);

  bool isTimingUndef() const { return getValType() == VASTNode::Slot; }

  typedef AssignmentVector::const_iterator const_itertor;
  const_itertor begin() const { return Assigns.begin(); }
  const_itertor end() const { return Assigns.end(); }
  unsigned size() const { return Assigns.size(); }
  bool empty() const { return Assigns.empty(); }

  // Functions to write the verilog code.
  void verifyAssignCnd(vlang_raw_ostream &OS, const Twine &Name,
                       const VASTModule *Mod) const;
  bool verify() const;
  void printSelector(raw_ostream &OS, unsigned Bitwidth) const;
  void printSelector(raw_ostream &OS) const {
    printSelector(OS, getBitWidth());
  }

  void dropUses() {
    assert(0 && "Function not implemented!");
  }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSeqValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqValue;
  }

  virtual void anchor() const;
};
} // end namespace

#endif
