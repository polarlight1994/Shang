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
  OS << Contents.Name << VASTValue::printBitRange(UB, LB, getBitWidth() > 1);
}

DatapathBuilder *MFDatapathContainer::createBuilder(MachineRegisterInfo *MRI) {
  assert(Builder == 0 && "The previous datapath build have not been release!");
  return (Builder = new DatapathBuilder(*this, *MRI));
}

VASTValPtr MFDatapathContainer::getOrCreateVASTMO(const char *Name,
                                                  MachineOperand DefMO) {
  DefMO.clearParent();
  assert((!DefMO.isReg() || !DefMO.isDef())
          && "The define flag should had been clear!");
  VASTMachineOperand *&VASTMO = VASTMOs[DefMO];
  if (!VASTMO)
    VASTMO = new (Allocator) VASTMachineOperand(Name, DefMO);

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
      V = getOrCreateVASTMO(allocateRegName(Reg), DefMO);
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

  std::string S;
  raw_string_ostream ss(S);
  ss << Op;
  ss.flush();

  return getOrCreateVASTMO(allocateName(S), Op);
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

VASTWire *MFDatapathContainer::exportValue(unsigned Reg) {
  VASTValPtr Val = Builder->lookupExpr(Reg);
  // The value do not exist.
  if (!Val) return 0;

  // Have we created the ported?
  VASTWire *&Wire = ExportedVals[Reg];

  // Do not create a port for the same register more than once.
  if (!Wire) {
    // Create the c-string and copy.
    const char *Name = allocateRegName(Reg);

    // The port is not yet exist, create it now.
    Wire = new (Allocator) VASTWire(Name, Val->getBitWidth());
    Wire->assign(Val);
  }

  return Wire;
}

const char *MFDatapathContainer::allocateName(const Twine &Name) {
  std::string Str = VBEMangle(Name.str());
  // Create the c-string and copy.
  char *CName = Allocator.Allocate<char>(Str.length() + 1);
  unsigned Term = Str.copy(CName, Str.length());
  CName[Term] = '\0';

  return CName;
}

const char *MFDatapathContainer::allocateRegName(unsigned Reg) {
  if (TargetRegisterInfo::isVirtualRegister(Reg)) {
    unsigned Idx = TargetRegisterInfo::virtReg2Index(Reg);
    return allocateName('v' + utostr_32(Idx) + 'r');
  } //else

  return allocateName('p' + utostr_32(Reg) + 'r');
}

void MFDatapathContainer::reset() {
  if (Builder) {
    delete Builder;
    Builder = 0;
  }

  VASTMOs.clear();
  Val2Reg.clear();
  ExportedVals.clear();
  DatapathContainer::reset();
}

void MFDatapathContainer::printTree(raw_ostream &OS, VASTWire *Root) {
  typedef VASTValue::dp_dep_it ChildIt;
  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(Root, VASTValue::dp_dep_begin(Root)));

  while (!VisitStack.empty()) {
    VASTValue *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == VASTValue::dp_dep_end(Node)) {
      VisitStack.pop_back();

      if (VASTExpr *E = dyn_cast<VASTExpr>(Node)) {
        const char *Name = allocateName("e" + utohexstr(uint64_t(E)) + "e");
        OS.indent(2) << "wire ";

        unsigned Bitwidth = E->getBitWidth();
        if (Bitwidth > 1) OS << "[" << (Bitwidth - 1) << ":0]";
        OS << ' ' << Name << " = ";
        E->printAsOperand(OS, false);
        OS << ";\n";

        // Assign the name to the expression.
        E->setExprName(Name);
      } else if (VASTWire *W = dyn_cast<VASTWire>(Node))
        W->printAssignment(OS.indent(2));

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->getAsLValue<VASTValue>();
    ++VisitStack.back().second;

    // ChildNode has a name means we had already visited it.
    if (VASTExpr *E = dyn_cast<VASTExpr>(ChildNode))
      if (E->hasName()) continue;

    if (!isa<VASTWire>(ChildNode) && !isa<VASTExpr>(ChildNode)) continue;

    VisitStack.push_back(std::make_pair(ChildNode,
                                        VASTValue::dp_dep_begin(ChildNode)));
  }
}

void
MFDatapathContainer::writeVerilog(raw_ostream &OS, StringRef ModuleName) {
  OS << "module " << ModuleName << "(\n";

  // Write the input list.
  typedef VASTMOMapTy::const_iterator in_iterator;
  for (in_iterator I = VASTMOs.begin(), E = VASTMOs.end(); I != E; ++I) {
    const VASTMachineOperand *MO = I->second;
    OS.indent(4) << "input wire";
    unsigned Bitwidth = MO->getBitWidth();
    if (Bitwidth > 1) OS << "[" << (Bitwidth - 1) << ":0]";

    OS << ' ' << MO->getName() << ",\n";
  }

  // And then the output list.
  typedef Reg2WireMapTy::const_iterator out_iterator;
  for (out_iterator I = ExportedVals.begin(), E = ExportedVals.end();
       I != E; ++I) {
    OS.indent(4) << "output wire";

    unsigned Bitwidth = I->second->getBitWidth();
    if (Bitwidth > 1) OS << "[" << (Bitwidth - 1) << ":0]";

    OS << ' ' << I->second->getName() << ",\n";
  }

  OS.indent(4) << "input wire dummy_" << ModuleName << "_output);\n";


  for (out_iterator I = ExportedVals.begin(), E = ExportedVals.end();
       I != E; ++I)
    printTree(OS, I->second);

  // Write the data-path.
  OS << "endmodule\n";
}
