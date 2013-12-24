//===----- VASTNodeBases.h - Base Classes in VerilogAST ---------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the base classes in the Verilog Abstract Syntax Tree.
//
//===----------------------------------------------------------------------===//
#ifndef VAST_NODE_BASE_H
#define VAST_NODE_BASE_H

#include "vast/VASTBitMask.h"

#include "llvm/ADT/None.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Casting.h"

#include <set>
#include <vector>

namespace llvm {
class BasicBlock;
class Value;
class Twine;
template<typename T> class ArrayRef;
}

namespace vast {
class VASTNamedValue;
class VASTValue;
class VASTExpr;
class VASTWrapper;
class VASTSeqValue;
class VASTSelector;
class VASTSymbol;
class VASTRegister;
class VASTSeqOp;
class VASTMemoryBank;
class VASTModule;
class vlang_raw_ostream;

using namespace llvm;
class VASTNode {
public:
  // Leaf node type of Verilog AST.
  enum VASTTypes {
    vastConstant,
    vastFirstValueType = vastConstant,
    vastSymbol,
    vastFirstMaskedValueType = vastSymbol,
    vastExpr,
    vastWrapper,
    vastSeqValue,
    vastLastValueType = vastSeqValue,
    vastLastMaskedValueType = vastSeqValue,

    vastInPort,
    vastOutPort,
    vastSlot,
    vastRegister,
    vastCtrlRegion,
    vastSubmodule,
    vastMemoryBank,

    vastSelector,
    // Fine-grain control flow.
    vastSeqInst,
    vastSeqCtrlOp,
    vastSlotCtrl,

    // Handle of the VASTValPtr, make sure the replacement in the datapath do
    // not invalid the external use.
    vastHandle,

    vastModule
  };

protected:
  union VASTNodeContents64Ty {
    const char *Name;
    BasicBlock *ParentBB;
    Value *LLVMValue;
    VASTSelector *Sel;
    VASTMemoryBank *Bank;
    const char *SOP;
    VASTNode *Node;
    uint64_t Int;
  } Contents64;

  union VASTNodeContents32Ty {
    uint32_t ExprNameID;
  } Contents32;

  union VASTNodeContents16Ty {
    uint16_t SeqInstData;
    struct VASTExprContentsTy {
      uint8_t Opcode;
      uint8_t LB;
    } ExprContents;
  } Contents16;

  union VASTNodeContents8Ty {
    uint8_t ValueBitwidth;
    bool SeqInstIsLatch : 1;
  } Contents8;

  const uint8_t NodeT : 7;
  bool IsDead         : 1;
  explicit VASTNode(VASTTypes T) : NodeT(T), IsDead(false) {}

  virtual void print(raw_ostream &OS) const = 0;

  friend class DatapathContainer;

  // Drop this VASTNode from the userlist of all its uses.
  virtual void dropUses();
public:
  virtual ~VASTNode() {}

  VASTTypes getASTType() const { return VASTTypes(NodeT); }
  bool isDead() const { return IsDead; }
  void setDead() { IsDead = true; }

  void dump() const;

  // Get the name of the object in timequest timing netlist.
  // FIXME: Make this a virtual function.
  std::string getSTAObjectName() const;

  static std::string DirectClkEnAttr, ParallelCaseAttr, FullCaseAttr;
};

template<typename T>
struct PtrInvPair : public PointerIntPair<T*, 1, bool> {
  typedef PointerIntPair<T*, 1, bool> Base;

  PtrInvPair(Base V) : PointerIntPair<T*, 1, bool>(V) {}

  PtrInvPair(T *V = NULL, bool IsInvert = false)
    : PointerIntPair<T*, 1, bool>(V, IsInvert) {}

  PtrInvPair(NoneType) : PointerIntPair<T*, 1, bool>(NULL, false) {}

  template<typename T1>
  PtrInvPair(const PtrInvPair<T1>& RHS)
    : PointerIntPair<T*, 1, bool>(RHS.get(), RHS.isInverted()) {}

  template<typename T1>
  PtrInvPair<T> &operator=(const PtrInvPair<T1> &RHS) {
    setPointer(RHS.get());
    setInt(RHS.isInverted());
    return *this;
  }

  operator void*() const {
    return get() ? this->getOpaqueValue() : NULL;
  }

  T *get() const { return this->getPointer(); }

  template<typename T1>
  T1 *getAsLValue() const { return dyn_cast_or_null<T1>(this->getPointer()); }

  bool isInverted() const { return this->getInt(); }
  PtrInvPair<T> invert(bool Invert = true) const {
    return Invert ? PtrInvPair<T>(get(), !isInverted()) : *this;
  }

  T *operator->() const { return this->get(); }

  // Forwarding function of VASTValues
  inline PtrInvPair<VASTValue> getOperand(unsigned i) const;
  inline PtrInvPair<VASTExpr> getExpr() const;
  inline APInt getAPInt() const;
  inline uint64_t getZExtValue() const;
  inline bool getBoolValue() const;
  inline APInt getBitSlice(unsigned UB, unsigned LB = 0) const;
  inline bool isAllZeros() const;
  inline bool isAllOnes() const;
  inline bool isMaxSigned() const;
  inline bool isMinSigned() const;

  // PtrInvPairs are equal when their Opaque Value are equal, which contain the
  // pointer and Int information.
  template<typename T1>
  bool operator==(const T1 *RHS) const {  return this->getOpaqueValue() == RHS; }
  template<typename T1>
  bool operator!=(const T1 *RHS) const { return !operator==(RHS); }
  template<typename T1>
  bool operator<(const T1 *RHS) const { return this->getOpaqueValue() < RHS; }
  template<typename T1>
  bool operator>(const T1 *RHS) const { return this->getOpaqueValue() > RHS; }

  bool operator==(NoneType) const {
    return Base::getPointer() == NULL;
  }

  bool operator!=(NoneType) const {
    return !operator==(None);
  }

  inline void printAsOperand(raw_ostream &OS, unsigned UB, unsigned LB) const {
    get()->printAsOperand(OS, UB, LB, isInverted());
  }

  inline void printAsOperand(raw_ostream &OS) const {
    get()->printAsOperand(OS, isInverted());
  }

  template<typename T1>
  bool isa() const { return llvm::isa<T1>(get()); }

  static bool type_less(PtrInvPair<T> LHS, PtrInvPair<T> RHS) {
    if (LHS->getASTType() < RHS->getASTType())
     return true;
    else if (LHS->getASTType() > RHS->getASTType())
     return false;

    return LHS.getOpaqueValue() < RHS.getOpaqueValue();
  }
private:
  // Hide the confusing getInt function.
  bool getInt() const { return Base::getInt(); }
};
} // end namespace vast

namespace llvm {
using namespace vast;

template<typename T>
struct DenseMapInfo<PtrInvPair<T> >
  : public DenseMapInfo<PointerIntPair<T*, 1, bool> > {};

template<typename T>
inline raw_ostream &operator<<(raw_ostream &OS, PtrInvPair<T> V) {
  V.printAsOperand(OS);
  return OS;
}

// Casting PtrInvPair.
template<class To, class From>
struct cast_retty_impl<PtrInvPair<To>, PtrInvPair<From> > {
  typedef PtrInvPair<To> ret_type;
};

template<class To, class From>
struct cast_retty_impl<PtrInvPair<To>, const PtrInvPair<From> >
  : public cast_retty_impl<PtrInvPair<To>, PtrInvPair<From> > {};


template<class ToTy, class FromTy>
struct cast_convert_val<PtrInvPair<ToTy>,
                        PtrInvPair<FromTy>,
                        PtrInvPair<FromTy> > {
  typedef PtrInvPair<ToTy> To;
  typedef PtrInvPair<FromTy> From;
  static typename cast_retty<To, From>::ret_type doit(const From &Val) {
    return To(cast_convert_val<ToTy, FromTy*, FromTy*>::doit(Val.get()),
              Val.isInverted());
  }
};


template<class ToTy, class FromTy>
struct cast_convert_val<PtrInvPair<ToTy>,
                        const PtrInvPair<FromTy>,
                        const PtrInvPair<FromTy> >
  : public cast_convert_val<PtrInvPair<ToTy>,
                            PtrInvPair<FromTy>,
                            PtrInvPair<FromTy> >
{};

template <typename To, typename From>
struct isa_impl<PtrInvPair<To>, PtrInvPair<From> > {
  static inline bool doit(const PtrInvPair<From> &Val) {
    return To::classof(Val.get());
  }
};

template <typename To, typename From>
struct isa_impl<PtrInvPair<To>, const PtrInvPair<From> >
  : public isa_impl<PtrInvPair<To>, PtrInvPair<From> > {};

template<class To, class From> struct cast_retty_impl<To, PtrInvPair<From> >
  : public cast_retty_impl<To, From*> {};

template<class To, class From> struct cast_retty_impl<To, const PtrInvPair<From> >
  : public cast_retty_impl<To, const From*> {};

template<class To, class FromTy> struct cast_convert_val<To,
                                                         PtrInvPair<FromTy>,
                                                         PtrInvPair<FromTy> > {
  typedef PtrInvPair<FromTy> From;
  static typename cast_retty<To, From>::ret_type doit(const From &Val) {
    return cast_convert_val<To, FromTy*, FromTy*>::doit(Val.get());
  }
};

template<class To, class FromTy>
struct cast_convert_val<To,
                        const PtrInvPair<FromTy>,
                        const PtrInvPair<FromTy> >
  : public cast_convert_val<To, PtrInvPair<FromTy>, PtrInvPair<FromTy> >
{};

template <typename To, typename From>
struct isa_impl<To, PtrInvPair<From> > {
  static inline bool doit(const PtrInvPair<From> &Val) {
    return !Val.isInverted() && To::classof(Val.get());
  }
};

template <typename To, typename From>
struct isa_impl<To, const PtrInvPair<From> > : public isa_impl<To, PtrInvPair<From> > {};
} // end namespace llvm

namespace vast {
using namespace llvm;
typedef PtrInvPair<VASTValue> VASTValPtr;

class VASTUse : public ilist_node<VASTUse> {
  VASTNode &User;
  VASTValPtr V;

  friend struct ilist_sentinel_traits<VASTUse>;

  void linkUseToUser();

  void operator=(const VASTUse &RHS); // DO NOT IMPLEMENT
  VASTUse(const VASTUse &RHS); // DO NOT IMPLEMENT
public:
  VASTUse(VASTNode *User, VASTValPtr V = 0);

  bool isInvalid() const { return !V; }
  void reset() {
    if (isInvalid()) return;

    unlinkUseFromUser();
    V = None;
  }

  void set(VASTValPtr RHS) {
    assert(!V && "Already using some value!");
    V = RHS;
    linkUseToUser();
  }

  void replaceUseBy(VASTValPtr RHS) {
    assert(V && V != RHS && "Cannot replace!");
    unlinkUseFromUser();
    V = RHS;
    linkUseToUser();
  }

  // Get the user of this use.
  VASTNode &getUser() const { return User; }

  // Remove this use from use list.
  void unlinkUseFromUser();

  bool operator==(NoneType) const {
    return isInvalid();
  }

  bool operator!=(NoneType) const {
    return !operator==(None);
  }

  bool operator==(const VASTValPtr RHS) const;

  bool operator!=(const VASTValPtr RHS) const {
    return !operator==(RHS);
  }

  bool operator<(const VASTUse &RHS) const {
    return V < RHS.V;
  }

  bool operator>(const VASTUse &RHS) const {
    return V > RHS.V;
  }

  // Return the underlying VASTValue.
  const VASTValPtr &get() const {
    assert(!isInvalid() && "Not a valid Use!");
    return V;
  }

  template<typename T>
  inline T *getAsLValue() const { return get().getAsLValue<T>(); }

  template<typename T>
  bool isa() const { return get().isa<T>(); }

  operator VASTValPtr() const { return get(); }

  VASTValPtr operator->() const { return get(); }

  bool isInverted() const { return get().isInverted(); }
  VASTValPtr invert(bool Invert = true) const { return get().invert(Invert); }

  inline void printAsOperand(raw_ostream &OS, unsigned UB, unsigned LB) const {
    get().printAsOperand(OS, UB, LB);
  }

  inline void printAsOperand(raw_ostream &OS) const {
    get().printAsOperand(OS);
  }

  const VASTValPtr &unwrap() const { return V; }
};
} // end namespace vast

namespace llvm {
using namespace vast;
template<>
struct ilist_traits<VASTUse> : public ilist_default_traits<VASTUse> {
  // FIXME: This sentinel is created and never released.
  static VASTUse *createSentinel() { return new VASTUse(NULL, NULL); }

  static void deleteNode(VASTUse *U) {}
};
} // end namespace llvm

namespace vast {
using namespace llvm;

template<class IteratorType, class NodeType>
class VASTUseIterator : public std::iterator<std::forward_iterator_tag,
                                             NodeType*, ptrdiff_t> {
    IteratorType I;   // std::vector<MSchedGraphEdge>::iterator or const_iterator
    typedef VASTUseIterator<IteratorType, NodeType> Self;
public:
  VASTUseIterator(IteratorType i) : I(i) {}

  bool operator==(const Self RHS) const { return I == RHS.I; }
  bool operator!=(const Self RHS) const { return I != RHS.I; }

  const Self &operator=(const Self &RHS) {
    I = RHS.I;
    return *this;
  }

  NodeType* operator*() const {
    return &I->getUser();
  }

  NodeType* operator->() const { return operator*(); }

  Self& operator++() {                // Preincrement
    ++I;
    return *this;
  }

  VASTUseIterator operator++(int) { // Postincrement
    VASTUseIterator tmp = *this;
    ++*this;
    return tmp;
  }

  VASTUse *get() { return I; }
};

class VASTOperandList {
  friend class VASTCtrlRgn;
  friend class DatapathContainer;
protected:
  VASTUse *Operands;
  unsigned Size;
public:

  VASTOperandList(unsigned Size);
  ~VASTOperandList();

  const VASTUse &getOperand(unsigned Idx) const {
    assert(Idx < Size && "Index out of range!");
    return Operands[Idx];
  }

  VASTUse &getOperand(unsigned Idx) {
    assert(Idx < Size && "Index out of range!");
    return Operands[Idx];
  }

  typedef const VASTUse *const_op_iterator;
  const_op_iterator op_begin() const { return Operands; }
  const_op_iterator op_end() const { return Operands + Size; }

  typedef VASTUse *op_iterator;
  op_iterator op_begin() { return Operands; }
  op_iterator op_end() { return Operands + Size; }

  unsigned size() const { return Size; }
  //typedef VASTUse *op_iterator;
  //op_iterator op_begin() const { return ops(); }
  //op_iterator op_end() const { return ops() + num_ops(); }

  ArrayRef<VASTUse> getOperands() const;

  void dropOperands();
};

class VASTValue : public VASTNode {
  typedef iplist<VASTUse> UseListTy;
  UseListTy UseList;
protected:

  VASTValue(VASTTypes T, unsigned BitWidth);

  void addUseToList(VASTUse *U) { UseList.push_back(U); }
  void removeUseFromList(VASTUse *U) { UseList.remove(U); }

  friend class VASTUse;

  virtual void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const;

  virtual void printAsOperandImpl(raw_ostream &OS) const {
    printAsOperandImpl(OS, getBitWidth(), 0);
  }

public:
  virtual ~VASTValue();
  unsigned getBitWidth() const { return Contents8.ValueBitwidth; }

  typedef VASTUseIterator<UseListTy::iterator, VASTNode> use_iterator;
  use_iterator use_begin() { return use_iterator(UseList.begin()); }
  use_iterator use_end() { return use_iterator(UseList.end()); }

  bool use_empty() const { return UseList.empty(); }
  size_t num_uses() const { return UseList.size(); }

  void printAsOperand(raw_ostream &OS, unsigned UB, unsigned LB,
                      bool isInverted) const;
  void printAsOperand(raw_ostream &OS, bool isInverted) const;

  virtual void print(raw_ostream &OS) const;

  // Helper function.
  static std::string BitRange(unsigned UB, unsigned LB = 0,
                              bool printOneBit = false);

  // Extract all SeqVals which are connect to this VASTValue through data-path.
  // Return true if there is any supporting sval.
  bool extractSupportingSeqVal(std::set<VASTSeqValue*> &SeqVals,
                               bool StopAtTimingBarrier = false);

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() >= vastFirstValueType &&
           A->getASTType() <= vastLastValueType;
  }
};
}// end namespace vast

namespace llvm {
using namespace vast;

// simplify_type - Allow clients to treat VASTRValue just like VASTValues when
// using casting operators.
template<> struct simplify_type<VASTUse> {
  typedef VASTValPtr SimpleType;        // The real type this represents...

  // An accessor to get the real value...
  static SimpleType getSimplifiedValue(const VASTUse &Val) {
    return Val.unwrap();
  }
};

template<>
struct simplify_type<const VASTUse> : public simplify_type<VASTUse> {};
} // end namespace llvm

namespace vast {
using namespace llvm;

// VASTValue with bitmask.
class VASTMaskedValue : public VASTValue, public VASTBitMask {
protected:
  VASTMaskedValue(VASTTypes T, unsigned BitWidth);
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTMaskedValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() >= vastFirstMaskedValueType &&
           A->getASTType() <= vastLastMaskedValueType;
  }

  bool evaluateMask();
};

class VASTNamedValue : public VASTMaskedValue {
protected:
  VASTNamedValue(VASTTypes T, const char *Name, unsigned BitWidth)
    : VASTMaskedValue(T, BitWidth) {
    assert((T == vastSymbol || T == vastWrapper || T == vastSeqValue)
           && "Bad DeclType!");
    Contents64.Name = Name;
  }

  virtual void printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                  unsigned LB) const;
  void printAsOperandImpl(raw_ostream &OS) const {
    printAsOperandImpl(OS, getBitWidth(), 0);
  }

public:
  const char *getName() const { return Contents64.Name; }

  static void PrintDecl(raw_ostream &OS, const Twine &Name, unsigned BitWidth,
                        bool declAsRegister, const char *Terminator = ";\n");
  void printDecl(raw_ostream &OS, bool declAsRegister,
                 const char *Terminator = ";\n") const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTNamedValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSymbol ||
           A->getASTType() == vastWrapper ||
           A->getASTType() == vastSeqValue;
  }

  static inline std::string Mangle(const std::string &S) {
    std::string Result;

    for (unsigned i = 0, e = S.size(); i != e; ++i) {
      if (isalnum(S[i]) || S[i] == '_') {
        Result += S[i];
      } else {
        Result += '_';
        Result += 'A'+(S[i]&15);
        Result += 'A'+((S[i]>>4)&15);
        Result += '_';
      }
    }

    return Result;
  }
};

class VASTSubModuleBase : public VASTNode,
                          public ilist_node<VASTSubModuleBase> {
  SmallVector<VASTSelector*, 8> Fanins;

  SmallVector<VASTSelector*, 4> Fanouts;
protected:
  const unsigned Idx;
  VASTSubModuleBase() : VASTNode(VASTTypes(-1)), Idx(0) {}

  VASTSubModuleBase(VASTTypes DeclType, const char *Name, unsigned Idx)
    : VASTNode(DeclType), Idx(Idx) {
    Contents64.Name = Name;
  }

  void addFanin(VASTSelector *S);
  void addFanout(VASTSelector *V);

  friend struct ilist_sentinel_traits<VASTSubModuleBase>;
public:
  ~VASTSubModuleBase() {}

  typedef SmallVectorImpl<VASTSelector*>::iterator fanin_iterator;
  fanin_iterator fanin_begin() { return Fanins.begin(); }
  fanin_iterator fanin_end() { return Fanins.end(); }

  typedef SmallVectorImpl<VASTSelector*>::const_iterator const_fanin_iterator;
  const_fanin_iterator fanin_begin() const { return Fanins.begin(); }
  const_fanin_iterator fanin_end()   const { return Fanins.end(); }

  typedef SmallVectorImpl<VASTValue*>::iterator fanout_iterator;

  VASTSelector *getFanout(unsigned Idx) const {
    return Fanouts[Idx];
  }

  VASTSelector *getFanin(unsigned Idx) const {
    return Fanins[Idx];
  }

  virtual void printDecl(raw_ostream &OS) const;

  virtual void print(vlang_raw_ostream &OS) const;
  void print(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSubModuleBase *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSubmodule ||
           A->getASTType() == vastMemoryBank ||
           A->getASTType() == vastCtrlRegion;
  }
};
} // end namespace

#endif
