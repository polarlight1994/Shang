//===----- VASTNodeBases.h - Base Classes in VerilogAST ---------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the base classes in the Verilog Abstract Syntax Tree.
//
//===----------------------------------------------------------------------===//
#ifndef SHANG_VAST_NODE_BASE_H
#define SHANG_VAST_NODE_BASE_H

#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Casting.h"

#include <set>
#include <vector>

namespace llvm {
class BasicBlock;
class Value;
class VASTNamedValue;
class VASTValue;
class VASTExpr;
class VASTSeqValue;
class VASTSymbol;
class VASTRegister;
class VASTSeqOp;
class VASTModule;
class vlang_raw_ostream;
template<typename T> class ArrayRef;

class VASTNode {
public:
  // Leaf node type of Verilog AST.
  enum VASTTypes {
    vastImmediate,
    vastFirstValueType = vastImmediate,
    vastSymbol,
    vastLLVMValue,
    vastExpr,
    vastWire,
    vastSeqValue,

    vastLastValueType = vastSeqValue,
    vastPort,
    vastSlot,
    vastRegister,
    vastBlockRAM,
    vastSubmodule,
    vastMemoryBus,

    // Fine-grain control flow.
    vastSeqInst,
    vastSeqCtrlOp,
    vastSlotCtrl,
    vastSeqCode,

    // Handle of the VASTValPtr, make sure the replacement in the datapath do
    // not invalid the external use.
    vastHandle,

    vastModule
  };

protected:
  union {
    const char *Name;
    VASTNamedValue *NamedValue;
    BasicBlock *ParentBB;
    Value *LLVMValue;
  } Contents;

  const uint8_t NodeT : 7;
  bool IsDead         : 1;
  explicit VASTNode(VASTTypes T) : NodeT(T), IsDead(false) {}

  virtual void print(raw_ostream &OS) const = 0;

  friend class DatapathContainer;

  // Drop this VASTNode from the userlist of all its uses.
  virtual void dropUses();
  void setDead() { IsDead = true; }
public:
  virtual ~VASTNode() {}

  VASTTypes getASTType() const { return VASTTypes(NodeT); }
  bool isDead() const { return IsDead; }

  void dump() const;

  static std::string DirectClkEnAttr, ParallelCaseAttr, FullCaseAttr;
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

  // getAsInlineOperand, with the invert flag.
  inline PtrInvPair<VASTValue> getAsInlineOperand() const {
    // Get the underlying value, and invert the underlying value if necessary.
    return get()->getAsInlineOperand(isInverted());
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
    if (LHS->getASTType() < RHS->getASTType()) return true;
    else if (LHS->getASTType() > RHS->getASTType()) return false;

    return LHS.getOpaqueValue() < RHS.getOpaqueValue();
  }
private:
  // Hide the confusing getInt function.
  bool getInt() const { return Base::getInt(); }
};

template<typename T>
inline raw_ostream &operator<<(raw_ostream &OS, PtrInvPair<T> V) {
  V.printAsOperand(OS);
  return OS;
}

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

  void replaceUseByOrKeep(VASTValPtr RHS) {
    if (V == RHS) return;

    replaceUseBy(RHS);
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

  bool operator>(const VASTUse &RHS) const {
    return V > RHS.V;
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

  bool isInverted() const { return get().isInverted(); }
  VASTValPtr invert(bool Invert = true) const { return get().invert(Invert); }

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
  // FIXME: This sentinel is created and never released.
  static VASTUse *createSentinel() { return new VASTUse(0, 0); }

  static void deleteNode(VASTUse *U) {}
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

class VASTOperandList {
  friend class VASTModule;
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

  // Convert the VASTNode to VASTOperandList.
  static VASTOperandList *GetDatapathOperandList(VASTNode *N);
  static VASTOperandList *GetOperandList(VASTNode *N);

  template<typename T>
  static void visitTopOrder(VASTValue *Root, std::set<VASTOperandList*> &Visited, T F);
};

class VASTValue : public VASTNode {
  typedef iplist<VASTUse> UseListTy;
  UseListTy *UseList;
protected:

  VASTValue(VASTTypes T, unsigned BitWidth);

  void addUseToList(VASTUse *U) { UseList->push_back(U); }
  void removeUseFromList(VASTUse *U) { UseList->remove(U); }

  friend class VASTUse;

  virtual void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const;

  virtual void printAsOperandImpl(raw_ostream &OS) const {
    printAsOperandImpl(OS, getBitWidth(), 0);
  }

  // Print the value as inline operand.
  virtual VASTValPtr getAsInlineOperandImpl() { return this; }
public:
  virtual ~VASTValue();
  const uint8_t BitWidth;
  unsigned getBitWidth() const { return BitWidth; }

  typedef VASTUseIterator<UseListTy::iterator, VASTNode> use_iterator;
  use_iterator use_begin() { return use_iterator(UseList->begin()); }
  use_iterator use_end() { return use_iterator(UseList->end()); }

  bool use_empty() const { return UseList->empty(); }
  size_t num_uses() const { return UseList->size(); }

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

  // Extract all SeqVals which are connect to this VASTValue through data-path.
  void extractSupporingSeqVal(std::set<VASTSeqValue*> &SeqVals);

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

template<typename T>
void VASTOperandList::visitTopOrder(VASTValue *Root,
                                    std::set<VASTOperandList*> &Visited, T F) {
  VASTOperandList *L = VASTOperandList::GetDatapathOperandList(Root);
  // The entire tree had been visited.
  if (!(L && Visited.insert(L).second)) return;

  typedef VASTOperandList::op_iterator ChildIt;
  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(Root, L->op_begin()));

  while (!VisitStack.empty()) {
    VASTValue *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == VASTOperandList::GetDatapathOperandList(Node)->op_end()) {
      VisitStack.pop_back();

      // Visit the current Node.
      F(Node);

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTOperandList *L = VASTOperandList::GetDatapathOperandList(ChildNode)) {
      // ChildNode has a name means we had already visited it.
      if (!Visited.insert(L).second) continue;

      VisitStack.push_back(std::make_pair(ChildNode, L->op_begin()));
    }
  }
}

class VASTNamedValue : public VASTValue {
protected:
  VASTNamedValue(VASTTypes T, const char *Name, unsigned BitWidth)
    : VASTValue(T, BitWidth) {
    assert((T == vastSymbol || T == vastWire || T == vastSeqValue
            || T == vastLLVMValue)
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
           A->getASTType() == vastLLVMValue;
  }
};

class VASTSignal : public VASTNamedValue {
protected:
  VASTSignal(VASTTypes DeclType, const char *Name, unsigned BitWidth);

  virtual void anchor() const;
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSignal *A) { return true; }
  static inline bool classof(const VASTSeqValue *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastWire || A->getASTType() == vastSeqValue;
  }
};

class VASTSubModuleBase : public VASTNode {
  SmallVector<VASTSeqValue*, 8> Fanins;
  SmallVector<VASTValue*, 4> Fanouts;
protected:
  const unsigned Idx;

  VASTSubModuleBase(VASTTypes DeclType, const char *Name, unsigned Idx)
    : VASTNode(DeclType), Idx(Idx) {
    Contents.Name = Name;
  }

public:
  typedef SmallVectorImpl<VASTSeqValue*>::iterator fanin_iterator;
  fanin_iterator fanin_begin() { return Fanins.begin(); }
  fanin_iterator fanin_end() { return Fanins.end(); }

  typedef SmallVectorImpl<VASTSeqValue*>::const_iterator const_fanin_iterator;
  const_fanin_iterator fanin_begin() const { return Fanins.begin(); }
  const_fanin_iterator fanin_end()   const { return Fanins.end(); }

  typedef SmallVectorImpl<VASTValue*>::iterator fanout_iterator;

  void addFanin(VASTSeqValue *V);
  void addFanout(VASTValue *V);

  VASTValue *getFanout(unsigned Idx) const {
    return Fanouts[Idx];
  }

  VASTSeqValue *getFanin(unsigned Idx) const {
    return Fanins[Idx];
  }

  virtual void print(vlang_raw_ostream &OS, const VASTModule *Mod) const;
  void print(raw_ostream &OS) const;
};
} // end namespace

#endif
