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
#include "shang/STGDistances.h"
#include "shang/Strash.h"

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
  : VASTNode(vastSelector), Parent(Node), BitWidth(BitWidth), T(T),
    PrintSelModule(false), Guard(this), Fanin(this) {
  Contents.Name = Name;
}

VASTSelector::~VASTSelector() {
  assert(Annotations.empty()
         && "Should explicitly release the annotations before deleting selectors!");
}


static const VASTSelector *getSelector(VASTValue *V) {
  if (VASTSeqValue *SV = dyn_cast<VASTSeqValue>(V))
    return SV->getSelector();

  return 0;
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

namespace {
// The VASTSeqValues from the same VASTSelector are not equal in the data flow,
// because their are representing the value of the same selector at different
// states of the circuit. However, they are structural equal because their are
// driven by the same register. Use this functor to avoid the redundant nodes
// in the netlist.
struct StructualLess : public std::binary_function<VASTValPtr, VASTValPtr, bool> {
  static const char *getValName(VASTValue *V) {
    if (VASTExpr *E = dyn_cast<VASTExpr>(V))
      return E->getTempName();

    if (VASTNamedValue *NV = dyn_cast<VASTNamedValue>(V))
      return NV->getName();

    return 0;
  }

  bool operator()(VASTValPtr LHS, VASTValPtr RHS) const {
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
};
}

void
VASTSelector::verifyHoldCycles(vlang_raw_ostream &OS, STGDistances *STGDist,
                               VASTValue *V, ArrayRef<VASTSlot*> ReadSlots) const {
  typedef std::set<VASTSeqValue*> SVSet;
  SVSet Srcs;

  OS << "// Verify timing of cone rooted on " << VASTValPtr(V) << "\n";
  // Get *all* source register of the cone rooted on SubExpr.
  V->extractSupportingSeqVal(Srcs, false /*Search across keep nodes!*/);

  if (Srcs.empty()) return;

  OS.if_() << "1'b0 ";
  for (unsigned i = 0; i < ReadSlots.size(); ++i) {
    VASTSlot *ReadSlot = ReadSlots[i];
    OS << "| (" << VASTValPtr(ReadSlot->getGuard())
       << " & " << ReadSlot->getValue()->getName() << ") ";
  }

  OS._then();

  std::set<VASTSelector*> VisitedSelectors;
  for (SVSet::iterator I = Srcs.begin(), E = Srcs.end(); I != E; ++I) {
    VASTSeqValue *Src = *I;

    if (!VisitedSelectors.insert(Src->getSelector()).second)
      continue;

    unsigned Interval = STGDist->getIntervalFromDef(Src->getSelector(), ReadSlots);

    // Ignore single cycle path and false paths.
    if (Interval == 1) continue;

    OS << "/*\n";
    typedef VASTSeqValue::fanin_iterator iterator;
    for (iterator I = Src->fanin_begin(), E = Src->fanin_end(); I != E; ++I) {
      VASTLatch U = *I;
      U.Op->print(OS);
    }
    OS << "\n*/\n";

    OS.if_() << Src->getName() << "_hold_counter < " << (Interval - 1);
    OS._then();
    for (unsigned i = 0; i < ReadSlots.size(); ++i) {
      VASTSlot *ReadSlot = ReadSlots[i];
      OS << "// read at slot: " << ReadSlot->SlotNum;
      if (BasicBlock *BB = ReadSlot->getParent())
        OS << ", " << BB->getName();
      OS << "\n";

      OS.if_() << VASTValPtr(ReadSlot->getGuard())
               << " & " << ReadSlot->getValue()->getName();
      OS._then();

      OS << "$display(\"Hold violation on " << Src->getName() << " at"
            " slot: " << ReadSlot->SlotNum;
      if (BasicBlock *BB = ReadSlot->getParent())
        OS << ", " << BB->getName();
      OS << " read by " << getName() << "; expected hold cycle:" << Interval
          << " actual hold cycle: %d\", "
          << Src->getName() << "_hold_counter + 1);\n";
      OS << "$finish(1);\n";
      OS.exit_block();
    }
    OS.exit_block();
  }

  OS.exit_block();

  OS << '\n';
}

void VASTSelector::printVerificationCode(vlang_raw_ostream &OS,
                                         STGDistances *STGDist) const {
  if (empty()) return;

  OS.always_ff_begin();

  // Reset the hold counter when the register is reset.
  OS << getName() << "_hold_counter <= " << STGDistances::Inf << ";\n";

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

  //verifyHoldCycles(OS, STGDist, getGuard().get(), AllSlots);

  //if (!isEnable() && !isSlot())
  //  verifyHoldCycles(OS, STGDist, getFanin().get(), AllSlots);

  //for (ann_iterator I = ann_begin(), E = ann_end(); I != E; ++I)
  //  verifyHoldCycles(OS, STGDist, VASTValPtr(I->first).get(), I->second);

  // Reset the hold counter when the register is changed.
  OS << getName() << "_hold_counter <= " << getName() << "_selector_guard"
     << " ? 0 : (" << getName() << "_hold_counter + 1);\n";
  OS.always_ff_end();
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
  }

  OS << "output wire" << VASTValue::printBitRange(getBitWidth()) << " fo,\n"
        "output wire guard);\n\n";

  // Build the enalbe.
  OS << "assign guard = guard0";
  for (unsigned i = 1, e = size(); i < e; ++i)
    OS << " | guard" << i;
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
  }

  OS << getName() << "_selector_wire,\n"
     << getName() << "_selector_guard);\n";
}

void VASTSelector::printSelector(raw_ostream &OS) const {
  if (empty()) return;

  if (forcePrintSelModule()) {
    instantiateSelector(OS);
    return;
  }

  OS << "// Synthesized MUX\n";

  OS << "wire " << getName() << "_selector_guard = "
     << VASTValPtr(Guard) << ";\n";

  if (isEnable() || isSlot()) return;

  // Print (or implement) the MUX by:
  // output = (Sel0 & FANNIN0) | (Sel1 & FANNIN1) ...
  OS << "wire " << VASTValue::printBitRange(getBitWidth(), 0, false)
     << ' ' << getName() << "_selector_wire = " << VASTValPtr(Fanin) << ";\n";
}

void VASTSelector::printRegisterBlock(vlang_raw_ostream &OS,
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

  // Generate the hold counter to verify the multi-cycle analysis.
  OS << "// synthesis translate_off\n";
  OS << "int unsigned " << getName() << "_hold_counter;\n";
  OS << "// synthesis translate_on\n\n";

  OS.always_ff_begin();
  // Reset the register.
  OS << getName()  << " <= "
     << VASTImmediate::buildLiteral(InitVal, getBitWidth(), false) << ";\n";

  OS.else_begin();

  // Print the assignment.
  if (isEnable() || isSlot()) {
    OS << getName() << " <= " << getName() << "_selector_guard" << ";\n";
  } else {
    OS.if_begin(Twine(getName()) + Twine("_selector_guard"));
    OS << getName() << " <= " << getName() << "_selector_wire"
       << VASTValue::printBitRange(getBitWidth(), 0, false) << ";\n";
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
  Annotations[V.get()].push_back(S);
}

void VASTSelector::buildMux(VASTExprBuilder &Builder, CachedStrashTable &CST) {
  typedef std::vector<VASTLatch> OrVec;
  typedef std::map<unsigned, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator iterator;

  CSEMapTy CSEMap;

  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    VASTLatch U = *I;

    if (isTrivialFannin(U)) continue;

    // Index the input of the assignment based on the strash id. By doing this
    // we can pack the structural identical inputs together.
    CSEMap[CST.getOrCreateStrashID(VASTValPtr(U))].push_back(U);
  }

  if (CSEMap.empty()) return;

  unsigned Bitwidth = getBitWidth();

  SmallVector<VASTValPtr, 4> SlotGuards, SlotFanins, FaninGuards, Fanins;
  SmallVector<VASTSlot*, 4> Slots;

  for (iterator I = CSEMap.begin(), E = CSEMap.end(); I != E; ++I) {
    SlotGuards.clear();
    SlotFanins.clear();

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI) {
      SmallVector<VASTValPtr, 2> CurGuards;
      const VASTLatch &L = *OI;
      SlotFanins.push_back(L);
      VASTSlot *S = L.getSlot();

      if (VASTValPtr SlotActive = L.getSlotActive())
        CurGuards.push_back(SlotActive);

      CurGuards.push_back(L.getGuard());
      VASTValPtr CurGuard = Builder.buildAndExpr(CurGuards, 1);

      // Simply keep all guarding condition, because they are only 1 bit nodes,
      // and their upper bound is the product of number of slots and number of
      // basic blocks.
      CurGuard = Builder.buildKeep(CurGuard);

      // The guarding condition itself is not guard, that is, the guarding
      // condition is read whenever the slot register is set. Hence, we should
      // annotate it with the control-equivalent group instead of the guarding
      // condition equivalent group!
      // FIXME: We can build apply the keep attribute according to the STG
      // subgroup hierarchy sequentially to relax the constraints.
      annotateReadSlot(S->getParentState(), CurGuard);
      SlotGuards.push_back(CurGuard);
      Slots.push_back(S);
    }

    VASTValPtr FIGuard = Builder.buildOrExpr(SlotGuards, 1);
    FaninGuards.push_back(FIGuard);

    VASTValPtr FIMask = Builder.buildBitRepeat(FIGuard, Bitwidth);
    VASTValPtr FIVal = Builder.buildAndExpr(SlotFanins, Bitwidth);
    VASTValPtr GuardedFIVal = Builder.buildAndExpr(FIVal, FIMask, Bitwidth);
    GuardedFIVal = Builder.buildKeep(GuardedFIVal);
    Fanins.push_back(GuardedFIVal);
    while (!Slots.empty())
      annotateReadSlot(Slots.pop_back_val(), GuardedFIVal);
  }

  // Strip the keep attribute if the keeped value is directly fan into the
  // register.
  Guard.set(Builder.buildOrExpr(FaninGuards, 1));
  Fanin.set(Builder.buildOrExpr(Fanins, Bitwidth));
}

  Annotations.clear();
}

//===----------------------------------------------------------------------===//
VASTSeqValue::VASTSeqValue(VASTSelector *Selector, unsigned Idx, Value *V)
  : VASTNamedValue(vastSeqValue, Selector->getName(), Selector->getBitWidth()),
    Selector(Selector), V(V), Idx(Idx) {
  Selector->addUser(this);
}

void VASTSeqValue::printFaninns(raw_ostream &OS) const {
  typedef VASTSeqValue::const_fanin_iterator iterator;

  for (iterator I = fanin_begin(), E = fanin_end(); I != E; ++I) {
    VASTLatch U = *I;
    U.Op->print(OS);
  }
}

void VASTSeqValue::dumpFaninns() const {
  printFaninns(dbgs());
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

void VASTRegister::print(vlang_raw_ostream &OS) const {
  Sel->printRegisterBlock(OS, InitVal);
}

void VASTRegister::print(raw_ostream &OS) const {
  vlang_raw_ostream S(dbgs());
  print(S);
}

void VASTRegister::printDecl(raw_ostream &OS) const {
  VASTNamedValue::PrintDecl(OS, Sel->getName(), Sel->getBitWidth(), true, "");
  OS << " = " << VASTImmediate::buildLiteral(InitVal, Sel->getBitWidth(), false)
     <<  ";\n";
}
