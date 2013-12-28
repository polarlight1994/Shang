//===----- VASTDatapathNodes.h - Datapath Nodes in VerilogAST ---*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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

#include "vast/VASTNodeBases.h"

#include "llvm/IR/Value.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/Allocator.h"

#include <map>

namespace llvm {
class Value;
}

namespace vast {
using namespace llvm;

class VASTConstant : public VASTValue, public FoldingSetNode  {
  const APInt Int;

  VASTConstant(const APInt &Other)
    : VASTValue(vastConstant, Other.getBitWidth()), Int(Other) {}

  VASTConstant(const VASTConstant&)  LLVM_DELETED_FUNCTION;
  void operator=(const VASTConstant&)LLVM_DELETED_FUNCTION;

  void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const;

  void printAsOperandImpl(raw_ostream &OS) const {
    printAsOperandImpl(OS, getBitWidth(), 0);
  }

  friend class DatapathContainer;
  static VASTConstant TrueValue, FalseValue;
public:
  static VASTConstant *const True, *const False;

  /// Profile - Used to insert VASTImm objects, or objects that contain VASTImm
  ///  objects, into FoldingSets.
  void Profile(FoldingSetNodeID& ID) const;

  const APInt &getAPInt() const { return Int; }
  uint64_t getZExtValue() const { return Int.getZExtValue(); }
  bool getBoolValue() const { return Int.getBoolValue(); }

  APInt getBitSlice(unsigned UB, unsigned LB = 0) const {
    return getBitSlice(Int, UB, LB);
  }

  bool isAllZeros() const {
    return Int.isMinValue();
  }

  bool isAllOnes() const {
    return Int.isAllOnesValue();
  }

  bool isMaxSigned() const {
    return Int.isMaxSignedValue();
  }

  bool isMinSigned() const {
    return Int.isMinSignedValue();
  }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTConstant *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastConstant;
  }

  // Helper functions to manipulate APInt at bit level.
  static APInt getBitSlice(const APInt &Int, unsigned UB, unsigned LB = 0) {
    return Int.lshr(LB).sextOrTrunc(UB - LB);
  }

  static
  std::string buildLiteral(uint64_t Value, unsigned bitwidth, bool isMinValue);
};

typedef PtrInvPair<VASTConstant> VASTConstPtr;

template<>
inline APInt PtrInvPair<VASTConstant>::getAPInt() const {
  APInt Val = get()->getAPInt();
  if (isInverted()) Val.flipAllBits();
  return Val;
}

template<>
inline uint64_t PtrInvPair<VASTConstant>::getZExtValue() const {
  return PtrInvPair<VASTConstant>::getAPInt().getZExtValue();
}

template<>
inline bool PtrInvPair<VASTConstant>::getBoolValue() const {
  return PtrInvPair<VASTConstant>::getAPInt().getBoolValue();
}

template<>
inline
APInt PtrInvPair<VASTConstant>::getBitSlice(unsigned UB, unsigned LB) const {
  return VASTConstant::getBitSlice(getAPInt(), UB, LB);
}
template<>
inline bool PtrInvPair<VASTConstant>::isAllZeros() const {
  return isInverted() ? get()->isAllOnes() : get()->isAllZeros();
}
template<>
inline bool PtrInvPair<VASTConstant>::isAllOnes() const {
  return isInverted() ? get()->isAllZeros() : get()->isAllOnes();
}

template<>
inline bool PtrInvPair<VASTConstant>::isMaxSigned() const {
  return isInverted() ? (~get()->getAPInt()).isMaxSignedValue()
                      : get()->isMaxSigned();
}
template<>
inline bool PtrInvPair<VASTConstant>::isMinSigned() const {
  return isInverted() ? (~get()->getAPInt()).isMinSignedValue()
                      : get()->isMinSigned();
}

class VASTExpr : public VASTMaskedValue, public VASTOperandList,
                 public FoldingSetNode, public ilist_node<VASTExpr> {
public:
  enum Opcode {
    // bit level maniplulate expressions.
    dpBitCat,
    dpBitRepeat,
    dpBitExtract,
    // Mask (with and operation the operand by an constant).
    dpBitMask,
    LastBitManipulate = dpBitMask,
    // bitwise logic datapath
    dpAnd,
    dpRAnd,
    dpRXor,
    // Cannot inline.
    // FU datapath
    dpAdd,
    FirstFUOpc = dpAdd,
    dpMul,
    dpShl,
    dpAshr,
    dpLshr,
    dpSGT,
    FirstICmpOpc = dpSGT,
    dpUGT,
    LastFUOpc = dpUGT,
    LastICmpOpc = dpUGT,
    // Lookup-tables.
    dpLUT,
    // Combinational ROM, a big lookup table.
    // The bitwidth of the ROM is stored in the second operand, i.e. the wrapper
    // that wrap the actually ROM context.
    dpROMLookUp,
    //
    dpKeep
  };
private:
  // Operands, right after this VASTExpr.
  const VASTUse *ops() const {
    return reinterpret_cast<const VASTUse*>(this + 1);
  }

  VASTExpr(const VASTExpr&);              // Do not implement
  void operator=(const VASTExpr&);        // Do not implement

  // Create a generic expression.
  VASTExpr(Opcode Opc, ArrayRef<VASTValPtr> Ops, unsigned BitWidth);

  // Create a LUT expression.
  VASTExpr(ArrayRef<VASTValPtr> Ops, unsigned BitWidth, const char *SOP);

  // Create a BitExtract expression.
  VASTExpr(VASTValPtr Op, unsigned UB, unsigned LB);

  // Create a ROM lookup expression.
  VASTExpr(VASTValPtr Addr, VASTMemoryBank *Bank, unsigned BitWidth);

  VASTExpr();

  void initializeOperands(ArrayRef<VASTValPtr> Ops);

  friend struct ilist_sentinel_traits<VASTExpr>;

  friend class DatapathContainer;

  bool printAsOperandInteral(raw_ostream &OS) const;

  void dropUses();

  void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const;

public:
  ~VASTExpr();

  unsigned getUB() const { return getLB() + getBitWidth(); }
  unsigned getLB() const { return Contents16.ExprContents.LB; }

  Opcode getOpcode() const {
    return VASTExpr::Opcode(Contents16.ExprContents.Opcode);
  }

  bool isCommutative() const {
    return getOpcode() == dpAdd || getOpcode() == dpAnd || getOpcode() == dpMul;
  }

  const char *getFUName() const;
  bool isInstantiatedAsSubModule() const;
  void printSubModName(raw_ostream &OS) const;
  bool printFUInstantiation(raw_ostream &OS) const;

  unsigned getRepeatTimes() const {
    assert(getOpcode() == VASTExpr::dpBitRepeat && "Incorrect expr type!");
    unsigned PatternWidth = getOperand(0)->getBitWidth();
    assert(getBitWidth() % PatternWidth == 0 && "Bad bitrepeat bitwidth!");
    return getBitWidth() / PatternWidth;
  }

  const char *getLUT() const;
  bool isComplementSOP() const;

  VASTMemoryBank *getROMContent() const {
    assert(getOpcode() == VASTExpr::dpROMLookUp && "Incorrect expr type!");
    return Contents64.Bank;
  }

  bool isTimingBarrier() const {
    return getOpcode() == dpKeep;
  }

  inline bool isSubWord() const {
    return getOpcode() == VASTExpr::dpBitExtract &&
           (getUB() != getOperand(0)->getBitWidth() || getLB() != 0);
  }

  inline bool isLowerPart() const {
    return isSubWord() && getLB() == 0;
  }

  bool hasNameID() const;
  // Assign a name to this expression.
  void assignNameID(unsigned NameID);
  unsigned getNameID() const;
  void printName(raw_ostream &OS) const;

  void print(raw_ostream &OS) const { printAsOperandInteral(OS); }
  void printMaskVerification(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTExpr *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastExpr;
  }

  /// Profile - Used to insert VASTExpr objects, or objects that contain
  /// VASTExpr objects, into FoldingSets.
  void Profile(FoldingSetNodeID& ID) const;
  void ProfileWithoutOperands(FoldingSetNodeID& ID) const;

  /// Helper function returning the properties of the opcodes.
  static bool IsICmp(Opcode Opc) {
    return Opc >= FirstICmpOpc && Opc <= LastICmpOpc;
  }

  static unsigned GetResultBitWidth(Opcode Opc) {
    switch (Opc) {
    default:      return 0;
    case dpRAnd:  case dpRXor: case dpSGT:   case dpUGT:   return 1;
    }
  }

  template<typename T>
  void visitConeTopOrder(std::set<VASTExpr*> &Visited, T &F);
};

template<typename T>
void
VASTExpr::visitConeTopOrder(std::set<VASTExpr*> &Visited, T &F) {
  // The entire tree had been visited.
  if (!Visited.insert(this).second) return;

  typedef VASTOperandList::op_iterator ChildIt;
  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(this, this->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == Node->op_end()) {
      VisitStack.pop_back();

      // Visit the current Node.
      F(Node);

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTExpr *ChildExpr = dyn_cast<VASTExpr>(ChildNode)) {
      // ChildNode has a name means we had already visited it.
      if (!Visited.insert(ChildExpr).second) continue;

      VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));
    }
  }
}

class VASTWrapper :public VASTNamedValue, public ilist_node<VASTWrapper> {
  PointerUnion<Value*,VASTNode*> Data;

  friend struct ilist_sentinel_traits<VASTWrapper>;
  VASTWrapper() : VASTNamedValue(vastWrapper, 0, 1) {}

public:

  VASTWrapper(const char *Name, unsigned BitWidth, Value* LLVMValue)
    : VASTNamedValue(vastWrapper, Name, BitWidth), Data(LLVMValue) {}

  VASTWrapper(const char *Name, unsigned BitWidth, VASTNode* Node)
    : VASTNamedValue(vastWrapper, Name, BitWidth), Data(Node) {}

  Value *getLLVMValue() const { return Data.dyn_cast<Value*>(); }
  // Return true if the wire represents
  bool isX() const;

  VASTNode *getVASTNode() const { return Data.dyn_cast<VASTNode*>(); }

  virtual void printDecl(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTWrapper *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastWrapper;
  }
};

class VASTExprBuilderContext;

// The container to hold all VASTExprs in data-path of the design.
class DatapathContainer {
  // Use pointer to workaround the typeid problem.
  // The unique immediate in the data-path.
  FoldingSet<VASTConstant> UniqueConstants;

  // Expression in data-path
  FoldingSet<VASTExpr> UniqueExprs;

  // The string of SOP for the LUT expressions
  StringSet<> SOPs;

  VASTExprBuilderContext* CurContexts;
protected:
  BumpPtrAllocator Allocator;

  void removeValueFromCSEMaps(VASTNode *N);
  void addModifiedValueToCSEMaps(VASTNode *N);
  template<typename T>
  void addModifiedValueToCSEMaps(T *V, FoldingSet<T> &CSEMap);

  iplist<VASTExpr> Exprs;

  VASTValPtr invert(VASTValPtr V);

  void notifyDeletion(VASTExpr *E);

  /// Perform the Garbage Collection to release the dead objects on the
  /// DatapathContainer
  bool gcImpl();
public:
  DatapathContainer();
  virtual ~DatapathContainer();

  BumpPtrAllocator &getAllocator() { return Allocator; }

  VASTValPtr createExprImpl(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                            unsigned Bitwidth);
  VASTValPtr createLUTImpl(ArrayRef<VASTValPtr> Ops, unsigned Bitwidth,
                           StringRef SOP);
  VASTValPtr createBitExtractImpl(VASTValPtr Op, unsigned UB, unsigned LB);
  VASTValPtr
  createROMLookUpImpl(VASTValPtr Addr, VASTMemoryBank *Bank, unsigned BitWidth);

  virtual void replaceAllUseWithImpl(VASTValPtr From, VASTValPtr To);

  VASTConstant *getConstantImpl(const APInt &Value);

  VASTConstant *getConstantImpl(uint64_t Value, int8_t BitWidth) {
    return getConstantImpl(APInt(BitWidth, Value));
  }

  typedef ilist<VASTExpr>::iterator expr_iterator;
  expr_iterator expr_begin() { return Exprs.begin(); }
  expr_iterator expr_end() { return Exprs.end(); }

  typedef ilist<VASTExpr>::const_iterator const_expr_iterator;
  const_expr_iterator expr_begin() const { return Exprs.begin(); }
  const_expr_iterator expr_end() const  { return Exprs.end(); }

  void reset();

  void recursivelyDeleteTriviallyDeadExprs(VASTExpr *L);

  bool gc() {
    bool changed = false;

    // Iteratively release the dead objects.
    while (gcImpl())
      changed = true;

    return changed;
  }

  // Context management.
  void pushContext(VASTExprBuilderContext *Context);
  void popContext(VASTExprBuilderContext *Context);

};
} // end namespace

#endif
