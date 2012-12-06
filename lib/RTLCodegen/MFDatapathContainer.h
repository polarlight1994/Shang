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
  VASTMachineOperand(const char *Name, const MachineOperand &MO)
    : VASTValue(VASTNode::vastCustomNode, VInstrInfo::getBitWidth(MO)), MO(MO) {
    Contents.Name = Name;
  }

  MachineOperand getMO() const {
    return MO;
  }

  const char *getName() const { return Contents.Name; }

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
  // Remember the VASTValPtr for each "external" MachineOperand.
  typedef DenseMap<MachineOperand, VASTMachineOperand*,
                   VMachineOperandValueTrait> VASTMOMapTy;
  VASTMOMapTy VASTMOs;

  // Remember the register for each VASTValPtr.
  typedef std::map<VASTValPtr, unsigned> Val2RegMapTy;
  Val2RegMapTy Val2Reg;

  // Remember the values used by the control-path.
  typedef std::map<unsigned, VASTWire*> Reg2WireMapTy;
  Reg2WireMapTy ExportedVals;

  // Allocate the c-string for the name.
  const char *allocateName(const Twine &Name);
  const char *allocateRegName(unsigned Reg);

  VASTValPtr getOrCreateVASTMO(const char *Name, MachineOperand DefMO);

  // Print the datapath tree whose root is "root".
  void printTree(raw_ostream &OS, VASTWire *Root);

protected:
  typedef VASTMOMapTy::const_iterator FaninIterator;
  FaninIterator fanin_begin() const { return VASTMOs.begin(); }
  FaninIterator fanin_end() const { return VASTMOs.end(); }

  typedef Reg2WireMapTy::const_iterator FanoutIterator;
  FanoutIterator fanout_begin() const { return ExportedVals.begin(); }
  FanoutIterator fanout_end() const { return ExportedVals.end(); }
public:
  explicit MFDatapathContainer() : Builder(0) {}
  virtual ~MFDatapathContainer() { reset(); }

  VASTValPtr getAsOperandImpl(MachineOperand &Op,
                              bool GetAsInlineOperand = true);

  VASTImmediate *getOrCreateImmediate(const APInt &Value) {
    return getOrCreateImmediateImpl(Value);
  }

  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned UB, unsigned LB) {
    return createExprImpl(Opc, Ops, UB, LB);
  }

  // VASTValPtr to virtual register mapping.
  unsigned lookupRegNum(VASTValPtr V) const {
    Val2RegMapTy::const_iterator at = Val2Reg.find(V);
    return at == Val2Reg.end() ? 0 : at->second;
  }

  template<bool AllowDifference>
  unsigned rememberRegNumForExpr(VASTValPtr V, unsigned RegNo) {
    bool inserted;
    Val2RegMapTy::iterator at;
    assert(TargetRegisterInfo::isVirtualRegister(RegNo)
           && TargetRegisterInfo::virtReg2Index(RegNo)
              < Builder->MRI.getNumVirtRegs()
           && "Bad RegNo!");
    tie(at, inserted) = Val2Reg.insert(std::make_pair(V, RegNo));
    if (!inserted && at->second != RegNo) {
      assert(AllowDifference && "Expr is rewritten twice?");
      Builder->MRI.replaceRegWith(RegNo, at->second);
      RegNo = at->second;
    }

    return RegNo;
  }

  // Build VASTValPtr for a MachineInstr.
  VASTValPtr buildDatapath(MachineInstr *MI);

  // Export the VASTValPtr corresponding to Reg to the output of the datapath.
  VASTWire *exportValue(unsigned Reg);

  DatapathBuilder *createBuilder(MachineRegisterInfo *MRI);

  DatapathBuilder *operator->() const { return Builder; }

  void reset();

  // Write the data-path in form of VerilogHDL.
  void writeVerilog(raw_ostream &OS, const Twine &Name);
};
}

#endif
