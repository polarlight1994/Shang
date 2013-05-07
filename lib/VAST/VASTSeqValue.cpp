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
VASTSelector::VASTSelector(const char *Name, unsigned BitWidth, Type T,
                           VASTNode *Node)
  : VASTNode(vastSelector), Parent(Node, T), BitWidth(BitWidth),
    EnableU(this) {
  Contents.Name = Name;
}

VASTSelector::~VASTSelector() {
  DeleteContainerPointers(Fanins);
}

bool VASTSelector::buildCSEMap(std::map<VASTValPtr,
                                        std::vector<const VASTSeqOp*> >
                               &CSEMap) const {
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    VASTLatch U = *I;
    CSEMap[U].push_back(U.Op);
  }

  return !CSEMap.empty();
}

bool VASTSelector::getUniqueLatches(std::set<VASTLatch> &UniqueLatches) const {
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

bool VASTSelector::verify() const {
  std::set<VASTLatch> UniqueLatches;

  return getUniqueLatches(UniqueLatches);
}

void VASTSelector::verifyAssignCnd(vlang_raw_ostream &OS,
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
  OS << "if (!$onehot0(" << AllPred << ")) begin\n"
        "  $display(\"At time %t, register "
        << getName() << " in module " << ( Mod ? Mod->getName() : "Unknown")
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

void VASTSelector::addAssignment(VASTSeqOp *Op, unsigned SrcNo) {
  VASTLatch L = VASTLatch(Op, SrcNo);
  assert(L->getBitWidth() == getBitWidth()
         && "Bitwidth not matched in assignment!");
  Assigns.push_back(L);
}

void VASTSelector::printSelector(raw_ostream &OS, bool PrintEnable) const {
  if (empty()) return;

  assert(isSelectorSynthesized() && "Cannot print unsynthesized selector!");

  assert((isEnable() || !Fanins.empty())  && "Bad Fanin numder!");
  OS << "// Synthesized MUX\n";

  if (PrintEnable)
    OS << "wire " << ' ' << getName() << "_selector_enable = "
       << VASTValPtr(EnableU) << ";\n\n";

  if (isEnable()) return;

  OS << "reg " << VASTValue::printBitRange(getBitWidth(), 0, false)
      << ' ' << getName() << "_selector_wire;\n";

  // Print the mux logic.
  OS << "always @(*)begin  // begin mux logic\n";
  OS.indent(2) << VASTModule::ParallelCaseAttr << " case (1'b1)\n";

  for (const_fanin_iterator I = fanin_begin(), E = fanin_end(); I != E; ++I) {
    Fanin *FI = *I;
    VASTValPtr Src = VASTValPtr(FI->FI);

    OS.indent(4) << '(' << VASTValPtr(FI->Pred) << "): begin\n";
    // Print the assignment under the condition.
    OS.indent(6) << getName() << "_selector_wire = "
                  << VASTValPtr(FI->FI) << ";\n";
    OS.indent(4) << "end\n";
  }

  // Write the default condition, otherwise latch will be inferred.
  OS.indent(4) << "default: begin\n";

  OS.indent(6) << getName() << "_selector_wire = " << getBitWidth() << "'bx;\n";

  OS.indent(4) << "end\n";
  OS.indent(2) << "endcase\nend  // end mux logic\n\n";
}

void VASTSelector::printRegisterBlock(vlang_raw_ostream &OS,
                                      const VASTModule *Mod,
                                      uint64_t InitVal) const {

  if (empty()) {
    // Print the driver of the output ports.
    if (isa<VASTOutPort>(getParent())) {
      OS.always_ff_begin();
      OS << getName()  << " <= "
         << VASTImmediate::buildLiteral(InitVal, getBitWidth(), false) << ";\n";
      OS.always_ff_end();
    }

    return;
  }

  // Print the data selector of the register.
  printSelector(OS);

  OS.always_ff_begin();
  // Reset the register.
  OS << getName()  << " <= "
    << VASTImmediate::buildLiteral(InitVal, getBitWidth(), false) << ";\n";
  OS.else_begin();

  // Print the assignment.
  if (isEnable())
    OS << getName() << " <= " << getName() << "_selector_enable" << ";\n";
  else {
    OS.if_begin(Twine(getName()) + Twine("_selector_enable"));
    OS << getName() << " <= " << getName() << "_selector_wire"
       << VASTValue::printBitRange(getBitWidth(), 0, false) << ";\n";
    OS.exit_block();
  }

  OS << "// synthesis translate_off\n";
  verifyAssignCnd(OS, Mod);
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end();
}

VASTSelector::Fanin::Fanin(VASTNode *N) : Pred(N), FI(N) {}

void VASTSelector::Fanin::AddSlot(VASTSlot *S) {
  Slots.push_back(S);
}

void VASTSelector::synthesisSelector(VASTExprBuilder &Builder) {
  typedef std::vector<const VASTSeqOp*> OrVec;
  typedef std::map<VASTValPtr, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator it;

  CSEMapTy CSEMap;

  if (!buildCSEMap(CSEMap)) return;

  // Print the mux logic.
  SmallVector<VASTValPtr, 2> SeqOpPreds;
  SmallVector<VASTValPtr, 8> FaninPreds;
  SmallVector<VASTValPtr, 16> EnablePreds;

  for (it I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    Fanin *FI = 0;
    if (!isEnable()) {
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

  assert((!isEnable() || Fanins.empty()) && "Enable should has only 1 fanin!");
  EnableU.set(Builder.buildOrExpr(EnablePreds, 1));
}

void VASTSelector::setParent(VASTNode *N) {
  assert(Parent.getPointer() == 0 && "Parent had already existed!");
  Parent.setPointer(N);
}

VASTNode *VASTSelector::getParent() const {
  assert(Parent.getPointer() && "Unexpected null parent!");
  return Parent.getPointer();
}

VASTSeqValue *VASTSelector::getSSAValue() const {
  assert(num_uses() == 1 && "Not single assignment!");
  return *Users.begin();
}

void VASTSelector::addUser(VASTSeqValue *V) {
  assert(!Users.count(V) && "User existed!");
  Users.insert(V);
}

void VASTSelector::removeUser(VASTSeqValue *V) {
  bool erased = Users.erase(V);
  assert(erased && "V is not a user of the current selector!");
  (void) erased;
}

void VASTSelector::printDecl(raw_ostream &OS) const {
  VASTNamedValue::PrintDecl(OS, getName(), getBitWidth(), true);
}

void VASTSelector::print(raw_ostream &OS) const {
  llvm_unreachable("Not implemented!");
}

void VASTSelector::eraseFanin(VASTLatch U) {
  assert(!isSelectorSynthesized() && "Cannot erase latch!");
  iterator at = std::find(begin(), end(), U);
  assert(at != end() && "U is not in the assignment vector!");
  Assigns.erase(at);
}

//===----------------------------------------------------------------------===//
VASTSeqValue::VASTSeqValue(VASTSelector *Selector, unsigned Idx, Value *V)
  : VASTNamedValue(vastSeqValue, Selector->getName(), Selector->getBitWidth()),
    Selector(Selector), V(V), Idx(Idx) {
  Selector->addUser(this);
}

void VASTSeqValue::dumpFanins() const {
  typedef VASTSeqValue::const_fanin_iterator iterator;

  for (iterator I = fanin_begin(), E = fanin_end(); I != E; ++I) {
    VASTLatch U = *I;
    U.Op->dump();
  }
}

VASTSelector *VASTSeqValue::getSelector() const {
  assert(Selector && "Unexpected null selector!");
  return Selector;
}

void VASTSeqValue::changeSelector(VASTSelector *NewSel) {
  assert(NewSel != getSelector() && "Selector not changed!");
  getSelector()->removeUser(this);
  Selector = NewSel;
  if (Selector) Selector->addUser(this);
}

VASTSeqValue::~VASTSeqValue() {}

VASTNode *VASTSeqValue::getParent() const {
  return getSelector()->getParent();
}

Value *VASTSeqValue::getLLVMValue() const {
  return V;
}

//===----------------------------------------------------------------------===//
VASTRegister::VASTRegister(VASTSelector *Sel, uint64_t InitVal)
  : VASTNode(vastRegister), Sel(Sel), InitVal(InitVal) {
  Sel->setParent(this);
}

VASTRegister::VASTRegister() : VASTNode(vastRegister), Sel(0), InitVal(0) {}

void VASTRegister::print(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  Sel->printRegisterBlock(OS, Mod, InitVal);
}

void VASTRegister::print(raw_ostream &OS) const {
  vlang_raw_ostream S(dbgs());
  print(S, 0);
}

void VASTRegister::printDecl(raw_ostream &OS) const {
  VASTNamedValue::PrintDecl(OS, Sel->getName(), Sel->getBitWidth(), true, "");
  OS << " = " << VASTImmediate::buildLiteral(InitVal, Sel->getBitWidth(), false)
     <<  ";\n";
}
