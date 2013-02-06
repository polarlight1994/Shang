//===----- VASTSeqValue.h - The Value in the Sequential Logic ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the VASTSeqValue. The VASTSeqValue represent the value in
// the sequential logic, it is not necessary SSA. The VASTSeqOp that define
// the values is available from the VASTSeqValue.
//
//===----------------------------------------------------------------------===//

#ifndef SHANG_VAST_SEQ_VALUE_H
#define SHANG_VAST_SEQ_VALUE_H

#include "shang/VASTSeqOp.h"
#include <map>

namespace llvm {
class Twine;

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

  ~VASTSeqValue();

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
  typedef AssignmentVector::iterator itertor;
  itertor begin() { return Assigns.begin(); }
  itertor end() { return Assigns.end(); }
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

}

#endif
