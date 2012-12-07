//===- MachineFunction2Datapath.cpp - MachineFunction <-> VAST --*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the classes which convert MachineFunction to VASTExprs.
//
//===----------------------------------------------------------------------===//
#ifndef VTM_MACHINE_FUNCTION_TO_DATAPATH
#define VTM_MACHINE_FUNCTION_TO_DATAPATH

#include "VASTExprBuilder.h"

#include "vtm/VInstrInfo.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/Support/ErrorHandling.h"

namespace llvm {
class MachineRegisterInfo;

class DatapathBuilderContext : public VASTExprBuilderContext {
public:
  virtual VASTValPtr getAsOperandImpl(MachineOperand &Op,
                                      bool GetAsInlineOperand = true) {
    llvm_unreachable("Function not implemented!");
    return 0;
  }
};

class DatapathBuilder : public VASTExprBuilder {
public:
  typedef std::map<unsigned, VASTValPtr> RegIdxMapTy;
  MachineRegisterInfo &MRI;
private:
  RegIdxMapTy Idx2Expr;

  DatapathBuilderContext &getContext() {
    return static_cast<DatapathBuilderContext&>(Context);
  }

public:
  DatapathBuilder(VASTExprBuilderContext &Context, MachineRegisterInfo &MRI)
    : VASTExprBuilder(Context), MRI(MRI) {}

  VASTValPtr getAsOperand(MachineOperand &Op, bool GetAsInlineOperand = true) {
    return getContext().getAsOperandImpl(Op, GetAsInlineOperand);
  }

  VASTValPtr createAndIndexExpr(MachineInstr *MI, bool mayExisted = false);

  // Virtual register mapping.
  VASTValPtr lookupExpr(unsigned RegNo) const;
  VASTValPtr indexVASTExpr(unsigned RegNo, VASTValPtr V);

  // Build VASTExpr from MachineInstr.
  VASTValPtr buildDatapathExpr(MachineInstr *MI);
  VASTValPtr buildUnaryOp(MachineInstr *MI, VASTExpr::Opcode Opc);
  VASTValPtr buildInvert(MachineInstr *MI);
  VASTValPtr buildReduceOr(MachineInstr *MI);
  template<typename FnTy>
  VASTValPtr buildBinaryOp(MachineInstr *MI, FnTy F) {
    return  F(getAsOperand(MI->getOperand(1)), getAsOperand(MI->getOperand(2)),
              VInstrInfo::getBitWidth(MI->getOperand(0)), this);
  }

  VASTValPtr expandLUT(MachineInstr *MI);
  VASTValPtr buildSel(MachineInstr *MI);
  VASTValPtr buildAdd(MachineInstr *MI);
  VASTValPtr buildICmp(MachineInstr *MI);
  VASTValPtr buildBitSlice(MachineInstr *MI);

  // Create a condition from a predicate operand.
  VASTValPtr createCnd(MachineOperand Op);
  VASTValPtr createCnd(MachineInstr *MI) {
    return createCnd(*VInstrInfo::getPredOperand(MI));
  }
};
}

#endif
