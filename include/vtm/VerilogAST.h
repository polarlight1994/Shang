//===------------- VLang.h - Verilog HDL writing engine ---------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The VLang provide funtions to complete common Verilog HDL writing task.
//
//===----------------------------------------------------------------------===//
#ifndef VBE_VLANG_H
#define VBE_VLANG_H

#include "vtm/Utilities.h"
#include "vtm/VerilogBackendMCTargetDesc.h"
#include "vtm/FUInfo.h"
#include "vtm/LangSteam.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/Support/Allocator.h"

#include <map>

namespace llvm {
class MachineInstr;
class MachineBasicBlock;
class MachineOperand;
class VASTModule;
template<typename T> struct PtrInvPair;
class VASTUse;
class VASTValue;
class VASTImmediate;
class VASTNamedValue;
class VASTSlot;
class VASTSignal;
class VASTWire;
class VASTSeqValue;
class VASTExpr;
class VASTRegister;
class VASTUse;
class VASTExprBuilder;

class VASTNode {
public:
  // Leaf node type of Verilog AST.
  enum VASTTypes {
    vastImmediate,
    vastFirstValueType = vastImmediate,
    vastSymbol,
    vastExpr,
    vastWire,
    vastSeqValue,
    // CustomNode used by pre-scheduling data-path optimizer and the IR level
    // resource usage estimation pass.
    vastCustomNode,
    vastLastValueType = vastCustomNode,
    vastPort,
    vastSlot,
    vastRegister,
    vastBlockRAM,

    vastModule
  };
protected:
  union {
    const char *Name;
    VASTNamedValue *Value;
    MachineInstr *BundleStart;
  } Contents;

  const uint8_t NodeT;
  explicit VASTNode(VASTTypes T) : NodeT(T) {}

  virtual void print(raw_ostream &OS) const = 0;

  friend class DatapathContainer;

  // Drop this VASTNode from the userlist of all its uses.
  virtual void dropUses() { };
public:
  virtual ~VASTNode() {}

  VASTTypes getASTType() const { return VASTTypes(NodeT); }


  void dump() const;
};

template<typename T>
struct PtrInvPair : public PointerIntPair<T*, 1, bool> {
  typedef PointerIntPair<T*, 1, bool> Base;
  PtrInvPair(T *V = 0, bool IsInvert = false)
    : PointerIntPair<T*, 1, bool>(V, IsInvert) {}

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
    return get() ? this->getOpaqueValue() : 0;
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
  inline APInt getBitSlice(unsigned UB, unsigned LB = 0) const;
  inline bool isAllZeros() const;
  inline bool isAllOnes() const;

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

  // getAsInlineOperand, with the invert flag.
  inline PtrInvPair<VASTValue> getAsInlineOperand() const {
    // Get the underlying value, and invert the underlying value if necessary.
    return cast<PtrInvPair<VASTValue> >(get()->getAsInlineOperand(isInverted()));
  }

  inline void printAsOperand(raw_ostream &OS, unsigned UB, unsigned LB) const {
    get()->printAsOperand(OS, UB, LB, isInverted());
  }

  inline void printAsOperand(raw_ostream &OS) const {
    get()->printAsOperand(OS, isInverted());
  }

  template<typename T1>
  bool isa() const { return llvm::isa<T1>(get()); }
};

// Casting PtrInvPair.
template<class To, class From>
struct cast_retty_impl<PtrInvPair<To>, PtrInvPair<From> >{
  typedef PtrInvPair<To> ret_type;
};

template<class ToTy, class FromTy>
struct cast_convert_val<PtrInvPair<ToTy>, PtrInvPair<FromTy>, PtrInvPair<FromTy> >{
  typedef PtrInvPair<ToTy> To;
  typedef PtrInvPair<FromTy> From;
  static typename cast_retty<To, From>::ret_type doit(const From &Val) {
    return To(cast_convert_val<ToTy, FromTy*, FromTy*>::doit(Val.get()),
              Val.isInverted());
  }
};

template <typename To, typename From>
struct isa_impl<PtrInvPair<To>, PtrInvPair<From> > {
  static inline bool doit(const PtrInvPair<From> &Val) {
    return To::classof(Val.get());
  }
};

template<class To, class From>
struct cast_retty_impl<To, PtrInvPair<From> > : public cast_retty_impl<To, From*>
{};

template<class To, class FromTy> struct cast_convert_val<To,
                                                         PtrInvPair<FromTy>,
                                                         PtrInvPair<FromTy> > {
  typedef PtrInvPair<FromTy> From;
  static typename cast_retty<To, From>::ret_type doit(const From &Val) {
    return cast_convert_val<To, FromTy*, FromTy*>::doit(Val.get());
  }
};

template <typename To, typename From>
struct isa_impl<To, PtrInvPair<From> > {
  static inline bool doit(const PtrInvPair<From> &Val) {
    return !Val.isInverted() && To::classof(Val.get());
  }
};

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
  VASTNode &getUser() { return User; }
  const VASTNode &getUser() const { return User; }

  // Remove this use from use list.
  void unlinkUseFromUser();

  bool operator==(const VASTValPtr RHS) const;

  bool operator!=(const VASTValPtr RHS) const {
    return !operator==(RHS);
  }

  bool operator<(const VASTUse &RHS) const {
    return V < RHS.V;
  }

  // Return the underlying VASTValue.
  VASTValPtr get() const {
    assert(!isInvalid() && "Not a valid Use!");
    return V;
  }

  template<typename T>
  inline T *getAsLValue() const { return get().getAsLValue<T>(); }

  template<typename T>
  bool isa() const { return get().isa<T>(); }

  operator VASTValPtr() const { return get(); }

  VASTValPtr operator->() const { return get(); }
  inline VASTValPtr getAsInlineOperand() const {
    return get().getAsInlineOperand();
  }

  inline void printAsOperand(raw_ostream &OS, unsigned UB, unsigned LB) const {
    get().printAsOperand(OS, UB, LB);
  }

  inline void printAsOperand(raw_ostream &OS) const {
    get().printAsOperand(OS);
  }

  VASTValPtr unwrap() const { return V; }

  // Prevent the user from being removed.
  void PinUser() const;
};

template<>
struct ilist_traits<VASTUse> : public ilist_default_traits<VASTUse> {
  static VASTUse *createSentinel() { return new VASTUse(0, 0); }

  static void deleteNode(VASTUse *U) {}

  static bool inAnyList(const VASTUse *U) {
    return U->getPrev() != 0 || U->getNext() != 0;
  }
};

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

class VASTValue : public VASTNode {
  typedef iplist<VASTUse> UseListTy;
  UseListTy UseList;
protected:

  VASTValue(VASTTypes T, unsigned BitWidth) : VASTNode(T), BitWidth(BitWidth) {
    assert(T >= vastFirstValueType && T <= vastLastValueType
           && "Bad DeclType!");
  }

  void addUseToList(VASTUse *U) { UseList.push_back(U); }
  void removeUseFromList(VASTUse *U) { UseList.remove(U); }

  friend class VASTUse;

  virtual void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const;

  virtual void printAsOperandImpl(raw_ostream &OS) const {
    printAsOperandImpl(OS, getBitWidth(), 0);
  }

  // Print the value as inline operand.
  virtual VASTValPtr getAsInlineOperandImpl() { return this; }
public:
  const uint8_t BitWidth;
  unsigned getBitWidth() const { return BitWidth; }

  typedef VASTUseIterator<UseListTy::iterator, VASTNode> use_iterator;
  use_iterator use_begin() { return use_iterator(UseList.begin()); }
  use_iterator use_end() { return use_iterator(UseList.end()); }

  bool use_empty() const { return UseList.empty(); }
  size_t num_uses() const { return UseList.size(); }

  void printAsOperand(raw_ostream &OS, unsigned UB, unsigned LB,
                      bool isInverted) const;
  void printAsOperand(raw_ostream &OS, bool isInverted) const;

  VASTValPtr getAsInlineOperand(bool isInverted) {
    return getAsInlineOperandImpl().invert(isInverted);
  }

  virtual void print(raw_ostream &OS) const;

  typedef const VASTUse *dp_dep_it;
  static dp_dep_it dp_dep_begin(const VASTValue *V);
  static dp_dep_it dp_dep_end(const VASTValue *V);

  static bool is_dp_leaf(const VASTValue *V) {
    return dp_dep_begin(V) == dp_dep_end(V);
  }

  // Helper function.
  static std::string printBitRange(unsigned UB, unsigned LB, bool printOneBit);

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() >= vastFirstValueType &&
           A->getASTType() <= vastLastValueType;
  }
};

// simplify_type - Allow clients to treat VASTRValue just like VASTValues when
// using casting operators.
template<> struct simplify_type<const VASTUse> {
  typedef VASTValPtr SimpleType;
  static SimpleType getSimplifiedValue(const VASTUse &Val) {
    return Val.unwrap();
  }
};

template<> struct simplify_type<VASTUse> {
  typedef VASTValPtr SimpleType;
  static SimpleType getSimplifiedValue(const VASTUse &Val) {
    return Val.unwrap();
  }
};

class VASTImmediate : public VASTValue, public FoldingSetNode  {
  const APInt Int;

  VASTImmediate(const APInt &Other)
    : VASTValue(vastImmediate, Other.getBitWidth()), Int(Other) {}

  VASTImmediate(const VASTImmediate&);              // Do not implement
  void operator=(const VASTImmediate&);             // Do not implement

  void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const;

  void printAsOperandImpl(raw_ostream &OS) const {
    printAsOperandImpl(OS, getBitWidth(), 0);
  }

  friend class DatapathContainer;
public:
  /// Profile - Used to insert VASTImm objects, or objects that contain VASTImm
  ///  objects, into FoldingSets.
  void Profile(FoldingSetNodeID& ID) const;

  const APInt &getAPInt() const { return Int; }
  uint64_t getZExtValue() const { return Int.getZExtValue(); }

  APInt getBitSlice(unsigned UB, unsigned LB = 0) const {
    return getBitSlice(Int, UB, LB);
  }

  bool isAllZeros() const {
    return Int.isMinValue();
  }

  bool isAllOnes() const {
    return Int.isAllOnesValue();
  }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTImmediate *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastImmediate;
  }

  // Helper functions to manipulate APInt at bit level.
  static APInt getBitSlice(const APInt &Int, unsigned UB, unsigned LB = 0) {
    return Int.lshr(LB).sextOrTrunc(UB - LB);
  }
};

typedef PtrInvPair<VASTImmediate> VASTImmPtr;
template<>
inline APInt PtrInvPair<VASTImmediate>::getAPInt() const {
  APInt Val = get()->getAPInt();
  if (isInverted()) Val.flipAllBits();
  return Val;
}

typedef PtrInvPair<VASTImmediate> VASTImmPtr;
template<>
inline
APInt PtrInvPair<VASTImmediate>::getBitSlice(unsigned UB, unsigned LB) const {
  return VASTImmediate::getBitSlice(getAPInt(), UB, LB);
}
template<>
inline bool PtrInvPair<VASTImmediate>::isAllZeros() const {
  return isInverted() ? get()->isAllOnes() : get()->isAllZeros();
}
template<>
inline bool PtrInvPair<VASTImmediate>::isAllOnes() const {
  return isInverted() ? get()->isAllZeros() : get()->isAllOnes();
}

class VASTNamedValue : public VASTValue {
protected:
  VASTNamedValue(VASTTypes T, const char *Name, unsigned BitWidth)
    : VASTValue(T, BitWidth) {
    assert((T == vastSymbol || T == vastWire || T == vastSeqValue
            || T == vastCustomNode)
           && "Bad DeclType!");
    Contents.Name = Name;
  }

  virtual void printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                  unsigned LB) const;
  void printAsOperandImpl(raw_ostream &OS) const {
    printAsOperandImpl(OS, getBitWidth(), 0);
  }
public:
  const char *getName() const { return Contents.Name; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTNamedValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSymbol ||
           A->getASTType() == vastWire ||
           A->getASTType() == vastSeqValue ||
           A->getASTType() == vastCustomNode;
  }
};

class VASTSymbol : public VASTNamedValue {
  VASTSymbol(const char *Name, unsigned BitWidth)
    : VASTNamedValue(VASTNode::vastSymbol, Name, BitWidth) {}

  friend class VASTModule;
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSymbol *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSymbol;
  }
};

class VASTSignal : public VASTNamedValue {
protected:
  VASTSignal(VASTTypes DeclType, const char *Name, unsigned BitWidth)
    : VASTNamedValue(DeclType, Name, BitWidth) {}
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSignal *A) { return true; }
  static inline bool classof(const VASTSeqValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastWire || A->getASTType() == vastSeqValue;
  }
};

class VASTExpr : public VASTValue, public FoldingSetNode {
public:
  enum Opcode {
    // bitwise logic datapath
    dpAnd,
    dpRAnd,
    dpRXor,
    dpSel,
    // bit level assignment.
    dpBitCat,
    dpBitRepeat,
    // Simple wire assignment.
    dpAssign,
    LastInlinableOpc = dpAssign,
    // Cannot inline.
    // FU datapath
    dpAdd,
    FirstFUOpc = dpAdd,
    dpMul,
    dpShl,
    dpSRA,
    dpSRL,
    dpSGT,
    dpSGE,
    dpUGT,
    dpUGE,
    LastFUOpc = dpUGE,
    // Lookup-tables.
    dpLUT,
    // Mux in datapath.
    dpMux,
    // Read/Write block RAM.
    dpRdBRAM, // Represented by the Address.
    dpWrBRAM, // Represented by tuple (Address, Data).
    // Blackbox,
    dpBlackBox
  };
private:
  // Operands, right after this VASTExpr.
  const VASTUse *ops() const {
    return reinterpret_cast<const VASTUse*>(this + 1);
  }
  VASTUse *ops() { return reinterpret_cast<VASTUse*>(this + 1); }

  // The total operand of this expression.
  unsigned ExprSize : 31;
  bool     IsNamed    : 1;

  VASTExpr(const VASTExpr&);              // Do not implement
  void operator=(const VASTExpr&);        // Do not implement

  VASTExpr(Opcode Opc, uint8_t numOps, unsigned UB, unsigned LB);

  friend class DatapathContainer;

  void printAsOperandInteral(raw_ostream &OS) const;

  void dropUses() {
    for (VASTUse *I = ops(), *E = ops() + NumOps; I != E; ++I)
      I->unlinkUseFromUser();
  }

  void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const;

  void printAsOperandImpl(raw_ostream &OS) const {
    printAsOperandImpl(OS, UB, LB);
  }

  VASTValPtr getAsInlineOperandImpl() {
    // Can the expression be printed inline?
    if (getOpcode() == VASTExpr::dpAssign && !isSubBitSlice())
      return getOperand(0).getAsInlineOperand();

    return this;
  }

  const char *getLUT() const;
public:
  const uint8_t Opc, NumOps,UB, LB;
  Opcode getOpcode() const { return VASTExpr::Opcode(Opc); }
  const char *getFUName() const;
  const std::string getSubModName() const;

  const VASTUse &getOperand(unsigned Idx) const {
    assert(Idx < NumOps && "Index out of range!");
    return ops()[Idx];
  }

  typedef const VASTUse *op_iterator;
  op_iterator op_begin() const { return ops(); }
  op_iterator op_end() const { return ops() + NumOps; }

  //typedef VASTUse *op_iterator;
  //op_iterator op_begin() const { return ops(); }
  //op_iterator op_end() const { return ops() + num_ops(); }

  ArrayRef<VASTUse> getOperands() const {
    return ArrayRef<VASTUse>(ops(), NumOps);
  }

  inline bool isSubBitSlice() const {
    return getOpcode() == dpAssign
           && (UB != getOperand(0)->getBitWidth() || LB != 0);
  }

  inline bool isZeroBasedBitSlice() const {
    return isSubBitSlice() && LB == 0;
  }

  bool hasName() const { return IsNamed != 0; }

  // Assign a name to this expression.
  void nameExpr() {
    assert(!hasName() && "Expr already have name!");
    IsNamed = true;
  }

  void unnameExpr() {
    assert(hasName() && "Expr already have name!");
    IsNamed = false;
  }

  std::string getTempName() const;

  bool isInlinable() const;

  void print(raw_ostream &OS) const { printAsOperandInteral(OS); }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTExpr *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastExpr;
  }

  /// Profile - Used to insert VASTExpr objects, or objects that contain
  /// VASTExpr objects, into FoldingSets.
  void Profile(FoldingSetNodeID& ID) const;
};

typedef PtrInvPair<VASTExpr> VASTExprPtr;
template<>
inline VASTValPtr PtrInvPair<VASTExpr>::getOperand(unsigned i) const {
  return get()->getOperand(i).get().invert(isInverted());
}

class VASTWire :public VASTSignal {
public:
  enum Type {
    Common,
    // Timing BlackBox, have latecy not capture by slots.
    haveExtraDelay,
    // Assignment with slot information.
    AssignCond
  };
private:
  unsigned   T : 2;
  unsigned Idx : 29;
  bool IsPinned : 1;
public:
  const char *const AttrStr;

  VASTWire(const char *Name, unsigned BitWidth, const char *Attr = "",
           bool IsPinned = false)
    : VASTSignal(vastWire, Name, BitWidth), U(this, 0), T(Common), Idx(0),
      IsPinned(IsPinned), AttrStr(Attr) {}

  void assign(VASTValPtr V, VASTWire::Type T = VASTWire::Common) {
    this->T = T;
    U.set(V);
  }

  bool isPinned() const { return IsPinned; }
  void Pin(bool isPinned = true ) { IsPinned = isPinned; }
private:
  friend class VASTModule;

  // VASTValue pointer point to the VASTExpr.
  VASTUse U;

  VASTWire(unsigned SlotNum, MachineInstr *DefMI)
    : VASTSignal(vastWire, 0, 1), U(this, 0), T(AssignCond),
      Idx(SlotNum), IsPinned(false), AttrStr("")
  {
    Contents.BundleStart = DefMI;
  } 

  void assignWithExtraDelay(VASTValPtr V, unsigned latency) {
    assign(V, haveExtraDelay);
    Idx = latency;
  }

  void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const;

  VASTValPtr getAsInlineOperandImpl() {
    if (VASTValPtr V = getDriver()) {
      // Can the expression be printed inline?
      if (VASTExprPtr E = dyn_cast<VASTExprPtr>(V)) {
        if (E->isInlinable()) return E.getAsInlineOperand();
      } else if (V->getBitWidth()) // The wire may wrapping a symbol.
        // This is a simple assignment.
        return V;
    }

    return this;
  }

  void dropUses() { if (U.get()) U.unlinkUseFromUser(); }
public:
  VASTValPtr getDriver() const { return U.unwrap(); }

  VASTExprPtr getExpr() const {
    return getDriver() ? dyn_cast<VASTExprPtr>(getDriver()) : 0;
  }

  VASTWire::Type getWireType() const { return Type(T); }

  unsigned getExtraDelayIfAny() const {
    return getWireType() == VASTWire::haveExtraDelay ? Idx : 0;
  }

  uint16_t getSlotNum() const {
    assert(getWireType() == VASTWire::AssignCond &&
           "Call getSlot on bad wire type!");
    return Idx;
  }

  MachineInstr *getDefMI() const {
    assert(getWireType() == VASTWire::AssignCond &&
           "Call getDefMI on bad wire type!");
    return Contents.BundleStart;
  }

  // Print the logic to the output stream.
  void printAssignment(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTWire *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastWire;
  }

  // Internal function used by VASTValue.
  VASTValue::dp_dep_it op_begin() const {
    return (U.isInvalid()) ? 0 : &U;
  }
  VASTValue::dp_dep_it op_end() const {
    return (U.isInvalid()) ? 0 : &U + 1;
  }
};

typedef PtrInvPair<VASTWire> VASTWirePtr;

template<>
inline VASTExprPtr PtrInvPair<VASTWire>::getExpr() const {
  return get()->getExpr().invert(isInverted());
}

struct VASTWireExpressionTrait : public DenseMapInfo<VASTWire*> {
  static unsigned getHashValue(const VASTWire *Val) {
    if (Val == 0) return DenseMapInfo<void*>::getHashValue(0);

    if (VASTValPtr Ptr = Val->getDriver())
      return DenseMapInfo<void*>::getHashValue(Ptr.getOpaqueValue());

    return DenseMapInfo<void*>::getHashValue(Val);
  }

  static const PtrInvPair<const VASTValue> getAssigningValue(const VASTWire *W) {
    if (W == getEmptyKey() || W == getTombstoneKey() || W == 0)
      return 0;

    if (const VASTValPtr V = W->getDriver()) return V;

    return W;
  }

  static bool isEqual(const VASTWire *LHS, const VASTWire *RHS) {
    return LHS == RHS || getAssigningValue(LHS) == getAssigningValue(RHS);
  }
};

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

  enum Type {
    Data,       // Common registers which hold data for data-path.
    Slot,       // Slot register which hold the enable signals for each slot.
    IO,         // The I/O port of the module.
    BRAM,       // Port of the block RAM
  };

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
  VASTSeqValue(const char *Name, unsigned Bitwidth, Type T, unsigned Idx,
               VASTNode &Parent)
    : VASTSignal(vastSeqValue, Name, Bitwidth), T(T), Idx(Idx),
      Parent(Parent) {}

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSeqValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSeqValue;
  }

  VASTSeqValue::Type getValType() const { return Type(T); }

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
  bool isTimingUndef() const { return getValType() == VASTSeqValue::Slot; }

  typedef AssignMapTy::const_iterator assign_itertor;
  assign_itertor begin() const { return Assigns.begin(); }
  assign_itertor end() const { return Assigns.end(); }
  unsigned size() const { return Assigns.size(); }
  bool empty() const { return Assigns.empty(); }

  void verifyAssignCnd(vlang_raw_ostream &OS, const Twine &Name,
                       const VASTModule *Mod) const;

  void buildCSEMap(std::map<VASTValPtr, std::vector<VASTValPtr> > &CSEMap) const;
};

class VASTBlockRAM : public VASTNode {
  unsigned Depth;
  unsigned WordSize;
  unsigned BRamNum;
public:
  VASTSeqValue WritePortA;
private:
  void printSelector(raw_ostream &OS, const VASTSeqValue &Port) const;
  void printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod,
                       const VASTSeqValue &Port) const;

  VASTBlockRAM(const char *Name, unsigned BRamNum, unsigned WordSize,
               unsigned Depth)
    : VASTNode(vastBlockRAM), Depth(Depth), WordSize(WordSize),
      BRamNum(BRamNum),
      WritePortA(Name, WordSize, VASTSeqValue::BRAM, BRamNum, *this)
  {}

  friend class VASTModule;
public:
  typedef VASTSeqValue::assign_itertor assign_itertor;
  unsigned getBlockRAMNum() const { return BRamNum; }

  unsigned getWordSize() const { return WordSize; }
  unsigned getDepth() const { return Depth; }

  void printSelector(raw_ostream &OS) const {
    printSelector(OS, WritePortA);
  }

  void printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod) const {
    printAssignment(OS, Mod, WritePortA);
  }

  void print(raw_ostream &OS) const {}

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTBlockRAM *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastBlockRAM;
  }

};

class VASTRegister : public VASTNode {
  VASTSeqValue Value;
  uint64_t InitVal;

  VASTRegister(const char *Name, unsigned BitWidth, uint64_t InitVal,
               VASTSeqValue::Type T = VASTSeqValue::Data, unsigned RegData = 0,
               const char *Attr = "");
  friend class VASTModule;

  void dropUses() { 
    assert(0 && "Function not implemented!");
  }

public:
  const char *const AttrStr;

  VASTSeqValue *getValue() { return &Value; }
  VASTSeqValue *operator->() { return getValue(); }

  const char *getName() const { return Value.getName(); }
  unsigned getBitWidth() const { return Value.getBitWidth(); }

  typedef VASTSeqValue::assign_itertor assign_itertor;
  assign_itertor assign_begin() const { return Value.begin(); }
  assign_itertor assign_end() const { return Value.end(); }
  unsigned num_assigns() const { return Value.size(); }

  void printSelector(raw_ostream &OS) const;

  // Print data transfer between registers.
  void printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod) const;
  // Return true if the reset is actually printed.
  bool printReset(raw_ostream &OS) const;
  void dumpAssignment() const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTRegister *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastRegister;
  }

  typedef VASTSeqValue::AndCndVec AndCndVec;
  static void printCondition(raw_ostream &OS, const VASTSlot *Slot,
                             const AndCndVec &Cnds);

  void print(raw_ostream &OS) const {}
};

class VASTPort : public VASTNode {

public:
  const bool IsInput;

  VASTPort(VASTNamedValue *V, bool isInput);

  VASTNamedValue *getValue() const { return Contents.Value; }
  VASTSeqValue *getSeqVal() const { return cast<VASTSeqValue>(getValue()); }

  const char *getName() const { return getValue()->getName(); }
  bool isInput() const { return IsInput; }
  bool isRegister() const { return !isInput() && !isa<VASTWire>(getValue()); }
  unsigned getBitWidth() const { return getValue()->getBitWidth(); }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTPort *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastPort;
  }

  void print(raw_ostream &OS) const;
  void printExternalDriver(raw_ostream &OS, uint64_t InitVal = 0) const;
  std::string getExternalDriverStr(unsigned InitVal = 0) const;
};

// The container to hold all VASTExprs in data-path of the design.
class DatapathContainer {
  // The unique immediate in the data-path.
  FoldingSet<VASTImmediate> UniqueImms;

  // Expression in data-path
  FoldingSet<VASTExpr> UniqueExprs;

protected:
  BumpPtrAllocator Allocator;

  void removeValueFromCSEMaps(VASTNode *N);
  void addModifiedValueToCSEMaps(VASTNode *N);
  template<typename T>
  void addModifiedValueToCSEMaps(T *V, FoldingSet<T> &CSEMap);

public:
  BumpPtrAllocator *getAllocator() { return &Allocator; }

  VASTValPtr createExprImpl(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned UB, unsigned LB);

  virtual void replaceAllUseWithImpl(VASTValPtr From, VASTValPtr To);

  VASTImmediate *getOrCreateImmediateImpl(const APInt &Value);

  VASTImmediate *getOrCreateImmediateImpl(uint64_t Value, int8_t BitWidth) {
    return getOrCreateImmediateImpl(APInt(BitWidth, Value));
  }

  VASTImmediate *getBoolImmediateImpl(bool Value) {
    return getOrCreateImmediateImpl(Value ? 1 : 0, 1);
  }

  void reset();
};

// The class that represent Verilog modulo.
class VASTModule : public VASTNode, public DatapathContainer {
public:
  typedef SmallVector<VASTPort*, 16> PortVector;
  typedef PortVector::iterator port_iterator;
  typedef PortVector::const_iterator const_port_iterator;

  typedef SmallVector<VASTWire*, 128> WireVector;
  typedef WireVector::iterator wire_iterator;

  typedef SmallVector<VASTRegister*, 128> RegisterVector;
  typedef RegisterVector::iterator reg_iterator;

  typedef SmallVector<VASTBlockRAM*, 16> BlockRAMVector;
  typedef BlockRAMVector::iterator bram_iterator;

  typedef SmallVector<VASTSeqValue*, 128> SeqValueVector;
  typedef SeqValueVector::iterator seqval_iterator;

  typedef std::vector<VASTSlot*> SlotVecTy;
  typedef SlotVecTy::iterator slot_iterator;
private:
  // Dirty Hack:
  // Buffers
  raw_string_ostream DataPath, ControlBlock;
  vlang_raw_ostream LangControlBlock;
  // The slots vector, each slot represent a state in the FSM of the design.
  SlotVecTy Slots;
  SeqValueVector SeqVals;

  // Input/Output ports of the design.
  PortVector Ports;
  // Wires and Registers of the design.
  WireVector Wires;
  RegisterVector Registers;
  BlockRAMVector BlockRAMs;

  typedef StringMap<VASTNamedValue*> SymTabTy;
  SymTabTy SymbolTable;
  typedef StringMapEntry<VASTNamedValue*> SymEntTy;

  // The Name of the Design.
  std::string Name;
  VASTExprBuilder *Builder;

  // The port starting offset of a specific function unit.
  SmallVector<std::map<unsigned, unsigned>, VFUs::NumCommonFUs> FUPortOffsets;
  unsigned NumArgPorts, RetPortIdx;

  VASTPort *addPort(const std::string &Name, unsigned BitWidth, bool isReg,
                    bool isInput);
public:
  static std::string DirectClkEnAttr, ParallelCaseAttr, FullCaseAttr;

  enum PortTypes {
    Clk = 0,
    RST,
    Start,
    SpecialInPortEnd,
    Finish = SpecialInPortEnd,
    SpecialOutPortEnd,
    NumSpecialPort = SpecialOutPortEnd,
    ArgPort, // Ports for function arguments.
    Others,   // Likely ports for function unit.
    RetPort // Port for function return value.
  };

  VASTModule(const std::string &Name, VASTExprBuilder *Builder)
    : VASTNode(vastModule),
    DataPath(*(new std::string())),
    ControlBlock(*(new std::string())),
    LangControlBlock(ControlBlock),
    Name(Name), Builder(Builder),
    FUPortOffsets(VFUs::NumCommonFUs),
    NumArgPorts(0) {
    Ports.append(NumSpecialPort, 0);
  }

  ~VASTModule();

  void setBuilder(VASTExprBuilder *Builder) {
    this->Builder = Builder;
  }

  void reset();

  const std::string &getName() const { return Name; }

  void printDatapath(raw_ostream &OS) const;
  void printRegisterBlocks(vlang_raw_ostream &OS) const;
  void printBlockRAMBlocks(vlang_raw_ostream &OS) const;

  // Print the slot control flow.
  void buildSlotLogic(VASTExprBuilder &Builder);
  void writeProfileCounters(VASTSlot *S, bool isFirstSlot);

  VASTValue *getSymbol(const std::string &Name) const {
    SymTabTy::const_iterator at = SymbolTable.find(Name);
    assert(at != SymbolTable.end() && "Symbol not found!");
    return at->second;
  }

  VASTValue *lookupSymbol(const std::string &Name) const {
    SymTabTy::const_iterator at = SymbolTable.find(Name);
    if (at == SymbolTable.end()) return 0;

    return at->second;
  }

  template<class T>
  T *lookupSymbol(const std::string &Name) const {
    return cast_or_null<T>(lookupSymbol(Name));
  }

  template<class T>
  T *getSymbol(const std::string &Name) const {
    return cast<T>(getSymbol(Name));
  }

  // Create wrapper to allow us get a bitslice of the symbol.
  VASTValPtr getOrCreateSymbol(const std::string &Name, unsigned BitWidth,
                               bool CreateWrapper);

  void allocaSlots(unsigned TotalSlots) {
    Slots.assign(TotalSlots, 0);
  }

  virtual void *Allocate(size_t Num, size_t Alignment){
    return Allocator.Allocate(Num, Alignment);
  }

  VASTSlot *getOrCreateSlot(unsigned SlotNum, MachineInstr *BundleStart);

  VASTSlot *getSlot(unsigned SlotNum) const {
    VASTSlot *S = Slots[SlotNum];
    assert(S && "Slot not exist!");
    return S;
  }

  VASTUse *allocateUse() { return Allocator.Allocate<VASTUse>(); }
  // Allow user to add ports.
  VASTPort *addInputPort(const std::string &Name, unsigned BitWidth,
                         PortTypes T = Others);

  VASTPort *addOutputPort(const std::string &Name, unsigned BitWidth,
                          PortTypes T = Others, bool isReg = true);

  void setFUPortBegin(FuncUnitId ID) {
    unsigned offset = Ports.size();
    std::pair<unsigned, unsigned> mapping
      = std::make_pair(ID.getFUNum(), offset);
    std::map<unsigned, unsigned> &Map = FUPortOffsets[ID.getFUType()];
    assert(!Map.count(mapping.first) && "Port begin mapping existed!");
    FUPortOffsets[ID.getFUType()].insert(mapping);
  }

  unsigned getFUPortOf(FuncUnitId ID) const {
    typedef std::map<unsigned, unsigned> MapTy;
    const MapTy &Map = FUPortOffsets[ID.getFUType()];
    MapTy::const_iterator at = Map.find(ID.getFUNum());
    assert(at != Map.end() && "FU do not existed!");
    return at->second;
  }

  const_port_iterator getFUPortItBegin(FuncUnitId ID) const {
    unsigned PortBegin = getFUPortOf(ID);
    return Ports.begin() + PortBegin;
  }

  void printModuleDecl(raw_ostream &OS) const;

  // Get all ports of this moudle.
  const PortVector &getPorts() const { return Ports; }
  unsigned getNumPorts() const { return Ports.size(); }

  VASTPort &getPort(unsigned i) const {
    // FIXME: Check if out of range.
    return *Ports[i];
  }

  const char *getPortName(unsigned i) const {
    return getPort(i).getName();
  }

  port_iterator ports_begin() { return Ports.begin(); }
  const_port_iterator ports_begin() const { return Ports.begin(); }

  port_iterator ports_end() { return Ports.end(); }
  const_port_iterator ports_end() const { return Ports.end(); }

  // Argument ports and return port.
  const VASTPort &getArgPort(unsigned i) const {
    // FIXME: Check if out of range.
    return getPort(i + VASTModule::SpecialOutPortEnd);
  }

  unsigned getNumArgPorts() const { return NumArgPorts; }
  unsigned getRetPortIdx() const { return RetPortIdx; }
  VASTPort &getRetPort() const {
    assert(getRetPortIdx() && "No return port in this module!");
    return getPort(getRetPortIdx());
  }

  unsigned getNumCommonPorts() const {
    return getNumPorts() - VASTModule::SpecialOutPortEnd;
  }

  const VASTPort &getCommonPort(unsigned i) const {
    // FIXME: Check if out of range.
    return getPort(i + VASTModule::SpecialOutPortEnd);
  }

  port_iterator common_ports_begin() {
    return Ports.begin() + VASTModule::SpecialOutPortEnd;
  }
  const_port_iterator common_ports_begin() const {
    return Ports.begin() + VASTModule::SpecialOutPortEnd;
  }

  VASTBlockRAM *addBlockRAM(unsigned BRamNum, unsigned Bitwidth, unsigned Size);

  VASTRegister *addRegister(const std::string &Name, unsigned BitWidth,
                            unsigned InitVal = 0,
                            VASTSeqValue::Type T = VASTSeqValue::Data,
                            uint16_t RegData = 0, const char *Attr = "");

  VASTRegister *addOpRegister(const std::string &Name, unsigned BitWidth,
                              unsigned FUNum, const char *Attr = "") {
    return addRegister(Name, BitWidth, 0, VASTSeqValue::Data, FUNum, Attr);
  }

  VASTRegister *addDataRegister(const std::string &Name, unsigned BitWidth,
                                unsigned RegNum = 0, const char *Attr = "") {
    return addRegister(Name, BitWidth, 0, VASTSeqValue::Data, RegNum, Attr);
  }

  VASTWire *addWire(const std::string &Name, unsigned BitWidth,
                    const char *Attr = "", bool IsPinned = false);

  reg_iterator reg_begin() { return Registers.begin(); }
  reg_iterator reg_end() { return Registers.end(); }

  seqval_iterator seqval_begin()  { return SeqVals.begin(); }
  seqval_iterator seqval_end()    { return SeqVals.end(); }

  slot_iterator slot_begin() { return Slots.begin(); }
  slot_iterator slot_end() { return Slots.end(); }

  VASTWire *createAssignPred(VASTSlot *Slot, MachineInstr *DefMI);
 
  void addAssignment(VASTSeqValue *V, VASTValPtr Src, VASTSlot *Slot,
                     SmallVectorImpl<VASTValPtr> &Cnds, MachineInstr *DefMI = 0,
                     bool AddSlotActive = true);

  VASTWire *addPredExpr(VASTWire *CndWire, SmallVectorImpl<VASTValPtr> &Cnds,
                        bool AddSlotActive = true);

  VASTWire *assign(VASTWire *W, VASTValPtr V, VASTWire::Type T = VASTWire::Common);
  VASTWire *assignWithExtraDelay(VASTWire *W, VASTValPtr V, unsigned latency);

  void printSignalDecl(raw_ostream &OS);

  void print(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTModule *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastModule;
  }

  vlang_raw_ostream &getControlBlockBuffer() {
    return LangControlBlock;
  }

  std::string &getControlBlockStr() {
    LangControlBlock.flush();
    return ControlBlock.str();
  }

  raw_ostream &getDataPathBuffer() {
    return DataPath;
  }

  std::string &getDataPathStr() {
    return DataPath.str();
  }

  // Out of line virtual function to provide home for the class.
  virtual void anchor();

  static const std::string GetMemBusEnableName(unsigned FUNum) {
    return VFUMemBus::getEnableName(FUNum) + "_r";
  }

  static const std::string GetFinPortName() {
    return "fin";
  }
};

// Helper functions
// Traverse the use tree to get the registers.
template<typename VisitPathFunc>
void DepthFirstTraverseDepTree(VASTValue *DepTree, VisitPathFunc VisitPath) {
  typedef VASTValue::dp_dep_it ChildIt;
  // Use seperate node and iterator stack, so we can get the path vector.
  typedef SmallVector<VASTValue*, 16> NodeStackTy;
  typedef SmallVector<ChildIt, 16> ItStackTy;
  NodeStackTy NodeWorkStack;
  ItStackTy ItWorkStack;
  // Remember what we had visited.
  std::set<VASTValue*> VisitedUses;

  // Put the root.
  NodeWorkStack.push_back(DepTree);
  ItWorkStack.push_back(VASTValue::dp_dep_begin(DepTree));

  while (!ItWorkStack.empty()) {
    VASTValue *Node = NodeWorkStack.back();

    ChildIt It = ItWorkStack.back();

    // Do we reach the leaf?
    if (VASTValue::is_dp_leaf(Node)) {
      VisitPath(NodeWorkStack);
      NodeWorkStack.pop_back();
      ItWorkStack.pop_back();
      continue;
    }

    // All sources of this node is visited.
    if (It == VASTValue::dp_dep_end(Node)) {
      NodeWorkStack.pop_back();
      ItWorkStack.pop_back();
      continue;
    }

    // Depth first traverse the child of current node.
    VASTValue *ChildNode = (*It).get().get();
    ++ItWorkStack.back();

    // Had we visited this node? If the Use slots are same, the same subtree
    // will lead to a same slack, and we do not need to compute the slack agian.
    if (!VisitedUses.insert(ChildNode).second) continue;

    // If ChildNode is not visit, go on visit it and its childrens.
    NodeWorkStack.push_back(ChildNode);
    ItWorkStack.push_back(VASTValue::dp_dep_begin(ChildNode));
  }
}
} // end namespace

#endif
