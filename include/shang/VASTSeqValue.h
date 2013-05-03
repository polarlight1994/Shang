//===----- VASTSeqValue.h - The Value in the Sequential Logic ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the VASTRegister. The VASTSeqValue represent the value in
// the sequential logic, it is not necessary SSA. The VASTSeqOp that define
// the values is available from the VASTRegister.
//
//===----------------------------------------------------------------------===//

#ifndef SHANG_VAST_SEQ_VALUE_H
#define SHANG_VAST_SEQ_VALUE_H

#include "shang/VASTSeqOp.h"
#include <map>

namespace llvm {
class Twine;
class VASTExprBuilder;

// Represent value in the sequential logic.
class VASTRegister : public VASTNamedValue, public ilist_node<VASTRegister> {
public:

  enum Type {
    Data,           // Common registers which hold data for data-path.
    Slot,           // Slot register which hold the enable signals for each slot.
    IO,             // The I/O port of the module.
    BRAM,           // Port of the block RAM
    Enable,         // The Enable Signal.
    StaticRegister // The register for the static global variables.
  };

  typedef ArrayRef<VASTValPtr> AndCndVec;

  // Synthesized Fanin.
  struct Fanin {
    Fanin(const Fanin&) LLVM_DELETED_FUNCTION;
    void operator=(const Fanin&) LLVM_DELETED_FUNCTION;

    std::vector<VASTSlot*> Slots;
    VASTUse Pred;
    VASTUse FI;

    Fanin(VASTRegister *V);

    void AddSlot(VASTSlot *S);
    typedef std::vector<VASTSlot*>::iterator slot_iterator;
  };
private:
  // For common registers, the Idx is the corresponding register number in the
  // MachineFunction. With this register number we can get the define/use/kill
  // information of transaction to this local storage.
  const unsigned T    : 3;
  const unsigned Idx  : 30;

  // Map the transaction condition to transaction value.
  typedef std::vector<VASTLatch> AssignmentVector;
  AssignmentVector Assigns;

  typedef std::vector<Fanin*> FaninVector;
  FaninVector Fanins;
  VASTUse EnableU;

  VASTNode *Parent;

  bool buildCSEMap(std::map<VASTValPtr,
                            std::vector<const VASTSeqOp*> >
                   &CSEMap) const;

  friend struct ilist_sentinel_traits<VASTRegister>;
  // Default constructor for ilist_sentinel_traits<VASTSeqOp>.
  VASTRegister()
    : VASTNamedValue(vastSeqValue, 0, 0), T(VASTRegister::IO), Idx(0),
      EnableU(this), Parent(this), InitialValue(0) {}

  bool getUniqueLatches(std::set<VASTLatch> &UniqueLatches) const;
public:
  const uint64_t InitialValue;

  VASTRegister(const char *Name, unsigned Bitwidth, Type T, unsigned Idx,
               VASTNode *Parent, uint64_t InitialValue)
    : VASTNamedValue(vastSeqValue, Name, Bitwidth), T(T), Idx(Idx), EnableU(this),
      Parent(Parent), InitialValue(InitialValue) {}

  ~VASTRegister();

  bool isStandAlone() const { return Parent == 0; }

  VASTRegister::Type getValType() const { return VASTRegister::Type(T); }

  unsigned getDataRegNum() const {
    assert((getValType() == Data) && "Wrong accessor!");
    return Idx;
  }

  unsigned getSlotNum() const {
    assert(getValType() == Slot && "Wrong accessor!");
    return Idx;
  }

  VASTNode *getParent() const { return Parent; }

  VASTLatch latchFront() const { return Assigns.front(); }
  void addAssignment(VASTSeqOp *Op, unsigned SrcNo, bool IsDef);
  void eraseLatch(VASTLatch U);

  bool isTimingUndef() const { return getValType() == VASTRegister::Slot; }

  typedef AssignmentVector::const_iterator const_iterator;
  const_iterator begin() const { return Assigns.begin(); }
  const_iterator end() const { return Assigns.end(); }
  typedef AssignmentVector::iterator iterator;
  iterator begin() { return Assigns.begin(); }
  iterator end() { return Assigns.end(); }
  unsigned size() const { return Assigns.size(); }
  bool empty() const { return Assigns.empty(); }

  typedef FaninVector::iterator fanin_iterator;
  fanin_iterator fanin_begin() { return Fanins.begin(); }
  fanin_iterator fanin_end() { return Fanins.end(); }

  typedef FaninVector::const_iterator const_fanin_iterator;
  const_fanin_iterator fanin_begin() const { return Fanins.begin(); }
  const_fanin_iterator fanin_end() const { return Fanins.end(); }
  bool isSelectorSynthesized() const { return !EnableU.isInvalid(); }
  VASTValPtr getEnable() const { return VASTValPtr(EnableU); }

  // Functions to write the verilog code.
  void verifyAssignCnd(vlang_raw_ostream &OS, const Twine &Name,
                       const VASTModule *Mod) const;
  bool verify() const;
  void printSelector(raw_ostream &OS, unsigned Bitwidth,
                     bool PrintEnable = true) const;
  void printSelector(raw_ostream &OS, bool PrintEnable = true) const {
    printSelector(OS, getBitWidth(), PrintEnable);
  }

  void printStandAloneDecl(raw_ostream &OS) const;
  void printStandAlone(vlang_raw_ostream &OS, const VASTModule *Mod) const;

  void dumpFanins() const;

  void dropUses() {
    assert(0 && "Function not implemented!");
  }

  void synthesisSelector(VASTExprBuilder &Builder);

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTRegister *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqValue;
  }

  virtual void anchor() const;
};

}

#endif
