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

#include "shang/VASTExprBuilder.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTSlot.h"
#include "shang/VASTModule.h"
#define DEBUG_TYPE "vast-seq-value"
#include "llvm/Support/Debug.h"


using namespace llvm;
//----------------------------------------------------------------------------//
void VASTSeqValue::dumpFanins() const {
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    VASTLatch U = *I;
    U.Op->dump();
  }
}

bool VASTSeqValue::buildCSEMap(std::map<VASTValPtr,
                                        std::vector<const VASTSeqOp*> >
                               &CSEMap) const {
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    VASTLatch U = *I;
    CSEMap[U].push_back(U.Op);
  }

  return !CSEMap.empty();
}

bool VASTSeqValue::getUniqueLatches(std::set<VASTLatch> &UniqueLatches) const {
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    VASTLatch U = *I;
    std::set<VASTLatch>::iterator at = UniqueLatches.find(U);
    if (at == UniqueLatches.end()) {
      UniqueLatches.insert(U);
      continue;
    }

    // If we find the latch with the same predicate, their fanin must be
    // identical.
    if (VASTValPtr(*at) != VASTValPtr(U))  return false;
  }

  return true;
}

bool VASTSeqValue::verify() const {
  std::set<VASTLatch> UniqueLatches;

  return getUniqueLatches(UniqueLatches);
}

void VASTSeqValue::verifyAssignCnd(vlang_raw_ostream &OS, const Twine &Name,
                                   const VASTModule *Mod) const {
  if (empty()) return;

  std::set<VASTLatch> UniqueLatches;

  bool Unique = getUniqueLatches(UniqueLatches);
  assert(Unique && "Assignement conflict detected!");
  (void) Unique;

  typedef std::set<VASTLatch>::iterator iterator;
  std::set<VASTValPtr> UniquePreds;
  typedef std::set<VASTValPtr>::iterator pred_iterator;

  // Concatenate all condition together to detect the case that more than one
  // case is activated.
  std::string AllPred;

  {
    raw_string_ostream AllPredSS(AllPred);

    AllPredSS << '{';
    for (iterator I = UniqueLatches.begin(), E = UniqueLatches.end();
         I != E; ++I) {
      VASTLatch L = *I;
      if (!L.getSlotActive()) {
        UniquePreds.insert(L.getPred());
        continue;
      }

      L.Op->printPredicate(AllPredSS);
      AllPredSS << ", ";
    }

    for (pred_iterator I = UniquePreds.begin(), E = UniquePreds.end();
         I != E; ++I)
      AllPredSS << *I << ", ";

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
  for (iterator I = UniqueLatches.begin(), E = UniqueLatches.end(); I != E; ++I) {
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
      if (Instruction *Inst = dyn_cast<Instruction>(V)) {
        OS << ", BB: " << Inst->getParent()->getName();
        OS << ' ' << *Inst;
      }
    }

    OS << "\n";
    OS.indent(2) << "end\n";
  }

  OS.indent(2) << "$finish();\nend\n";
}

void VASTSeqValue::addAssignment(VASTSeqOp *Op, unsigned SrcNo, bool IsDef) {
  if (IsDef)  Op->addDefDst(this);
  Assigns.push_back(VASTLatch(Op, SrcNo));
}

void VASTSeqValue::eraseLatch(VASTLatch U) {
  iterator at = std::find(begin(), end(), U);
  assert(at != end() && "U is not in the assignment vector!");
  Assigns.erase(at);
}

void VASTSeqValue::printSelector(raw_ostream &OS, unsigned Bitwidth,
                                 bool PrintEnable) const {
  if (empty()) return;

  if (!EnableU.isInvalid()) {
    assert((getValType() == VASTSeqValue::Enable || !Fanins.empty())
            && "Bad Fanin numder!");
    OS << "// Synthesized MUX\n";

    if (PrintEnable)
      OS << "wire " << ' ' << getName() << "_selector_enable = "
         << VASTValPtr(EnableU) << ";\n\n";

    if (getValType() == VASTSeqValue::Enable) return;

    OS << "reg " << VASTValue::printBitRange(Bitwidth, 0, false)
       << ' ' << getName() << "_selector_wire;\n";

    // Print the mux logic.
    OS << "always @(*)begin  // begin mux logic\n";
    OS.indent(2) << VASTModule::ParallelCaseAttr << " case (1'b1)\n";

    for (const_fanin_iterator I = fanin_begin(), E = fanin_end(); I != E; ++I) {
      Fanin *FI = *I;
      OS.indent(4) << '(' << VASTValPtr(FI->Pred) << "): begin\n";
      // Print the assignment under the condition.
      OS.indent(6) << getName() << "_selector_wire = "
                   << VASTValPtr(FI->FI) << ";\n";
      OS.indent(4) << "end\n";
    }

    // Write the default condition, otherwise latch will be inferred.
    OS.indent(4) << "default: begin\n";

    OS.indent(6) << getName() << "_selector_wire = " << Bitwidth << "'bx;\n";

    OS.indent(4) << "end\n";
    OS.indent(2) << "endcase\nend  // end mux logic\n\n";
    return;
  }

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
  DeleteContainerPointers(Fanins);
}

VASTSeqValue::Fanin::Fanin(VASTSeqValue *V) : Pred(V), FI(V) {}

void VASTSeqValue::Fanin::AddSlot(VASTSlot *S) {
  Slots.push_back(S);
}

void VASTSeqValue::synthesisSelector(VASTExprBuilder &Builder) {
  typedef std::vector<const VASTSeqOp*> OrVec;
  typedef std::map<VASTValPtr, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator it;

  CSEMapTy CSEMap;

  if (!buildCSEMap(CSEMap)) return;

  // Print the mux logic.
  SmallVector<VASTValPtr, 2> SeqOpPreds;
  SmallVector<VASTValPtr, 8> FaninPreds;
  SmallVector<VASTValPtr, 16> EnablePreds;

  bool IsEnable = (getValType() == VASTSeqValue::Enable);

  for (it I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    Fanin *FI = 0;
    if (!IsEnable) {
      FaninPreds.clear();
      FI = new Fanin(this);
    }

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      SeqOpPreds.clear();
      const VASTSeqOp *Op = *OI;
      if (VASTValPtr SlotActive = Op->getSlotActive()) {
        VASTValPtr V = SlotActive.getAsInlineOperand();
        if (V != this) SeqOpPreds.push_back(V);
        else           SeqOpPreds.push_back(SlotActive);
      }

      SeqOpPreds.push_back(Op->getPred());

      VASTValPtr FIPred = Builder.buildAndExpr(SeqOpPreds, 1);
      EnablePreds.push_back(FIPred);
      if (FI) {
        FaninPreds.push_back(FIPred);
        FI->AddSlot(Op->getSlot());
      }
    }

    // For enables, there is only 1 fanin, which is the Or of all predicated.
    if (FI == 0) continue;

    VASTValPtr CurPred = Builder.buildOrExpr(FaninPreds, 1);
    FI->Pred.set(CurPred);
    FI->FI.set(I->first);
    Fanins.push_back(FI);
  }

  assert((!IsEnable || Fanins.empty()) && "Enable should has only 1 fanin!");
  EnableU.set(Builder.buildOrExpr(EnablePreds, 1));
}

void VASTSeqValue::printStandAloneDecl(raw_ostream &OS) const {
  printDecl(OS, true, "");
  OS << " = "
     << VASTImmediate::buildLiteral(InitialValue, getBitWidth(), false)
     <<  ";\n";
}

void VASTSeqValue::printStandAlone(vlang_raw_ostream &OS,
                                   const VASTModule *Mod) const {
  VASTNode *Parent = getParent();
  if (Parent && !isa<VASTPort>(Parent)) return;

  if (empty()) return;

  // Print the data selector of the register.
  printSelector(OS);

  OS.always_ff_begin();
  // Reset the register.
  OS << getName()  << " <= "
     << VASTImmediate::buildLiteral(InitialValue, getBitWidth(), false)
     << ";\n";
  OS.else_begin();

  // Print the assignment.
  if (getValType() == VASTSeqValue::Enable)
    OS << getName() << " <= " << getName() << "_selector_enable" << ";\n";
  else {
    OS.if_begin(Twine(getName()) + Twine("_selector_enable"));
    OS << getName() << " <= " << getName() << "_selector_wire"
       << VASTValue::printBitRange(getBitWidth(), 0, false) << ";\n";
    OS.exit_block();
  }

  OS << "// synthesis translate_off\n";
  verifyAssignCnd(OS, getName(), Mod);
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end();
}
