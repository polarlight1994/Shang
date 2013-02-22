//===--- VASTSeqValue.cpp - The Value in the Sequential Logic ---*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VASTSeqValue. The VASTSeqValue represent the value in
// the sequential logic, it is not necessary SSA. The VASTSeqOp that define
// the values is available from the VASTSeqValue.
//
//===----------------------------------------------------------------------===//

#include "LangSteam.h"

#include "shang/VASTSeqValue.h"
#include "shang/VASTSlot.h"
#include "shang/VASTModule.h"
#define DEBUG_TYPE "vast-seq-value"
#include "llvm/Support/Debug.h"


using namespace llvm;
//----------------------------------------------------------------------------//
bool VASTSeqValue::buildCSEMap(std::map<VASTValPtr,
                                        std::vector<const VASTSeqOp*> >
                               &CSEMap) const {
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    VASTSeqUse U = *I;
    CSEMap[U].push_back(U.Op);
  }

  return !CSEMap.empty();
}

bool VASTSeqValue::verify() const {
  std::set<VASTSeqOp*, less_ptr<VASTSeqOp> > UniqueDefs;

  for (const_iterator I = begin(), E = end(); I != E; ++I) {
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
    for (const_iterator I = begin(), E = end(); I != E; ++I) {
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
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTSeqOp &Op = *(*I).Op;
    OS.indent(2) << "if (";
    Op.printPredicate(OS);
    OS << ") begin\n";

    OS.indent(4) << "$display(\"Condition: ";
    Op.printPredicate(OS);

    OS << ",  Src: " << VASTValPtr(*I);

    VASTSlot *S = Op.getSlot();
    OS << ", current slot: " << Op.getSlotNum() << ", ";

    if (BasicBlock *BB = S->getParent()) OS << BB->getName() << ',';

    OS << "\"); // ";
    if (Value *V = Op.getValue()) {
      OS << *V;
      if (Instruction *Inst = dyn_cast<Instruction>(V))
        OS << ", BB: " << Inst->getParent()->getName();
    }

    OS << "\n";
    OS.indent(2) << "end\n";
  }

  OS.indent(2) << "$finish();\nend\n";
}

void VASTSeqValue::addAssignment(VASTSeqOp *Op, unsigned SrcNo, bool IsDef) {
  if (IsDef)  Op->addDefDst(this);
  Assigns.push_back(VASTSeqUse(Op, SrcNo));
}

void VASTSeqValue::eraseUse(VASTSeqUse U) {
  iterator at = std::find(begin(), end(), U);
  assert(at != end() && "U is not in the assignment vector!");
  Assigns.erase(at);
}

void VASTSeqValue::printSelector(raw_ostream &OS, unsigned Bitwidth,
                                 bool PrintEnable) const {
  typedef std::vector<const VASTSeqOp*> OrVec;
  typedef std::map<VASTValPtr, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator it;

  CSEMapTy CSEMap;

  if (!buildCSEMap(CSEMap)) return;

  // Create the temporary signal.
  OS << "// Combinational MUX\n";

  if (getValType() != VASTSeqValue::Enable)
    OS << "reg " << VASTValue::printBitRange(Bitwidth, 0, false)
       << ' ' << getName() << "_selector_wire;\n";

  if (PrintEnable)
    OS << "reg " << ' ' << getName() << "_selector_enable = 0;\n\n";

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
    if (getValType() != VASTSeqValue::Enable)
      OS.indent(6) << getName() << "_selector_wire = " << I->first << ";\n";

    // Print the enable.
    if (PrintEnable) OS.indent(6) << getName() << "_selector_enable = 1'b1;\n";
    OS.indent(4) << "end\n";
  }

  // Write the default condition, otherwise latch will be inferred.
  OS.indent(4) << "default: begin\n";

  if (getValType() != VASTSeqValue::Enable)
    OS.indent(6) << getName() << "_selector_wire = " << Bitwidth << "'bx;\n";

  if (PrintEnable) OS.indent(6) << getName() << "_selector_enable = 1'b0;\n";
  OS.indent(4) << "end\n";
  OS.indent(2) << "endcase\nend  // end mux logic\n\n";
}

void VASTSeqValue::anchor() const {}

VASTSeqValue::~VASTSeqValue() {

}
