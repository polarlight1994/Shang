//===---- ControlLogicBuilder.cpp - Build the control logic  ----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the functions related to the control path of the design.
//
//===----------------------------------------------------------------------===//
#include "VASTExprBuilder.h"

#include "vtm/VASTControlPathNodes.h"
#include "vtm/VASTSubModules.h"
#include "vtm/VASTModule.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#define DEBUG_TYPE "vtm-ctrl-logic-builder"
#include "llvm/Support/Debug.h"

using namespace llvm;

VASTSlot::VASTSlot(unsigned slotNum, MachineInstr *BundleStart, VASTModule *VM)
  : VASTNode(vastSlot), SlotReg(this, 0), SlotActive(this, 0),
    SlotReady(this, 0), StartSlot(slotNum), EndSlot(slotNum), II(~0),
    SlotNum(slotNum) {
  Contents.BundleStart = BundleStart;

  // Create the relative signals.
  std::string SlotName = "Slot" + utostr_32(slotNum);
  VASTRegister *R = VM->addRegister(SlotName + "r", 1, SlotNum == 0 ? 1 : 0,
                                    VASTSeqValue::Slot, SlotNum,
                                    VASTModule::DirectClkEnAttr.c_str());
  SlotReg.set(R->getValue());

  VASTWire *Ready = VM->addWire(SlotName + "Ready", 1,
                                VASTModule::DirectClkEnAttr.c_str());
  SlotReady.set(Ready);

  VASTWire *Active = VM->addWire(SlotName + "Active", 1,
                                 VASTModule::DirectClkEnAttr.c_str());
  SlotActive.set(Active);
}

VASTSlot::VASTSlot(unsigned slotNum, VASTSlot *StartSlot)
  : VASTNode(vastSlot), SlotReg(this, 0), SlotActive(this, 0),
    SlotReady(this, 0), StartSlot(slotNum), EndSlot(slotNum), II(~0),
    SlotNum(slotNum){
  Contents.BundleStart = 0;
  // Finish slot alias with the start slot.
  SlotReg.set(StartSlot->SlotReg);
  SlotReady.set(StartSlot->SlotReady);
  SlotActive.set(StartSlot->SlotActive);
}

MachineInstr *VASTSlot::getBundleStart() const {
  return Contents.BundleStart;
}

MachineBasicBlock *VASTSlot::getParentBB() const {
  if (MachineInstr *BundleStart = getBundleStart())
    return BundleStart->getParent();

  return 0;
}

VASTValPtr &VASTSlot::getOrCreateSuccCnd(VASTSlot *DstSlot) {
  assert(DstSlot && "Bad DstSlot!");
  VASTValPtr &U = NextSlots[DstSlot];
  // If we are adding a new succ slot, link the DstSlot to current slot as well.
  if (!U) DstSlot->PredSlots.push_back(this);

  return U;
}

bool VASTSlot::hasNextSlot(VASTSlot *NextSlot) const {
  if (NextSlots.empty()) return NextSlot->SlotNum == SlotNum + 1;

  return NextSlots.count(NextSlot);
}

VASTRegister *VASTSlot::getRegister() const {
  return cast<VASTRegister>(getValue()->getParent());
}

VASTSeqValue *VASTSlot::getValue() const {
  return cast<VASTSeqValue>(SlotReg);
}

const char *VASTSlot::getName() const {
  return getValue()->getName();
}

void VASTSlot::print(raw_ostream &OS) const {
  OS << "Slot#"<< SlotNum << " Pred: ";
  for (const_pred_iterator I = pred_begin(), E = pred_end(); I != E; ++I)
    OS << "S#" << (*I)->SlotNum << ", ";

  OS << '\n';

  for (const_op_iterator I = op_begin(), E = op_end(); I != E; ++I)
    (*I)->print(OS.indent(2));

  OS << "Succ: ";

  for (const_succ_cnd_iterator I = succ_cnd_begin(), E = succ_cnd_end();
       I != E; ++I) {
    OS << "S#" << I->first->SlotNum << " (";
    I->second.printAsOperand(OS);
    OS << "), ";
  }
}

//===----------------------------------------------------------------------===//
VASTSeqUse::operator VASTUse &() const {
  return Op->getUseInteranal(No);
}

VASTSeqUse::operator VASTValPtr() const {
  return Op->getUseInteranal(No);
}

VASTUse &VASTSeqUse::operator ->() const {
  return Op->getUseInteranal(No);
}

VASTSlot *VASTSeqUse::getSlot() const {
  return Op->getSlot();
}

VASTUse &VASTSeqUse::getPred() const {
  return Op->getPred();
}

VASTValPtr VASTSeqUse::getSlotActive() const {
  return Op->getSlotActive();
}

VASTSeqValue *VASTSeqUse::getDst() const {
  return cast<VASTSeqValue>(&Op->getUseInteranal(No).getUser());
}

//===----------------------------------------------------------------------===//
VASTSeqOp *VASTSeqDef::operator ->() const {
  return Op;
}

VASTSeqDef::operator VASTSeqValue *() const {
  return Op->Defs[No];
}

const char *VASTSeqDef::getName() const {
  return Op->Defs[No]->getName();
}

//----------------------------------------------------------------------------//
bool VASTSeqOp::operator <(const VASTSeqOp &RHS) const  {
  // Same predicate?
  if (getPred() < RHS.getPred()) return true;
  else if (getPred() > RHS.getPred()) return false;

  // Same slot?
  return getSlot() < RHS.getSlot();
}

void VASTSeqOp::addDefDst(VASTSeqValue *Def) {
  assert(std::find(Defs.begin(), Defs.end(), Def) == Defs.end()
         && "Define the same seqval twice!");
  Defs.push_back(Def);
}

void VASTSeqOp::addSrc(VASTValPtr Src, unsigned SrcIdx, bool IsDef, VASTSeqValue *D) {
  assert(SrcIdx < getNumSrcs() && "Bad source index!");
  // The source value of assignment is used by the SeqValue.
  new (src_begin() + SrcIdx) VASTUse(D ? (VASTNode*)D : (VASTNode*)this, Src);
  // Do not add the assignment if the source is invalid.
  if (Src && D) D->addAssignment(this, SrcIdx, IsDef);
}

void VASTSeqOp::print(raw_ostream &OS) const {
  for (unsigned I = 0, E = getNumDefs(); I != E; ++I) {
    OS << Defs[I]->getName() << ", ";
  }
  
  OS << "<-@" << getSlotNum() << "{ pred";
  for (unsigned i = 0; i < Size; ++i) {
    if (VASTValPtr V = getOperand(i).unwrap()) V.printAsOperand(OS);
    else                                       OS << "<nullptr>";
    OS << ", ";
  }
  OS << "} ";

  if (getDefMI()) getDefMI()->print(OS);
  else            OS << '\n';
}

void VASTSeqOp::printPredicate(raw_ostream &OS) const {
  OS << '(';
  if (VASTValPtr SlotActive = getSlotActive()) {
    SlotActive.printAsOperand(OS);
    OS << '&';
  }

  getPred().printAsOperand(OS);

  OS << ')';
}

VASTSeqOp::VASTSeqOp(VASTSlot *S, bool UseSlotActive, MachineInstr *DefMI,
                     VASTUse *Operands, unsigned Size, bool IsVirtual)
  : VASTOperandList(Operands, Size + 1), VASTNode(vastSeqOp), S(S, UseSlotActive),
    DefMI(DefMI, IsVirtual) {
  S->addOperation(this);
}

unsigned VASTSeqOp::getSlotNum() const { return getSlot()->SlotNum; }

VASTValPtr VASTSeqOp::getSlotActive() const {
  if (S.getInt())
    return getSlot()->getActive();

  return VASTValPtr();
}

VASTOperandList *VASTOperandList::GetOperandList(VASTNode *N) {
  if (VASTOperandList *L = GetDatapathOperandList(N))
    return L;

  return dyn_cast_or_null<VASTSeqOp>(N);
}
//----------------------------------------------------------------------------//

bool VASTSeqValue::buildCSEMap(std::map<VASTValPtr,
                                        std::vector<const VASTSeqOp*> >
                               &CSEMap) const {
  for (const_itertor I = begin(), E = end(); I != E; ++I) {
    VASTSeqUse U = *I;
    CSEMap[U].push_back(U.Op);
  }

  return !CSEMap.empty();
}

bool VASTSeqValue::verify() const {
  std::set<VASTSeqOp*, less_ptr<VASTSeqOp> > UniqueDefs;

  for (const_itertor I = begin(), E = end(); I != E; ++I) {
    VASTSeqUse U = *I;
    if (!UniqueDefs.insert(U.Op).second)
      return false;
  }

  return true;
}

void VASTSeqValue::verifyAssignCnd(vlang_raw_ostream &OS, const Twine &Name,
                                   const VASTModule *Mod) const {
  if (empty()) return;

  assert(verify() && "Assignement conflict detected!");

  // Concatenate all condition together to detect the case that more than one
  // case is activated.
  std::string AllPred;

  {
    raw_string_ostream AllPredSS(AllPred);

    AllPredSS << '{';
    for (const_itertor I = begin(), E = end(); I != E; ++I) {
      (*I).Op->printPredicate(AllPredSS);
      AllPredSS << ", ";
    }
    AllPredSS << "1'b0 }";
  }

  // As long as $onehot0(expr) returns true if at most one bit of expr is high,
  // we can use it to detect if more one case condition is true at the same
  // time.
  OS << "if (!$onehot0(" << AllPred << "))"
        " begin\n $display(\"At time %t, register "
        << Name << " in module " << ( Mod ? Mod->getName() : "Unknown")
        << " has more than one active assignment: %b!\", $time(), "
        << AllPred << ");\n";

  // Display the conflicted condition and its slot.
  for (const_itertor I = begin(), E = end(); I != E; ++I) {
    const VASTSeqOp &Op = *(*I).Op;
    OS.indent(2) << "if (";
    Op.printPredicate(OS);
    OS << ") begin\n";

    OS.indent(4) << "$display(\"Condition: ";
    Op.printPredicate(OS);

    unsigned CndSlot = Op.getSlotNum();
    VASTSlot *S = Mod->getSlot(CndSlot);
    OS << ", current slot: " << CndSlot << ", ";

    if (MachineBasicBlock *MBB = S->getParentBB()) {
      OS << "in BB#" << MBB->getNumber() << ',';
      if (const BasicBlock *BB = MBB->getBasicBlock())
        OS << BB->getName() << ',';
    }

    if (S->hasAliasSlot()) {
      OS << " Alias slots: ";
      for (unsigned s = S->alias_start(), e = S->alias_end(), ii = S->alias_ii();
           s < e; s += ii)
        OS << s << ", ";
    }
    OS << "\");\n";
    OS.indent(2) << "end\n";
  }

  OS.indent(2) << "$finish();\nend\n";
}

void VASTSeqValue::addAssignment(VASTSeqOp *Op, unsigned SrcNo, bool IsDef) {
  if (IsDef)  Op->addDefDst(this);
  Assigns.push_back(VASTSeqUse(Op, SrcNo));
}

void VASTSeqValue::printSelector(raw_ostream &OS, unsigned Bitwidth) const {
  typedef std::vector<const VASTSeqOp*> OrVec;
  typedef std::map<VASTValPtr, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator it;

  CSEMapTy CSEMap;

  if (!buildCSEMap(CSEMap)) return;

  // Create the temporary signal.
  OS << "// Combinational MUX\n"
     << "reg " << VASTValue::printBitRange(Bitwidth, 0, false)
     << ' ' << getName() << "_selector_wire;\n"
     << "reg " << ' ' << getName() << "_selector_enable = 0;\n\n";

  // Print the mux logic.
  OS << "always @(*)begin  // begin mux logic\n";
  OS.indent(2) << VASTModule::ParallelCaseAttr << " case (1'b1)\n";

  for (it I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    OS.indent(4) << '(';

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI)
    {
      (*OI)->printPredicate(OS);
      OS << '|';
    }

    OS << "1'b0): begin\n";
    // Print the assignment under the condition.
    OS.indent(6) << getName() << "_selector_wire = ";
    I->first.printAsOperand(OS);
    OS << ";\n";
    // Print the enable.
    OS.indent(6) << getName() << "_selector_enable = 1'b1;\n";
    OS.indent(4) << "end\n";
  }

  // Write the default condition, otherwise latch will be inferred.
  OS.indent(4) << "default: begin\n";
  OS.indent(6) << getName() << "_selector_wire = " << Bitwidth << "'bx;\n";
  OS.indent(6) << getName() << "_selector_enable = 1'b0;\n";
  OS.indent(4) << "end\n";
  OS.indent(2) << "endcase\nend  // end mux logic\n\n";
}

void VASTSeqValue::anchor() const {}
