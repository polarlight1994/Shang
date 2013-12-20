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

#include "vast/VASTExprBuilder.h"
#include "vast/VASTSeqValue.h"
#include "vast/VASTSlot.h"
#include "vast/VASTModule.h"
#include "vast/STGDistances.h"
#include "vast/Strash.h"

#include "llvm/ADT/SmallString.h"
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

//----------------------------------------------------------------------------//
VASTSelector::VASTSelector(const char *Name, unsigned BitWidth, Type T,
                           VASTNode *Node)
  : VASTNode(vastSelector), Parent(Node), BitWidth(BitWidth), T(T), Guard(this),
    Fanin(this) {
  Contents64.Name = Name;
}

VASTSelector::~VASTSelector() {
  assert(Annotations.empty()
         && "Should explicitly release the annotations before deleting selectors!");
}

bool VASTSelector::isTrivialFannin(const VASTLatch &L) const {
  VASTValPtr FIVal = L;

  // Ignore the trivial loops.
  if (VASTSeqValue *V = dyn_cast<VASTSeqValue>(FIVal))
    if (V->getSelector() == this && IgnoreTrivialLoops)
      return true;

  // Ignore the X values.
  if (VASTWrapper *W = dyn_cast<VASTWrapper>(FIVal.get()))
    if (W->isX() && IgnoreXFanins)
      return true;

  return false;
}

unsigned VASTSelector::numNonTrivialFanins() const {
  unsigned Count = 0;

  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTLatch &L = *I;

    // Ignore the trivial fanins.
    if (isTrivialFannin(L))
      continue;

    ++Count;
  }

  return Count;
}

VASTLatch VASTSelector::getUniqueFannin() const {
  assert(numNonTrivialFanins() == 1 && "There is no unique fanin!");

  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTLatch &L = *I;

    // Ignore the trivial fanins.
    if (isTrivialFannin(L))
      continue;

    return L;
  }

  llvm_unreachable("Should had returned the unique fanin!");
  return VASTLatch();
}

namespace {
// The VASTSeqValues from the same VASTSelector are not equal in the data flow,
// because their are representing the value of the same selector at different
// states of the circuit. However, they are structural equal because their are
// driven by the same register. Use this functor to avoid the redundant nodes
// in the netlist.
struct StructualLess : public std::binary_function<VASTValPtr, VASTValPtr, bool> {
  static const char *GetValName(VASTValue *V) {
    if (VASTNamedValue *NV = dyn_cast<VASTNamedValue>(V))
      return NV->getName();

    return NULL;
  }

  static unsigned GetNameID(VASTValue *V) {
    if (VASTExpr *E = dyn_cast<VASTExpr>(V))
      return E->getNameID();

    return 0;
  }

  static const VASTSelector *GetSelector(VASTValue *V) {
    if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(V))
      return SV->getSelector();

    return NULL;
  }

  bool operator()(VASTValPtr LHS, VASTValPtr RHS) const {
    if (LHS && RHS && LHS.isInverted() == RHS.isInverted()) {
      unsigned LHSNameID = GetNameID(LHS.get()),
               RHSNameID = GetNameID(RHS.get());
      if (LHSNameID && RHSNameID)
        return LHSNameID < RHSNameID;

      const char *LHSName = GetValName(LHS.get()),
                 *RHSName = GetValName(RHS.get());
      if (LHSName && RHSName)
        return LHSName < RHSName;

      const VASTSelector *LHSSel = GetSelector(LHS.get()),
                         *RHSSel = GetSelector(RHS.get());

      if (LHSSel && RHSSel)
        return LHSSel < RHSSel;
    }

    return LHS < RHS;
  }
};
}

void
VASTSelector::verifyHoldCycles(vlang_raw_ostream &OS, STGDistances *STGDist,
                               VASTValue *V, VASTSlot *ReadSlot) const {
  typedef std::set<VASTSeqValue*> SVSet;
  SVSet Srcs;

  OS << "// Verify timing of cone rooted on " << VASTValPtr(V) << "\n";
  // Get *all* source register of the cone rooted on SubExpr.
  V->extractSupportingSeqVal(Srcs, false /*Search across keep nodes!*/);

  if (Srcs.empty()) return;

  for (SVSet::iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    VASTSeqValue *Src = *I;
    if (!Src->getLLVMValue())
      continue;

    unsigned Interval = STGDist->getIntervalFromDef(Src, ReadSlot);

    // Ignore single cycle path and false paths.
    if (Interval == 1 || Interval == STGDistances::Inf) continue;

    OS << "/*\n";
    typedef VASTSeqValue::fanin_iterator iterator;
    for (iterator I = Src->fanin_begin(), E = Src->fanin_end(); I != E; ++I) {
      VASTLatch U = *I;
      U.Op->print(OS);
    }
    OS << "\n*/\n";

    OS.if_() << Src->getName() << "_hold_counter < " << (Interval - 1);
    OS._then();
    OS << "// read at slot: " << ReadSlot->SlotNum;
    if (BasicBlock *BB = ReadSlot->getParent())
      OS << ", " << BB->getName();
    OS << "\n";

    OS << "$display(\"Hold violation on " << Src->getName() << " at"
          " slot: " << ReadSlot->SlotNum;
    if (BasicBlock *BB = ReadSlot->getParent())
      OS << ", " << BB->getName();
    OS << " written at slot %d read by " << getName()
        << "; expected hold cycle:" << Interval
        << " actual hold cycle: %d\", "
        << Src->getName() << "_last_assigned_slot, "
        << Src->getName() << "_hold_counter + 1);\n";
    OS << "$finish(1);\n";
    OS.exit_block();
  }

  OS << '\n';
}

void
VASTSelector::initTraceDataBase(raw_ostream &OS, const char *TraceDataBase) {
  OS << "$fwrite (" << TraceDataBase << ", \"";
  OS.write_escaped("CREATE TABLE InstTrace("
                   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                   "  ActiveTime INTEGER,"
                   "  Instruction TEXT,"
                   "  Opcode TEXT,"
                   "  BB TEXT,"
                   "  OperandValue INTEGER,"
                   "  RegisterName Text,"
                   "  SlotNum INTEGER"
                   ");\n");
  OS.write_escaped("CREATE TABLE SlotTrace("
                   "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                   "  ActiveTime INTEGER,"
                   "  BB TEXT,"
                   "  SlotNum INTEGER"
                   ");\n");
  OS << "\");\n";
}

void VASTSelector::dumpSlotTrace(vlang_raw_ostream &OS, const VASTSeqOp *Op,
                                 const char *TraceDataBase) const {
  VASTSlot *S = Op->getSlot();

  OS << "$fwrite (" << TraceDataBase << ", \"";
  OS.write_escaped("INSERT INTO SlotTrace(ActiveTime, BB, SlotNum) VALUES(");
  // Time
  OS.write_escaped("%t, ");

  // BB
  OS.write_escaped("\"");
  if (BasicBlock *BB = dyn_cast_or_null<BasicBlock>(getSSAValue()->getLLVMValue()))
    OS.write_escaped(BB->getName());
  else
    OS.write_escaped("entry/exit");
  OS.write_escaped("\", ");

  // SlotNum
  OS << S->SlotNum;

  OS.write_escaped(");\n");

  OS << "\", $time());\n";
}

void VASTSelector::dumpInstTrace(vlang_raw_ostream &OS, const VASTSeqOp *Op,
                                 const VASTLatch &L, const Instruction *Inst,
                                 const char *TraceDataBase) const {
  VASTSlot *S = Op->getSlot();

  OS << "$fwrite (" << TraceDataBase << ", \"";
  OS.write_escaped("INSERT INTO InstTrace("
                   "ActiveTime, Instruction, Opcode, BB, OperandValue, "
                   "RegisterName, SlotNum) VALUES(");
  // Time
  OS.write_escaped("%t, ");

  // Instruction
  OS.write_escaped("\"%s\", ");

  // Opcode
  OS.write_escaped("\"");
  OS << Inst->getOpcodeName();
  OS.write_escaped("\", ");

  // Parent slot
  OS.write_escaped("\"");
  if (BasicBlock *BB = S->getParent())
    OS.write_escaped(BB->getName());
  else
    OS.write_escaped("n/a");
  OS.write_escaped("\", ");

  // Current Operand value
  OS.write_escaped("%d, ");

  // Register Name
  OS.write_escaped("\"");
  // The name of slot register depends on the schedule, simply generate the
  // same name for all slot register to avoid the difference in trace only
  // because of different schedule.
  if (isSlot())
    OS << "Slot Register";
  else
    OS << getName();
  OS.write_escaped("\", ");

  // SlotNum
  OS << S->SlotNum;
  OS.write_escaped(");\n");

  OS << "\", $time(), ";
  SmallString<64> InstStr;
  raw_svector_ostream SS(InstStr);
  SS << *Inst;

  OS << '"';
  OS.write_escaped(SS.str());
  OS << "\", ";
  VASTValPtr(L).printAsOperand(OS);
  OS << ");\n";
}

void
VASTSelector::dumpTrace(vlang_raw_ostream &OS, const VASTSeqOp *Op,
                        const VASTLatch &L, const char *TraceDataBase) const {
  if (isSlot())
    dumpSlotTrace(OS, Op, TraceDataBase);

  if (Instruction *Inst = dyn_cast_or_null<Instruction>(Op->getValue()))
    dumpInstTrace(OS, Op, L, Inst, TraceDataBase);
}

void
VASTSelector::printVerificationCode(vlang_raw_ostream &OS, STGDistances *STGDist,
                                    const char *TraceDataBase) const {
  if (empty()) return;

  OS.if_begin("!rstN");

  // Reset the hold counter when the register is reset.
  OS << getName() << "_hold_counter <= " << STGDistances::Inf << ";\n";
  OS << getName() << "_last_assigned_slot <= " << STGDistances::Inf << ";\n";

  OS.else_begin();

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
        << getName() << " has more than one active assignment: %b!\", $time(), "
        << AllCnd << ");\n";

  SmallVector<VASTSlot*, 8> AllSlots;

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
    AllSlots.push_back(S);

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

  // Reset the hold counter when the register is changed.
  OS << getName() << "_hold_counter <= " << getName() << "_selector_guard"
     << " ? 0 : (" << getName() << "_hold_counter + 1);\n";

  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTLatch &L = *I;

    if (isTrivialFannin(L))
      continue;

    const VASTSeqOp *Op = L.Op;
    OS.if_();
    Op->printGuard(OS);
    // Be careful of operations that is not guarded by slot, their guard can
    // set even in the slot that their are not scheduled to. These cases are
    // usually introduced by MUX pipelining, etc, which assign to the same
    // register with the same guard and same fanin in different slots,
    // it is save to the behavior of the circuit. But it introduces incorrect
    // instruction trace, hence we need to guard it by the slot register.
    if (!Op->guardedBySlotActive()) {
      OS << " & ";
      Op->getSlot()->getValue()->printAsOperand(OS, false);
    }

    OS._then();

    OS << getName() << "_last_assigned_slot <= " << L.getSlotNum() << ";\n";

    verifyHoldCycles(OS, STGDist, VASTValPtr(L).get(), L.getSlot());
    verifyHoldCycles(OS, STGDist, VASTValPtr(L.getGuard()).get(), L.getSlot());

    if (TraceDataBase)
      dumpTrace(OS, Op, L, TraceDataBase);

    OS.exit_block();
  }

  OS.exit_block();
}

void VASTSelector::addAssignment(VASTSeqOp *Op, unsigned SrcNo) {
  VASTLatch L = VASTLatch(Op, SrcNo);
  assert(L->getBitWidth() == getBitWidth()
         && "Bitwidth not matched in assignment!");
  Assigns.push_back(L);
}

void VASTSelector::printSelectorModule(raw_ostream &O) const {
  if (empty() || isSelectorSynthesized()) return;

  vlang_raw_ostream OS(O);

  OS << "module shang_" << getName() << "_selector(";
  OS.module_begin();
  // Print the input ports.
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTLatch &L = *I;

    // Ignore the trivial fanins.
    if (isTrivialFannin(L))
      continue;

    OS << "input wire" << VASTValue::BitRange(getBitWidth())
       << " slot" << L.getSlotNum() << "fi,\n";
    OS << "input wire slot" << L.getSlotNum() << "guard,\n";
  }

  OS << "output wire" << VASTValue::BitRange(getBitWidth()) << " fo,\n"
        "output wire guard);\n\n";

  // Build the enable.
  OS << "assign guard = 1'b0";
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTLatch &L = *I;

    // Ignore the trivial fanins.
    if (isTrivialFannin(L))
      continue;

    OS << " | slot" << L.getSlotNum() << "guard";
  }
  OS << ";\n";

  // Build the data output.
  OS << "assign fo = (" << getBitWidth() << "'b0)";
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTLatch &L = *I;

    // Ignore the trivial fanins.
    if (isTrivialFannin(L))
      continue;

    OS << "| slot" << L.getSlotNum() << "fi";
  }

  OS << ";\n";

  OS.module_end();
  OS << '\n';
  OS.flush();
}

void VASTSelector::instantiateSelector(raw_ostream &OS) const {
  if (empty()) return;

  // Create the temporary signal.
  OS << "// Combinational MUX\n";

  OS << "wire " << VASTValue::BitRange(getBitWidth(), 0, false)
     << ' ' << getName() << "_selector_wire;\n";

  OS << "wire " << ' ' << getName() << "_selector_guard;\n\n";

  OS << "shang_" << getName() << "_selector " << getName() << "_selector(";
  // Print the inputs of the mux.
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    const VASTLatch &L = *I;
    // Ignore the trivial fanins.
    if (isTrivialFannin(L))
      continue;
    // {" << getBitWidth() << "{slot" << L.getSlotNum() << "guard}} &"
    // " slot" << L.getSlotNum() << "fi

    SmallString<64> Guard;
    {
      raw_svector_ostream SS(Guard);
      SS << '(' << VASTValPtr(L.getGuard());
      if (VASTValPtr V = L.getSlotActive())
        SS << " & " << V << ')';
    }

    // Print the fanin.
    OS << ".slot" << L.getSlotNum() << "fi("
       << "{" << getBitWidth() << "{" << Guard << "}} &" << VASTValPtr(L)
       << "), .slot" << L.getSlotNum() << "guard(" << Guard << "),\n";
  }

  OS << ".fo(" << getName() << "_selector_wire),\n"
     << ".guard(" << getName() << "_selector_guard));\n";
}

void VASTSelector::printSelector(raw_ostream &OS) const {
  if (empty())
    return;

  if (!isSelectorSynthesized()) {
    instantiateSelector(OS);
    return;
  }

  OS << "// Synthesized MUX\n";

  OS << "wire " << getName() << "_selector_guard = "
     << VASTValPtr(Guard) << ";\n";

  if (isEnable() || isSlot()) return;

  // Print (or implement) the MUX by:
  // output = (Sel0 & FANNIN0) | (Sel1 & FANNIN1) ...
  OS << "wire " << VASTValue::BitRange(getBitWidth(), 0, false)
     << ' ' << getName() << "_selector_wire = " << VASTValPtr(Fanin) << ";\n";
}

void VASTSelector::printRegisterBlock(vlang_raw_ostream &OS,
                                      uint64_t InitVal) const {

  if (empty()) {
    // Print the driver of the output ports.
    if (isa<VASTOutPort>(getParent())) {
      OS.always_ff_begin();
      OS << getName()  << " <= "
         << VASTConstant::buildLiteral(InitVal, getBitWidth(), false) << ";\n";
      OS.always_ff_end();
    }

    return;
  }

  // Print the data selector of the register.
  printSelector(OS);

  // Generate the hold counter to verify the multi-cycle analysis.
  OS << "// synthesis translate_off\n";
  OS << "int unsigned " << getName() << "_hold_counter;\n";
  OS << "int unsigned " << getName() << "_last_assigned_slot;\n";
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_begin();
  // Reset the register.
  OS << getName()  << " <= "
     << VASTConstant::buildLiteral(InitVal, getBitWidth(), false) << ";\n";

  OS.else_begin();

  // Print the assignment.
  if (isEnable() || isSlot()) {
    OS << getName() << " <= " << getName() << "_selector_guard" << ";\n";
  } else {
    OS.if_begin(Twine(getName()) + Twine("_selector_guard"));
    OS << getName() << " <= " << getName() << "_selector_wire"
       << VASTValue::BitRange(getBitWidth(), 0, false) << ";\n";
    OS.exit_block();
  }

  OS.always_ff_end();
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

void VASTSelector::annotateReadSlot(VASTSlot *S, VASTValPtr V)  {
  VASTExpr *Expr = dyn_cast<VASTExpr>(V.get());
  if (!Expr)
    return;

  if (Expr->isTimingBarrier()) {
    Annotations[Expr].push_back(S);
    return;
  }

  // The timing barrier maybe decomposed by bit-level optimization, preform
  // depth first search to annotate them.
  // The expression tree may contains keep nodes with other annotations. If we
  // generate the keep nodes correctly, the newly generated keep nodes are
  // supposed to shield the earlier keep nodes.
  typedef VASTOperandList::op_iterator ChildIt;
  std::vector<std::pair<VASTExpr*, ChildIt> > VisitStack;
  std::set<VASTExpr*> Visited;

  VisitStack.push_back(std::make_pair(Expr, Expr->op_begin()));

  while (!VisitStack.empty()) {
    VASTExpr *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == Node->op_end()) {
      VisitStack.pop_back();
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTExpr *ChildExpr = dyn_cast<VASTExpr>(ChildNode)) {
      // ChildNode has a name means we had already visited it.
      if (!Visited.insert(ChildExpr).second) continue;

      if (ChildExpr->isTimingBarrier()) {
        Annotations[ChildExpr].push_back(S);
        continue;
      }

      VisitStack.push_back(std::make_pair(ChildExpr, ChildExpr->op_begin()));
    }
  }
}

void VASTSelector::dropMux() {
  Annotations.clear();
  Fanin.reset();
  Guard.reset();
}

void VASTSelector::setMux(VASTValPtr Fanin, VASTValPtr Guard) {
  this->Fanin.set(Fanin);
  this->Guard.set(Guard);
}

void VASTSelector::setName(const char *Name) {
  if (Contents64.Name == Name)
    return;

  // Change the name of the selector.
  Contents64.Name = Name;

  for (def_iterator I = def_begin(), E = def_end(); I != E; ++I) {
    VASTSeqValue *SV = *I;

    if (SV->getName() != getName())
      SV->changeSelector(this);
  }
}

//===----------------------------------------------------------------------===//
VASTSeqValue::VASTSeqValue(VASTSelector *Selector, unsigned Idx, Value *V)
  : VASTNamedValue(vastSeqValue, Selector->getName(), Selector->getBitWidth()),
    Selector(Selector), V(V), Idx(Idx) {
  Selector->addUser(this);
}

void VASTSeqValue::printFanins(raw_ostream &OS) const {
  typedef VASTSeqValue::const_fanin_iterator iterator;

  for (iterator I = fanin_begin(), E = fanin_end(); I != E; ++I) {
    VASTLatch U = *I;
    U.Op->print(OS);
  }
}

void VASTSeqValue::dumpFanins() const {
  printFanins(dbgs());
}

VASTSelector *VASTSeqValue::getSelector() const {
  assert(Selector && "Unexpected null selector!");
  return Selector;
}

void VASTSeqValue::changeSelector(VASTSelector *NewSel) {
  if (NewSel == getSelector()) {
    assert(Contents64.Name != Selector->getName() && "Selector not changed!");
    Contents64.Name = Selector->getName();
    return;
  }

  getSelector()->removeUser(this);
  Selector = NewSel;
  if (Selector) {
    Contents64.Name = Selector->getName();
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

const VASTLatch &VASTSeqValue::getUniqueFanin() const {
  assert(num_fanins() == 1
         && "Cannot call getUniqueFanin on VASTSeqValue without unique fanin!");
  return *fanin_begin();
}

//===----------------------------------------------------------------------===//
VASTRegister::VASTRegister(VASTSelector *Sel, uint64_t InitVal)
  : VASTNode(vastRegister), InitVal(InitVal), Sel(Sel) {
  Sel->setParent(this);
}

VASTRegister::VASTRegister() : VASTNode(vastRegister), InitVal(0), Sel(0) {}

void VASTRegister::print(vlang_raw_ostream &OS) const {
  Sel->printRegisterBlock(OS, InitVal);
}

void VASTRegister::print(raw_ostream &OS) const {
  vlang_raw_ostream S(dbgs());
  print(S);
}

void VASTRegister::printDecl(raw_ostream &OS) const {
  VASTNamedValue::PrintDecl(OS, Sel->getName(), Sel->getBitWidth(), true, "");
  OS << " = " << VASTConstant::buildLiteral(InitVal, Sel->getBitWidth(), false)
     <<  ";\n";
}
