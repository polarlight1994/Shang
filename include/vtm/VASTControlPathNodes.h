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

class VASTSlot : public VASTNode {
public:
  // TODO: Store the pointer to the Slot instead the slot number.
  typedef std::map<VASTSlot*, VASTUse*> SuccVecTy;
  typedef SuccVecTy::iterator succ_cnd_iterator;
  typedef SuccVecTy::const_iterator const_succ_cnd_iterator;

  // Use mapped_iterator which is a simple iterator adapter that causes a
  // function to be dereferenced whenever operator* is invoked on the iterator.
  typedef
  std::pointer_to_unary_function<std::pair<VASTSlot*, VASTUse*>, VASTSlot*>
  slot_getter;

  typedef mapped_iterator<succ_cnd_iterator, slot_getter> succ_iterator;
  typedef mapped_iterator<const_succ_cnd_iterator, slot_getter>
          const_succ_iterator;

  typedef std::map<VASTSeqValue*, VASTUse*> FUCtrlVecTy;
  typedef FUCtrlVecTy::const_iterator const_fu_ctrl_it;

  typedef std::map<VASTValue*, VASTUse*> FUReadyVecTy;
  typedef FUReadyVecTy::const_iterator const_fu_rdy_it;

  typedef SmallVector<VASTSlot*, 4> PredVecTy;
  typedef PredVecTy::iterator pred_it;
private:
  // The relative signal of the slot: Slot register, Slot active and Slot ready.
  VASTUse SlotReg;
  VASTUse SlotActive;
  VASTUse SlotReady;
  // The ready signals that need to wait before we go to next slot.
  FUReadyVecTy Readys;
  // The function units that enabled at this slot.
  FUCtrlVecTy Enables;
  // The function units that need to disable when condition is not satisfy.
  FUCtrlVecTy Disables;

  PredVecTy PredSlots;

  SuccVecTy NextSlots;
  // Slot ranges of alias slot.
  uint16_t StartSlot;
  uint16_t EndSlot;
  uint16_t II;
  // Successor slots of this slot.
  succ_cnd_iterator succ_cnd_begin() { return NextSlots.begin(); }
  succ_cnd_iterator succ_cnd_end() { return NextSlots.end(); }

  const_succ_cnd_iterator succ_cnd_begin() const { return NextSlots.begin(); }
  const_succ_cnd_iterator succ_cnd_end() const { return NextSlots.end(); }

  void dropUses() {
    assert(0 && "Function not implemented!");
  }

  friend class VASTModule;
public:
  const uint16_t SlotNum;

  VASTSlot(unsigned slotNum, MachineInstr *BundleStart, VASTModule *VM);

  MachineBasicBlock *getParentBB() const;
  MachineInstr *getBundleStart() const;

  void buildCtrlLogic(VASTModule &Mod, VASTExprBuilder &Builder);
  // Print the logic of ready signal of this slot, need alias slot information.
  void buildReadyLogic(VASTModule &Mod, VASTExprBuilder &Builder);
  VASTValPtr buildFUReadyExpr(VASTExprBuilder &Builder);

  void print(raw_ostream &OS) const;

  VASTSeqValue *getValue() const;
  const char *getName() const;
  // Getting the relative signals.
  VASTRegister *getRegister() const;
  VASTValue *getReady() const { return cast<VASTValue>(SlotReady); }
  VASTValue *getActive() const { return cast<VASTValue>(SlotActive); }

  void addSuccSlot(VASTSlot *NextSlot, VASTValPtr Cnd, VASTModule *VM);
  bool hasNextSlot(VASTSlot *NextSlot) const;

  // Next VASTSlot iterator.
  succ_iterator succ_begin() {
    return map_iterator(NextSlots.begin(),
                        slot_getter(pair_first<VASTSlot*, VASTUse*>));
  }

  const_succ_iterator succ_begin() const {
    return map_iterator(NextSlots.begin(),
                        slot_getter(pair_first<VASTSlot*, VASTUse*>));
  }

  succ_iterator succ_end() {
    return map_iterator(NextSlots.end(),
                        slot_getter(pair_first<VASTSlot*, VASTUse*>));
  }

  const_succ_iterator succ_end() const {
    return map_iterator(NextSlots.end(),
                        slot_getter(pair_first<VASTSlot*, VASTUse*>));
  }

  // Predecessor slots of this slot.
  pred_it pred_begin() { return PredSlots.begin(); }
  pred_it pred_end() { return PredSlots.end(); }


  VASTUse &allocateEnable(VASTSeqValue *P, VASTModule *VM);
  VASTUse &allocateReady(VASTValue *V, VASTModule *VM);
  VASTUse &allocateDisable(VASTSeqValue *P, VASTModule *VM);
  VASTUse &allocateSuccSlot(VASTSlot *S, VASTModule *VM);

  // Signals need to be enabled at this slot.
  bool isEnabled(VASTSeqValue *P) const { return Enables.count(P); }
  const_fu_ctrl_it enable_begin() const { return Enables.begin(); }
  const_fu_ctrl_it enable_end() const { return Enables.end(); }

  // Signals need to set before this slot is ready.
  bool readyEmpty() const { return Readys.empty(); }
  const_fu_rdy_it ready_begin() const { return Readys.begin(); }
  const_fu_rdy_it ready_end() const { return Readys.end(); }

  // Signals need to be disabled at this slot.
  bool isDiabled(VASTSeqValue *P) const { return Disables.count(P); }
  bool disableEmpty() const { return Disables.empty(); }
  const_fu_ctrl_it disable_begin() const { return Disables.begin(); }
  const_fu_ctrl_it disable_end() const { return Disables.end(); }

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
class VASTSeqValue : public VASTSignal {
public:
  typedef ArrayRef<VASTValPtr> AndCndVec;

private:
  // For common registers, the Idx is the corresponding register number in the
  // MachineFunction. With this register number we can get the define/use/kill
  // information of assignment to this local storage.
  const unsigned T    : 2;
  const unsigned Idx  : 30;

  // Map the assignment condition to assignment value.
  typedef DenseMap<VASTWire*, VASTUse*, VASTWireExpressionTrait> AssignMapTy;
  AssignMapTy Assigns;

  VASTNode &Parent;
public:
  VASTSeqValue(const char *Name, unsigned Bitwidth, VASTNode::SeqValType T,
               unsigned Idx, VASTNode &Parent)
    : VASTSignal(vastSeqValue, Name, Bitwidth), T(T), Idx(Idx),
      Parent(Parent) {}

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSeqValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqValue;
  }

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

  void addAssignment(VASTUse *Src, VASTWire *AssignCnd);
  bool isTimingUndef() const { return getValType() == VASTNode::Slot; }

  typedef AssignMapTy::const_iterator assign_itertor;
  assign_itertor begin() const { return Assigns.begin(); }
  assign_itertor end() const { return Assigns.end(); }
  unsigned size() const { return Assigns.size(); }
  bool empty() const { return Assigns.empty(); }

  void verifyAssignCnd(vlang_raw_ostream &OS, const Twine &Name,
                       const VASTModule *Mod) const;

  void buildCSEMap(std::map<VASTValPtr, std::vector<VASTValPtr> > &CSEMap) const;

  virtual void anchor() const;
};
} // end namespace

#endif
