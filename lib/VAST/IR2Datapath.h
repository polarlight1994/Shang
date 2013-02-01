//===-------------- IR2Datapath.h - LLVM IR <-> VAST ------------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the classes which convert LLVM IR to VASTExprs.
//
//===----------------------------------------------------------------------===//

#ifndef VTM_LLVM_IR_TO_DATAPATH
#define VTM_LLVM_IR_TO_DATAPATH

#include "VASTExprBuilder.h"

#include "llvm/InstVisitor.h"
#include "llvm/ADT/ValueMap.h"

namespace llvm {
class DataLayout;

class DatapathBuilderContext : public VASTExprBuilderContext {
public:
  typedef DenseMap<Value*, VASTValPtr> ValueMapTy;

private:
  ValueMapTy Value2Expr;
  DataLayout *TD;

public:
  explicit DatapathBuilderContext(DataLayout *TD) : TD(TD) {}

  DataLayout *getDataLayout() const { return TD; }

  unsigned getValueSizeInBits(const Value *V) const;
  unsigned getValueSizeInBits(const Value &V) const {
    return getValueSizeInBits(&V);
  }

  virtual VASTValPtr getAsOperandImpl(Value *Op,
                                      bool GetAsInlineOperand = true) {
    llvm_unreachable("Function not implemented!");
    return 0;
  }

  VASTValPtr lookupExpr(Value *Val) const;
  VASTValPtr indexVASTExpr(Value *Val, VASTValPtr V);
};

class DatapathBuilder : public VASTExprBuilder,
                        public InstVisitor<DatapathBuilder,
                                           VASTValPtr> {
  DatapathBuilderContext &getContext() const {
    return static_cast<DatapathBuilderContext&>(Context);
  }


public:
  DatapathBuilder(DatapathBuilderContext &Context)
    : VASTExprBuilder(Context) {}

  DataLayout *getDataLayout() const { return getContext().getDataLayout(); }

  unsigned getValueSizeInBits(const Value *V) const {
    return getContext().getValueSizeInBits(V);
  }

  unsigned getValueSizeInBits(const Value &V) const {
    return getValueSizeInBits(&V);
  }

  VASTValPtr getAsOperand(Value *Op, bool GetAsInlineOperand = true) {
    return getContext().getAsOperandImpl(Op, GetAsInlineOperand);
  }

  // Value mapping.
  VASTValPtr lookupExpr(Value *Val) const {
    return getContext().lookupExpr(Val);
  }

  VASTValPtr indexVASTExpr(Value *Val, VASTValPtr V) {
    return getContext().indexVASTExpr(Val, V);
  }

  // Converting CastInst
  VASTValPtr visitTruncInst(TruncInst &I);
  VASTValPtr visitZExtInst(ZExtInst &I);
  VASTValPtr visitSExtInst(SExtInst &I);
  VASTValPtr visitBitCastInst(BitCastInst &I);

  VASTValPtr visitSelectInst(SelectInst &I);

  VASTValPtr visitICmpInst(ICmpInst &I);

  VASTValPtr visitGetElementPtrInst(GetElementPtrInst &I);

  VASTValPtr visitBinaryOperator(BinaryOperator &I);

  VASTValPtr visitInstruction(Instruction &I) {
    // Unhandled instructions can be safely ignored.
    return VASTValPtr();
  }
};
}

#endif
