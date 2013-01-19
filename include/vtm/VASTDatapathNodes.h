//===----- VASTDatapathNodes.h - Datapath Nodes in VerilogAST ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the Datapath Nodes in the Verilog Abstract Syntax Tree.
//
//===----------------------------------------------------------------------===//
#ifndef VTM_VAST_DATA_PATH_NODES_H
#define VTM_VAST_DATA_PATH_NODES_H

#include "vtm/VASTNodeBases.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/Allocator.h"

#include <map>

namespace llvm {
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

  static
  std::string buildLiteral(uint64_t Value, unsigned bitwidth, bool isMinValue);
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

class VASTSymbol : public VASTNamedValue {
  VASTSymbol(const char *Name, unsigned BitWidth);

  friend class VASTModule;
public:
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSymbol *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSymbol;
  }
};

class VASTExpr : public VASTValue, public VASTOperandList,
                 public FoldingSetNode {
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
    dpMux
  };
private:
  // Operands, right after this VASTExpr.
  const VASTUse *ops() const {
    return reinterpret_cast<const VASTUse*>(this + 1);
  }

  // The total operand of this expression.
  bool     IsNamed    : 1;

  VASTExpr(const VASTExpr&);              // Do not implement
  void operator=(const VASTExpr&);        // Do not implement

  VASTExpr(Opcode Opc, uint8_t numOps, unsigned UB, unsigned LB);

  friend class DatapathContainer;

  void printAsOperandInteral(raw_ostream &OS) const;

  void dropUses();

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
  const uint8_t Opc, UB, LB;
  Opcode getOpcode() const { return VASTExpr::Opcode(Opc); }
  const char *getFUName() const;
  const std::string getSubModName() const;

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

class VASTWire :public VASTSignal, public VASTOperandList {
  unsigned Idx : 31;
  bool IsPinned : 1;
  friend class VASTModule;

public:
  const char *const AttrStr;

  VASTWire(const char *Name, unsigned BitWidth, VASTUse *U, const char *Attr = "",
           bool IsPinned = false)
    : VASTSignal(vastWire, Name, BitWidth), VASTOperandList(U, 1),
      Idx(0), IsPinned(IsPinned), AttrStr(Attr) {
    new (Operands) VASTUse(this);
  }

  void assign(VASTValPtr V) {
    getOperand(0).set(V);
  }

  bool isPinned() const { return IsPinned; }
  void Pin(bool isPinned = true ) { IsPinned = isPinned; }
private:
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

  virtual void dropUses();
public:
  VASTValPtr getDriver() const { return getOperand(0).unwrap(); }

  VASTExprPtr getExpr() const {
    return getDriver() ? dyn_cast<VASTExprPtr>(getDriver()) : 0;
  }

  // Print the logic to the output stream.
  void printAssignment(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTWire *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastWire;
  }
};

typedef PtrInvPair<VASTWire> VASTWirePtr;

template<>
inline VASTExprPtr PtrInvPair<VASTWire>::getExpr() const {
  return get()->getExpr().invert(isInverted());
}

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
  BumpPtrAllocator &getAllocator() { return Allocator; }

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
} // end namespace

#endif
