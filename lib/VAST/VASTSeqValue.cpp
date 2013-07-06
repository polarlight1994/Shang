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
// the sequential logic, it is in SSA form.
// This file also implement the VASTSelector, which defines VASTSeqValues by
// its fanins.
//
//===----------------------------------------------------------------------===//

#include "LangSteam.h"

#include "shang/VASTExprBuilder.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTSlot.h"
#include "shang/VASTModule.h"

#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vast-seq-value"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<bool> IgnoreTrivialLoops("shang-selector-ignore-trivial-loops",
  cl::desc("Ignore the trivial loops (R -> R) in the selector"),
  cl::init(true));

static cl::opt<bool> IgnoreXFanins("shang-selector-ignore-x-fanins",
  cl::desc("Ignore the undefined fanins (x) in the selector"),
  cl::init(true));

static cl::opt<bool> PrintMUXAsParallelCase("shang-print-selector-as-parallel-case",
  cl::desc("Print the selector as parallel case"),
  cl::init(false));

//----------------------------------------------------------------------------//
VASTSelector::VASTSelector(const char *Name, unsigned BitWidth, Type T,
                           VASTNode *Node)
  : VASTNode(vastSelector), Parent(Node), BitWidth(BitWidth), T(T),
    PrintSelModule(false), ClkEn(this), Guard(this) {
  Contents.Name = Name;
}

VASTSelector::~VASTSelector() {
  DeleteContainerPointers(Fanins);
}

static const char *getValName(VASTValue *V) {
  if (VASTExpr *E = dyn_cast<VASTExpr>(V))
    return E->getTempName();

  if (VASTNamedValue *NV = dyn_cast<VASTNamedValue>(V))
    return NV->getName();

  return 0;
}

static const VASTSelector *getSelector(VASTValue *V) {
  if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(V))
    return SV->getSelector();

  return 0;
}

bool
VASTSelector::StructualLess::operator()(VASTValPtr LHS, VASTValPtr RHS) const {
  if (LHS && RHS && LHS.isInverted() == RHS.isInverted()) {
    const char *LHSName = getValName(LHS.get()),
               *RHSName = getValName(RHS.get());
    if (LHSName && RHSName)
      return LHSName < RHSName;

    const VASTSelector *LHSSel = getSelector(LHS.get()),
                       *RHSSel = getSelector(RHS.get());

    if (LHSSel && RHSSel)
      return LHSSel < RHSSel;
  }

  return LHS < RHS;
}

bool VASTSelector::isTrivialFannin(const VASTLatch &L) const {
  VASTValPtr FIVal = L;

  // Ignore the trivial loops.
  if (VASTSeqValue *V = dyn_cast<VASTSeqValue>(FIVal))
    if (V->getSelector() == this && IgnoreTrivialLoops)
      return true;

  // Ignore the X values.
  if (VASTWire *W = dyn_cast<VASTWire>(FIVal.get()))
    if (W->isX() && IgnoreXFanins)
      return true;

  return false;
}

bool VASTSelector::buildCSEMap(CSEMapTy &CSEMap) const {
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    VASTLatch U = *I;

    if (isTrivialFannin(U)) continue;

    CSEMap[U].push_back(U.Op);
  }

  return !CSEMap.empty();
}

void VASTSelector::verifyAssignCnd(vlang_raw_ostream &OS,
                                   const VASTModule *Mod) const {
  if (empty()) return;

  // Concatenate all condition together to detect the case that more than one
  // case is activated.
  std::string AllCnd;

  {
    raw_string_ostream AllCndSS(AllCnd);
    std::set<VASTValPtr, StructualLess> IdenticalCnds;

    AllCndSS << '{';
    for (const_iterator I = begin(), E = end(); I != E; ++I) {
      const VASTLatch &L = *I;
      const VASTSeqOp *Op = L.Op;
      if (!Op->guardedBySlotActive()) {
        bool visited = !IdenticalCnds.insert(Op->getGuard()).second;
        // For the guarding condition without slot active, only print them
        // once.
        if (visited) continue;
      }

      Op->printGuard(AllCndSS);
      AllCndSS << ", ";
    }

    AllCndSS << "1'b0 }";
  }

  // As long as $onehot0(expr) returns true if at most one bit of expr is high,
  // we can use it to detect if more one case condition is true at the same
  // time.
  OS << "if (!$onehot0(" << AllCnd << ")) begin\n"
        "  $display(\"At time %t, register "
        << getName() << " in module " << ( Mod ? Mod->getName() : "Unknown")
        << " has more than one active assignment: %b!\", $time(), "
        << AllCnd << ");\n";

  // Display the conflicted condition and its slot.
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTLatch &L = *I;
    const VASTSeqOp *Op = L.Op;
    OS.indent(2) << "if (";
    Op->printGuard(OS);
    OS << ") begin\n";

    OS.indent(4) << "$display(\"Condition: ";
    Op->printGuard(OS);

    OS << ",  Src: " << VASTValPtr(L);

    VASTSlot *S = Op->getSlot();
    OS << ", current slot: " << Op->getSlotNum() << ", ";

    if (BasicBlock *BB = S->getParent()) OS << BB->getName() << ',';

    OS << "\"); /* ";
    if (Value *V = Op->getValue()) {
      OS << *V;
      if (Instruction *Inst = dyn_cast<Instruction>(V))
        OS << ", BB: " << Inst->getParent()->getName();
    }

    OS << "*/\n";
    OS.indent(2) << "end\n";
  }

  OS.indent(2) << "$finish(1);\nend\n";
}

void VASTSelector::addAssignment(VASTSeqOp *Op, unsigned SrcNo) {
  VASTLatch L = VASTLatch(Op, SrcNo);
  assert(L->getBitWidth() == getBitWidth()
         && "Bitwidth not matched in assignment!");
  Assigns.push_back(L);
}

void VASTSelector::printSelectorModule(raw_ostream &O) const {
  vlang_raw_ostream OS(O);

  if (empty()) return;

  OS << "module shang_" << getName() << "_selector(";
  OS.module_begin();
  // Print the input ports.
  for (unsigned i = 0, e = size(); i < e; ++i) {
    OS << "input wire" << VASTValue::printBitRange(getBitWidth())
       << " fi" << i << ",\n";
    OS << "input wire guard" << i << ",\n";
    OS << "input wire clken" << i << ",\n";
  }

  OS << "output wire" << VASTValue::printBitRange(getBitWidth()) << " fo,\n"
        "output wire guard,\n"
        "output wire clken);\n\n";

  // Build the enalbe.
  OS << "assign guard = guard0";
  for (unsigned i = 1, e = size(); i < e; ++i)
    OS << " | guard" << i;
  OS << ";\n";

  OS << "assign clken = clken0";
  for (unsigned i = 1, e = size(); i < e; ++i)
    OS << " | clken" << i;
  OS << ";\n";

  // Build the data output.
  OS << "assign fo = ({" << getBitWidth() << "{guard0}} & fi0)";
  for (unsigned i = 1, e = size(); i < e; ++i)
    OS << "| ({" << getBitWidth() << "{guard" << i << "}} & fi" << i << ')';
  OS << ";\n";

  OS.module_end();
  OS << '\n';
  OS.flush();
}

void VASTSelector::instantiateSelector(raw_ostream &OS) const {
  if (empty()) return;

  // Create the temporary signal.
  OS << "// Combinational MUX\n";

  OS << "wire " << VASTValue::printBitRange(getBitWidth(), 0, false)
     << ' ' << getName() << "_selector_wire;\n";

  OS << "wire " << ' ' << getName() << "_selector_guard;\n\n";
  OS << "wire " << ' ' << getName() << "_selector_clken;\n\n";

  OS << "shang_" << getName() << "_selector " << getName() << "_selector(";
  // Print the inputs of the mux.
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTLatch &L = *I;
    // Ignore the trivial fanins.
    if (isTrivialFannin(L)) {
      OS << VASTImmediate::buildLiteral(0, getBitWidth(), false) << ", 1'b0, ";
      continue;
    }

    // Print the fanin.
    OS << VASTValPtr(L);
    // Print the guarding condition, the combination of SlotActive and the
    // control-flow guard.
    OS << ", (" << VASTValPtr(L.getGuard());
    if (VASTValPtr V = L.getSlotActive())
      OS << " & " << V;
    OS << "),\n";
    if (VASTValPtr V = L.getSlotActive())
      OS << V;
    else // Promote the guard to clock enable if clock enable does not present.
      OS << VASTValPtr(L.getGuard());
    OS << ",\n";
  }

  OS << getName() << "_selector_wire,\n"
     << getName() << "_selector_guard,\n"
     << getName() << "_selector_clken);\n";
}

void VASTSelector::printMUXAsBigOr(raw_ostream &OS) const {
  OS << "wire " << VASTValue::printBitRange(getBitWidth(), 0, false)
    << ' ' << getName() << "_selector_wire = ";


  for (const_fanin_iterator I = fanin_begin(), E = fanin_end(); I != E; ++I) {
    Fanin *FI = *I;

    OS << "({" << getBitWidth() << '{' << VASTValPtr(FI->Guard) << "}} & "
       << VASTValPtr(FI->FI) << ") | ";
  }

  OS << VASTImmediate::buildLiteral(0, getBitWidth(), false) << ";\n";
}

void VASTSelector::printMuxAsParallelCase(raw_ostream &OS) const {
  OS << "reg " << VASTValue::printBitRange(getBitWidth(), 0, false)
    << ' ' << getName() << "_selector_wire;\n";

  // Print the mux logic.
  OS << "always @(*)begin  // begin mux logic\n";
  OS.indent(2) << VASTModule::ParallelCaseAttr << " case (1'b1)\n";

  for (const_fanin_iterator I = fanin_begin(), E = fanin_end(); I != E; ++I) {
    Fanin *FI = *I;

    OS.indent(4) << '(' << VASTValPtr(FI->Guard) << "): begin\n";
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

void VASTSelector::printSelector(raw_ostream &OS, bool PrintEnable) const {
  if (empty()) return;

  if (forcePrintSelModule()) {
    instantiateSelector(OS);
    return;
  }

  assert((isEnable() || !Fanins.empty())  && "Bad Fanin numder!");
  OS << "// Synthesized MUX\n";

  if (PrintEnable) {
    OS << "wire " << ' ' << getName() << "_selector_clken = "
       << VASTValPtr(ClkEn) << ";\n";
    OS << "wire " << ' ' << getName() << "_selector_guard = "
       << VASTValPtr(Guard) << ";\n\n";
  }

  if (isEnable()) return;

  if (PrintMUXAsParallelCase) printMuxAsParallelCase(OS);
  else                        printMUXAsBigOr(OS);
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
  OS.if_begin(Twine(getName()) + Twine("_selector_clken"));
  if (isEnable()) {
    OS << getName() << " <= " << getName() << "_selector_guard" << ";\n";
    OS.else_begin() << getName() << " <=  1'b0;\n";
  } else {
    OS.if_begin(Twine(getName()) + Twine("_selector_guard"));
    OS << getName() << " <= " << getName() << "_selector_wire"
       << VASTValue::printBitRange(getBitWidth(), 0, false) << ";\n";
    OS.exit_block();
  }
  OS.exit_block();

  OS << "// synthesis translate_off\n";
  verifyAssignCnd(OS, Mod);
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_end();
}

VASTSelector::Fanin::Fanin(VASTNode *N) : ClkEn(N), Guard(N), FI(N) {}

void VASTSelector::Fanin::AddSlot(VASTSlot *S) {
  Slots.push_back(S);
}

void VASTSelector::synthesizeSelector(VASTExprBuilder &Builder) {
  typedef std::vector<const VASTSeqOp*> OrVec;
  typedef CSEMapTy::const_iterator it;

  CSEMapTy CSEMap;

  if (!buildCSEMap(CSEMap)) return;

  // Print the mux logic.
  std::set<VASTValPtr, StructualLess> FaninClkens, FaninGuards,
                                      AllClkEns, AllGuards;

  for (it I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    VASTValPtr FIVal = I->first;

    Fanin *FI = 0;
    if (!isEnable()) {
      FaninClkens.clear();
      FaninGuards.clear();
      FI = new Fanin(this);
    }

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      SmallVector<VASTValPtr, 2> CurGuards;
      const VASTSeqOp *Op = *OI;
      // Promote the guard to clock enable by default, it will be overwritten if
      // clock enable presents
      VASTValPtr CurClkEn = Op->getGuard();

      if (VASTValPtr SlotActive = Op->getSlotActive()) {
        CurGuards.push_back(SlotActive);
        CurClkEn = SlotActive;
      }

      CurGuards.push_back(Op->getGuard());
      VASTValPtr CurGuard = Builder.buildAndExpr(CurGuards, 1);

      AllGuards.insert(CurGuard);
      AllClkEns.insert(CurClkEn);

      if (FI == 0) continue;

      FaninGuards.insert(CurGuard);
      FaninClkens.insert(CurClkEn);
      FI->AddSlot(Op->getSlot());
    }

    // For enables, there is only 1 fanin, which is the Or of all predicated.
    if (FI == 0) continue;

    SmallVector<VASTValPtr, 4> ClkEns(FaninClkens.begin(), FaninClkens.end());
    FI->ClkEn.set(Builder.buildOrExpr(ClkEns, 1));
    SmallVector<VASTValPtr, 4> Guards(FaninGuards.begin(), FaninGuards.end());
    FI->Guard.set(Builder.buildOrExpr(Guards, 1));
    FI->FI.set(FIVal);
    Fanins.push_back(FI);
  }

  assert((!isEnable() || Fanins.empty()) && "Enable should has only 1 fanin!");
  SmallVector<VASTValPtr, 4> ClkEnArray(AllClkEns.begin(), AllClkEns.end());
  ClkEn.set(Builder.buildOrExpr(ClkEnArray, 1));
  SmallVector<VASTValPtr, 4> GuardArray(AllGuards.begin(), AllGuards.end());
  Guard.set(Builder.buildOrExpr(GuardArray, 1));
}

void VASTSelector::setParent(VASTNode *N) {
  assert(Parent == 0 && "Parent had already existed!");
  Parent = N;
}

VASTNode *VASTSelector::getParent() const {
  assert(Parent && "Unexpected null parent!");
  return Parent;
}

VASTSeqValue *VASTSelector::getSSAValue() const {
  assert(num_defs() == 1 && "Not single assignment!");
  return *Defs.begin();
}

void VASTSelector::addUser(VASTSeqValue *V) {
  assert(!Defs.count(V) && "User existed!");
  Defs.insert(V);
}

void VASTSelector::removeUser(VASTSeqValue *V) {
  bool erased = Defs.erase(V);
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

void VASTSeqValue::dumpFaninns() const {
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
  if (Selector) {
    Contents.Name = Selector->getName();
    Selector->addUser(this);
  }
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
  : VASTNode(vastRegister), InitVal(InitVal), Sel(Sel) {
  Sel->setParent(this);
}

VASTRegister::VASTRegister() : VASTNode(vastRegister), InitVal(0), Sel(0) {}

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
