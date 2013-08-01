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
#include "shang/VASTHandle.h"

#include "llvm/ADT/SmallPtrSet.h"
#include <map>

namespace llvm {
class Twine;
class VASTExprBuilder;
class VASTSeqValue;
class STGDistances;
class CachedStrashTable;

class VASTSelector : public VASTNode, public ilist_node<VASTSelector> {
public:
  enum Type {
    Temp,           // Common registers which hold data for data-path.
    Static,         // The register for the static global variables.
    Slot,           // Slot register which hold the enable signals for each slot.
    Enable,         // Register for enable signals.
    FUInput,        // Represent the input of functional unit
    FUOutput        // Represent the output of functional unit
  };

private:
  VASTSelector(const VASTSelector&) LLVM_DELETED_FUNCTION;
  void operator=(const VASTSelector&) LLVM_DELETED_FUNCTION;

  VASTNode* Parent;
  SmallPtrSet<VASTSeqValue*, 8> Defs;
  const uint8_t BitWidth;
  const uint8_t T : 3;

  friend class VASTSeqValue;
  void addUser(VASTSeqValue *V);
  void removeUser(VASTSeqValue *V);

  // Map the transaction condition to transaction value.
  typedef std::vector<VASTLatch> AssignmentVector;
  AssignmentVector Assigns;

  struct UseLess : public std::binary_function<VASTValPtr, VASTValPtr, bool> {
    bool operator()(const VASTUse *LHS, const VASTUse *RHS) const {
      return VASTValPtr(*LHS).get() < VASTValPtr(*RHS).get();
    }
  };

  typedef std::map<VASTHandle, SmallVector<VASTSlot*, 4> > AnnotationMap;
  AnnotationMap Annotations;


  VASTUse Guard, Fanin;

  void printSelector(raw_ostream &OS) const;
  void verifyHoldCycles(vlang_raw_ostream &OS, STGDistances *STGDist,
                        VASTValue *V, ArrayRef<VASTSlot*> ReadSlots) const;

  void instantiateSelector(raw_ostream &OS) const;

public:
  VASTSelector(const char *Name = 0, unsigned BitWidth = 0,
               Type T = Temp, VASTNode *Node = 0);

  ~VASTSelector();

  VASTNode *getParent() const;
  void setParent(VASTNode *N);

  const char *getName() const { return Contents.Name; }
  unsigned getBitWidth() const { return BitWidth; }

  Type getType() const { return Type(T); }
  bool isEnable() const { return getType() == Enable; }
  bool isSlot() const { return getType() == Slot; }
  bool isTemp() const { return getType() == Temp; }
  bool isStatic() const { return getType() == Static; }
  bool isFUOutput() const { return getType() == FUOutput; }
  bool isFUInput() const { return getType() == FUInput; }

  typedef SmallPtrSet<VASTSeqValue*, 8>::const_iterator def_iterator;
  def_iterator def_begin() const { return Defs.begin(); }
  def_iterator def_end() const { return Defs.end(); }
  bool def_empty() const { return Defs.empty(); }
  unsigned num_defs() const { return Defs.size(); }
  bool defines(VASTSeqValue *V) const { return Defs.count(V); }

  typedef AssignmentVector::const_iterator const_iterator;
  const_iterator begin() const { return Assigns.begin(); }
  const_iterator end() const { return Assigns.end(); }
  typedef AssignmentVector::iterator iterator;
  iterator begin() { return Assigns.begin(); }
  iterator end() { return Assigns.end(); }
  unsigned size() const { return Assigns.size(); }
  bool empty() const { return Assigns.empty(); }

  void annotateReadSlot(ArrayRef<VASTSlot*> Slots, VASTValPtr V);
  typedef AnnotationMap::const_iterator ann_iterator;
  ann_iterator ann_begin() const { return Annotations.begin(); }
  ann_iterator ann_end() const { return Annotations.end(); }

  VASTValPtr getGuard() const { return Guard; }
  VASTValPtr getFanin() const { return Fanin; }

  // Return true if the latched value is X (undefined value) or the SeqVal from
  // the same selector.
  bool isTrivialFannin(const VASTLatch &L) const;
  unsigned numNonTrivialFanins() const;
  VASTLatch getUniqueFannin() const;

  void setMux(VASTValPtr Fanin, VASTValPtr Guard);
  void dropMux();

  bool isSelectorSynthesized() const { return !Guard.isInvalid(); }
  void printSelectorModule(raw_ostream &O) const;

  VASTSeqValue *getSSAValue() const;

  void addAssignment(VASTSeqOp *Op, unsigned SrcNo);

  void eraseFanin(VASTLatch U);

  void print(raw_ostream &OS) const;

  void printDecl(raw_ostream &OS) const;
  void printRegisterBlock(vlang_raw_ostream &OS, uint64_t InitVal) const;

  // Generate the code to verify the register assignment.
  void printVerificationCode(vlang_raw_ostream &OS, STGDistances *STGDist) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSelector *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSelector;
  }
};

// Represent values, in SSA form, in the sequential logic.
class VASTSeqValue : public VASTNamedValue, public ilist_node<VASTSeqValue> {
public:
  template<typename Iterator>
  class FaninIterator
    : public std::iterator<std::forward_iterator_tag,
                           typename Iterator::value_type,
                           ptrdiff_t,
                           typename Iterator::pointer,
                           typename Iterator::reference> {
    typedef FaninIterator<Iterator> _Self;
    const VASTSeqValue *V;
    Iterator I, E;

    void skipUnrelated() {
      while (I != E && (*I).getDst() != V)
        ++I;
    }

  public:
    FaninIterator(Iterator I, Iterator E, const VASTSeqValue *V)
      : V(V), I(I), E(E)
    {
      skipUnrelated();
    }

    bool operator== (const _Self &RHS) const { return I == RHS.I; }
    bool operator!= (const _Self &RHS) const { return I != RHS.I; }

    typename Iterator::reference operator*() const { return *I; }
    typename Iterator::pointer operator->() const { return operator*(); }

    _Self &operator++ () {
      ++I;
      skipUnrelated();
      return *this;
    }

    _Self operator++ (int) {
      _Self tmp = *this; ++*this; return tmp;
    }
  };

private:
  // Use pointer to to the Selector, this allow us to change the selector at
  // will.
  VASTSelector *Selector;
  Value *V;

  // For common registers, the Idx is the corresponding register number in the
  // MachineFunction. With this register number we can get the define/use/kill
  // information of transaction to this local storage.
  const unsigned Idx;

  friend struct ilist_sentinel_traits<VASTSeqValue>;
  // Default constructor for ilist_sentinel_traits<VASTSeqOp>.
  VASTSeqValue()
    : VASTNamedValue(vastSeqValue, 0, 0), Selector(0), V(0), Idx(0) {}

public:
  VASTSeqValue(VASTSelector *Selector, unsigned Idx, Value *V);

  ~VASTSeqValue();

  VASTSelector *getSelector() const;
  void changeSelector(VASTSelector *NewSel);

  // Forward the functions from the Selector.
  VASTSelector::Type getType() const { return getSelector()->getType(); }
  bool isEnable() const { return getSelector()->isEnable(); }
  bool isSlot() const { return getSelector()->isSlot(); }
  bool isTemp() const { return getSelector()->isTemp(); }
  bool isStatic() const { return getSelector()->isStatic(); }
  bool isFUOutput() const { return getSelector()->isFUOutput(); }
  bool isFUInput() const { return getSelector()->isFUInput(); }

  unsigned getDataRegNum() const {
    assert(isTemp() && "Wrong accessor!");
    return Idx;
  }

  unsigned getSlotNum() const {
    assert(isSlot() && "Wrong accessor!");
    return Idx;
  }

  VASTNode *getParent() const;
  Value *getLLVMValue() const;

  void printFanins(raw_ostream &OS) const;
  void dumpFanins() const;

  void dropUses() {
    assert(0 && "Function not implemented!");
  }

  typedef FaninIterator<VASTSelector::iterator> fanin_iterator;
  typedef FaninIterator<VASTSelector::const_iterator> const_fanin_iterator;

  fanin_iterator fanin_begin() {
    return fanin_iterator(getSelector()->begin(), getSelector()->end(), this);
  }

  fanin_iterator fanin_end() {
    return fanin_iterator(getSelector()->end(), getSelector()->end(), this);
  }

  const_fanin_iterator fanin_begin() const {
    return const_fanin_iterator(getSelector()->begin(), getSelector()->end(), this);
  }

  const_fanin_iterator fanin_end() const {
    return const_fanin_iterator(getSelector()->end(), getSelector()->end(), this);
  }

  size_t num_fanins() const { return std::distance(fanin_begin(), fanin_end()); }
  bool   fanin_empty() const { return num_fanins() == 0; }

  VASTNode *getProfilePtr() const { return getSelector(); }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSeqValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqValue;
  }
};

class VASTRegister : public VASTNode, public ilist_node<VASTRegister> {
  const uint64_t InitVal;
  VASTSelector *Sel;

  VASTRegister();
  VASTRegister(VASTSelector *Sel, uint64_t InitVal);
  friend class VASTModule;
  friend struct ilist_sentinel_traits<VASTRegister>;
public:
  VASTSelector *getSelector() const { return Sel; }

  // Forward the functions from the Selector.
  unsigned getBitWidth() const { return getSelector()->getBitWidth(); }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTRegister *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastRegister;
  }

  void printDecl(raw_ostream &OS) const;

  void print(raw_ostream &OS) const;
  void print(vlang_raw_ostream &OS) const;
};
}

#endif
