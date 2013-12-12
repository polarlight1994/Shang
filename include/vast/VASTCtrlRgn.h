//===----- VASTCtrlRgn.h - Control Region in VASTModule ---------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the control regions in a VASTModule, it may corresponds to
// the control flow of a function, or a loop body, etc. It is a subclass of
// VASTSubmoduleBase, because it is something like a submodule: it has fanins
// (live-in values), fanouts (live-out values). But there is no start/fin signal
// because the STG of VASTCtrlRgn is directly connected to its parent control
// region. Unlike VASTModule, the VASTCtrlRgn to not own other VASTNodes except
// the VASTSlots and VASTSeqOps.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_CONTROL_REGION_H
#define VAST_CONTROL_REGION_H

#include "vast/VASTNodeBases.h"
#include "vast/VASTSlot.h"
#include "vast/VASTSeqOp.h"

namespace llvm {
class VASTModule;

class VASTCtrlRgn : public VASTSubModuleBase {
  VASTModule *const Parent;
  // The parent function for this module.
  Function *const F;
  // TODO: For a sub region of the function, provide the set of basic blocks
  // or the Loop/Region object.

  typedef ilist<VASTSlot> SlotListTy;
  typedef ilist<VASTSeqOp> SeqOpListTy;

  SlotListTy Slots;

  SeqOpListTy Ops;

  friend class VASTModule;

  VASTCtrlRgn(VASTModule &Parent, Function &F, const char *Name);
  VASTCtrlRgn();


  VASTSeqCtrlOp *createCtrlLogic(VASTValPtr Src, VASTSlot *Slot,
                                 VASTValPtr GuardCnd, bool UseSlotActive);
protected:
  // Allow releasing the resources in CtrlRgn explicitly.
  void finalize();

public:
  ~VASTCtrlRgn();

  VASTModule *getParentModule() const;
  Function *getFunction() const;
  BasicBlock *getEntryBlock() const;

  //
  void addFanin(VASTSelector *S) {
    VASTSubModuleBase::addFanin(S);
  }

  void addFanout(VASTSelector *S) {
    VASTSubModuleBase::addFanout(S);
  }

  // The STG related functions.
  typedef SlotListTy::iterator slot_iterator;
  typedef SlotListTy::const_iterator const_slot_iterator;
  slot_iterator slot_begin() { return Slots.begin(); }
  slot_iterator slot_end() { return Slots.end(); }

  const_slot_iterator slot_begin() const { return Slots.begin(); }
  const_slot_iterator slot_end() const { return Slots.end(); }

  SlotListTy &getSLotList() { return Slots; }

  VASTSlot *createSlot(unsigned SlotNum, BasicBlock *ParentBB, unsigned Schedule,
                       VASTValPtr Pred = VASTImmediate::True,
                       bool IsVirtual = false);

  VASTSlot *getLandingSlot();
  VASTSlot *createLandingSlot();

  VASTSlot *getStartSlot();
  VASTSlot *getFinishSlot();

  // Operations in the current control region. 
  // Create a SeqOp that contains NumOps operands, please note that the predicate
  // operand is excluded from NumOps.
  VASTSeqInst *lauchInst(VASTSlot *Slot, VASTValPtr Pred, unsigned NumOps,
                         Value *V, bool IsLatch);

  VASTSeqInst *latchValue(VASTSeqValue *SeqVal, VASTValPtr Src, VASTSlot *Slot,
                          VASTValPtr GuardCnd, Value *V, unsigned Latency = 0);

  /// Create an assignment on the control logic.
  VASTSeqCtrlOp *assignCtrlLogic(VASTSeqValue *SeqVal, VASTValPtr Src,
                                 VASTSlot *Slot, VASTValPtr GuardCnd,
                                 bool UseSlotActive);
  VASTSeqCtrlOp *assignCtrlLogic(VASTSelector *Selector, VASTValPtr Src,
                                 VASTSlot *Slot, VASTValPtr GuardCnd,
                                 bool UseSlotActive);
  /// Create an assignment on the control logic which may need further conflict
  /// resolution.
  VASTSlotCtrl *createStateTransition(VASTNode *N, VASTSlot *Slot,
                                      VASTValPtr GuardCnd);

  // Iterate over all operations in current control region.
  typedef SeqOpListTy::iterator seqop_iterator;
  typedef SeqOpListTy::const_iterator const_seqop_iterator;
  seqop_iterator seqop_begin() { return Ops.begin(); }
  seqop_iterator seqop_end() { return Ops.end(); }
  SeqOpListTy &getOpList() { return Ops; }

  void print(raw_ostream &OS) const;

  void viewGraph() const;

  /// Perform the Garbage Collection to release the dead objects on the
  /// VASTModule
  bool gc();

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTCtrlRgn *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastCtrlRegion;
  }
};
}

#endif
