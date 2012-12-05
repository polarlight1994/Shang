//==- MFDatapathContainer.h - A Comprehensive Datapath Container -*- C++ -*-==//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the M(achine)F(unction)DatapathContainer, which is a
// Comprehensive datapath container.
//
//===----------------------------------------------------------------------===//
#ifndef MACHINE_FUNCTION_DATAPATH_CONTAINER_H
#define MACHINE_FUNCTION_DATAPATH_CONTAINER_H

#include "MachineFunction2Datapath.h"

#include "vtm/VerilogAST.h"

namespace llvm {
class VASTMachineOperand : public VASTValue {
  const MachineOperand MO;
public:
  VASTMachineOperand(const MachineOperand &MO)
    : VASTValue(VASTNode::vastCustomNode, VInstrInfo::getBitWidth(MO)),
    MO(MO) {}

  MachineOperand getMO() const {
    return MO;
  }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTMachineOperand *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastCustomNode;
  }

  void printAsOperandImpl(raw_ostream &OS, unsigned UB, unsigned LB) const;
};

class MFDatapathContainer : public DatapathBuilderContext,
                            public DatapathContainer {
  DatapathBuilder *Builder;
  typedef DenseMap<MachineOperand, VASTMachineOperand*,
                    VMachineOperandValueTrait> VASTMOMapTy;
  VASTMOMapTy VASTMOs;

public:
  explicit MFDatapathContainer() : Builder(0) {}
  ~MFDatapathContainer() { reset(); }

  VASTValPtr getAsOperandImpl(MachineOperand &Op,
                              bool GetAsInlineOperand = true);

  VASTImmediate *getOrCreateImmediate(const APInt &Value) {
    return getOrCreateImmediateImpl(Value);
  }

  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned UB, unsigned LB) {
    return createExprImpl(Opc, Ops, UB, LB);
  }

  // TODO: Remember the outputs by wires?.
  VASTValPtr getOrCreateVASTMO(MachineOperand DefMO);

  DatapathBuilder *createBuilder(MachineRegisterInfo *MRI);

  DatapathBuilder *operator->() const { return Builder; }

  void reset();
};
}

#endif
