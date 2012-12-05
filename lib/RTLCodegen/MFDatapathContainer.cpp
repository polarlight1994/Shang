//=- MFDatapathContainer.cpp - A Comprehensive Datapath Container -*- C++ -*-=//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file imperment the MFDatapathContainer, which is a comprehensive
// datapath container.
//
//===----------------------------------------------------------------------===//


#include "MachineFunction2Datapath.h"
#include "MFDatapathContainer.h"

using namespace llvm;

template<typename T>
inline static T *check(T *Ptr) {
  assert(Ptr && "Bad pointer!");
  return Ptr;
}

void VASTMachineOperand::printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                            unsigned LB) const {
  OS << getMO() << '[' << UB << ',' << LB << ']';
}

DatapathBuilder *MFDatapathContainer::createBuilder(MachineRegisterInfo *MRI) {
  assert(Builder == 0 && "The previous datapath build have not been release!");
  return (Builder = new DatapathBuilder(*this, *MRI));
}

VASTValPtr MFDatapathContainer::getOrCreateVASTMO(MachineOperand DefMO) {
  DefMO.clearParent();
  assert((!DefMO.isReg() || !DefMO.isDef())
          && "The define flag should had been clear!");
  VASTMachineOperand *&VASTMO = VASTMOs[DefMO];
  if (!VASTMO)
    VASTMO = new (Allocator) VASTMachineOperand(DefMO);

  return VASTMO;
}

VASTValPtr MFDatapathContainer::getAsOperandImpl(MachineOperand &Op,
                                                 bool GetAsInlineOperand) {
  unsigned BitWidth = VInstrInfo::getBitWidth(Op);
  switch (Op.getType()) {
  case MachineOperand::MO_Register: {
    unsigned Reg = Op.getReg();
    if (!Reg) return 0;

    VASTValPtr V = Builder->lookupExpr(Reg);

    if (!V) {
      MachineInstr *DefMI = check(Builder->MRI.getVRegDef(Reg));
      assert(VInstrInfo::isControl(DefMI->getOpcode())
        && "Reg defined by data-path should had already been indexed!");
      MachineOperand DefMO = DefMI->getOperand(0);
      DefMO.setIsDef(false);
      V = getOrCreateVASTMO(DefMO);
    }

    // The operand may only use a sub bitslice of the signal.
    V = Builder->buildBitSliceExpr(V, BitWidth, 0);
    // Try to inline the operand.
    if (GetAsInlineOperand) V = V.getAsInlineOperand();
    return V;
                                    }
  case MachineOperand::MO_Immediate:
    return getOrCreateImmediateImpl(Op.getImm(), BitWidth);
  default: break;
  }

  return getOrCreateVASTMO(Op);
}

VASTValPtr MFDatapathContainer::buildDatapath(MachineInstr *MI) {
  if (!VInstrInfo::isDatapath(MI->getOpcode())) return 0;

  unsigned ResultReg = MI->getOperand(0).getReg();
  VASTValPtr V = Builder->buildDatapathExpr(MI);

  // Remember the register number mapping, the register maybe CSEd.
  unsigned FoldedReg = rememberRegNumForExpr<true>(V, ResultReg);
  // If ResultReg is not CSEd to other Regs, index the newly created Expr.
  if (FoldedReg == ResultReg)
    Builder->indexVASTExpr(FoldedReg, V);

  return V;
}

void MFDatapathContainer::reset() {
  if (Builder) {
    delete Builder;
    Builder = 0;
  }

  VASTMOs.clear();
  Val2Reg.clear();
  DatapathContainer::reset();
}
