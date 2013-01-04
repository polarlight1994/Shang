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
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vtm-ctrl-logic-builder"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<bool>
EnableBBProfile("vtm-enable-bb-profile",
                cl::desc("Generate counters to profile the design"),
                cl::init(false));

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

MachineInstr *VASTSlot::getBundleStart() const {
  return Contents.BundleStart;
}

MachineBasicBlock *VASTSlot::getParentBB() const {
  if (MachineInstr *BundleStart = getBundleStart())
    return BundleStart->getParent();

  return 0;
}

void VASTSlot::addSuccSlot(VASTSlot *NextSlot, VASTValPtr Cnd, VASTModule *VM) {
  VASTUse *&U = NextSlots[NextSlot];
  assert(U == 0 && "Succ Slot already existed!");
  NextSlot->PredSlots.push_back(this);
  U = new (VM->allocateUse()) VASTUse(this, Cnd);
}

VASTUse &VASTSlot::allocateEnable(VASTSeqValue *P, VASTModule *VM) {
  VASTUse *&U = Enables[P];
  if (U == 0) U = new (VM->allocateUse()) VASTUse(this, 0);

  return *U;
}

VASTUse &VASTSlot::allocateReady(VASTValue *V, VASTModule *VM) {
  VASTUse *&U = Readys[V];
  if (U == 0) U = new (VM->allocateUse()) VASTUse(this, 0);

  return *U;
}

VASTUse &VASTSlot::allocateDisable(VASTSeqValue *P, VASTModule *VM) {
  VASTUse *&U = Disables[P];
  if (U == 0) U = new (VM->allocateUse()) VASTUse(this, 0);

  return *U;
}

VASTUse &VASTSlot::allocateSuccSlot(VASTSlot *NextSlot, VASTModule *VM) {
  VASTUse *&U = NextSlots[NextSlot];
  if (U == 0) {
    U = new (VM->allocateUse()) VASTUse(this, 0);
    NextSlot->PredSlots.push_back(this);
  }

  return *U;
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
  llvm_unreachable("VASTSlot::print should not be called!");
}

VASTValPtr VASTSlot::buildFUReadyExpr(VASTExprBuilder &Builder) {
  SmallVector<VASTValPtr, 4> Ops;

  for (VASTSlot::const_fu_rdy_it I = ready_begin(), E = ready_end();I != E; ++I) {
    // If the condition is true then the signal must be 1 to ready.
    VASTValPtr ReadyCnd = Builder.buildNotExpr(I->second->getAsInlineOperand());
    Ops.push_back(Builder.buildOrExpr(I->first, ReadyCnd, 1));
  }

  // No waiting signal means always ready.
  if (Ops.empty()) return Builder.getBoolImmediate(true);

  return Builder.buildAndExpr(Ops, 1);
}

void VASTSlot::buildReadyLogic(VASTModule &Mod, VASTExprBuilder &Builder) {
  SmallVector<VASTValPtr, 4> Ops;
  // FU ready for current slot.
  Ops.push_back(buildFUReadyExpr(Builder));

  if (hasAliasSlot()) {
    for (unsigned s = alias_start(), e = alias_end(), ii = alias_ii();
         s < e; s += ii) {
      if (s == SlotNum) continue;

      VASTSlot *AliasSlot = Mod.getSlot(s);

      if (AliasSlot->readyEmpty()) continue;

      // FU ready for alias slot, when alias slot register is 1, its waiting
      // signal must be 1.
      VASTValPtr AliasReady = AliasSlot->buildFUReadyExpr(Builder);
      VASTValPtr AliasDisactive = Builder.buildNotExpr(AliasSlot->getValue());
      Ops.push_back(Builder.buildOrExpr(AliasDisactive, AliasReady, 1));
    }
  }

  // All signals should be 1 before the slot is ready.
  VASTValPtr ReadyExpr = Builder.buildAndExpr(Ops, 1);
  Mod.assign(cast<VASTWire>(getReady()), ReadyExpr);
  // The slot is activated when the slot is enable and all waiting signal is
  // ready.
  Mod.assign(cast<VASTWire>(getActive()),
             Builder.buildAndExpr(SlotReg, ReadyExpr, 1));
}

void VASTSlot::buildCtrlLogic(VASTModule &Mod, VASTExprBuilder &Builder) {
  vlang_raw_ostream &CtrlS = Mod.getControlBlockBuffer();
  // TODO: Build the AST for these logic.
  CtrlS.if_begin(getName());
  bool ReadyPresented = !readyEmpty();

  // DirtyHack: Remember the enabled signals in alias slots, the signal may be
  // assigned at a alias slot.
  std::set<const VASTValue *> AliasEnables;
  // A slot may be enable by its alias slot if II of a pipelined loop is 1.
  VASTValPtr PredAliasSlots = 0;

  if (hasAliasSlot()) {
    CtrlS << "// Alias slots: ";

    for (unsigned s = alias_start(), e = alias_end(), ii = alias_ii();
         s < e; s += ii) {
      CtrlS << s << ", ";
      if (s == SlotNum) continue;

      const VASTSlot *AliasSlot = Mod.getSlot(s);
      if (AliasSlot->hasNextSlot(this)) {
        assert(!PredAliasSlots
               && "More than one PredAliasSlots found!");
        PredAliasSlots = AliasSlot->getActive();
      }

      for (VASTSlot::const_fu_ctrl_it I = AliasSlot->enable_begin(),
           E = AliasSlot->enable_end(); I != E; ++I) {
        bool inserted = AliasEnables.insert(I->first).second;
        assert(inserted && "The same signal is enabled twice!");
        (void) inserted;
      }

      ReadyPresented  |= !AliasSlot->readyEmpty();
    }

    CtrlS << '\n';
  } // SS flushes automatically here.

  DEBUG_WITH_TYPE("vtm-codegen-self-verify",
  if (SlotNum != 0) {
    CtrlS << "$display(\"" << getName() << " in " << Mod.getName() << " BB#"
          << getParentBB()->getNumber() << ' ';
    if (const BasicBlock *BB = getParentBB()->getBasicBlock())
      CtrlS << BB->getName();
    CtrlS << " ready at %d\", $time());\n";
  });

  bool hasSelfLoop = false;
  SmallVector<VASTValPtr, 2> EmptySlotEnCnd;

  assert(!NextSlots.empty() && "Expect at least 1 next slot!");
  CtrlS << "// Enable the successor slots.\n";
  for (VASTSlot::const_succ_cnd_iterator I = succ_cnd_begin(),E = succ_cnd_end();
        I != E; ++I) {
    hasSelfLoop |= I->first->SlotNum == SlotNum;
    VASTSeqValue *NextSlotReg = I->first->getValue();
    Mod.addAssignment(NextSlotReg, *I->second, this, EmptySlotEnCnd);
  }

  assert(!(hasSelfLoop && PredAliasSlots)
         && "Unexpected have self loop and pred alias slot at the same time.");
  // Do not assign a value to the current slot enable twice.
  if (!hasSelfLoop) {
    // Only disable the current slot if there is no alias slot enable current
    // slot.
    if (PredAliasSlots)
      EmptySlotEnCnd.push_back(Builder.buildNotExpr(PredAliasSlots));

    // Disable the current slot.
    Mod.addAssignment(getValue(), Mod.getBoolImmediateImpl(false), this,
                      EmptySlotEnCnd);
  }

  if (!ReadyPresented) {
    DEBUG_WITH_TYPE("vtm-codegen-self-verify",
    if (SlotNum != 0) {
      CtrlS << "if (start) begin $display(\"" << getName() << " in "
            << Mod.getName()
            << " bad start %b\\n\", start);  $finish(); end\n";

      CtrlS << "if (Slot0r) begin $display(\"" << getName() << " in "
            << Mod.getName()
            << " bad Slot0 %b\\n\", Slot0r);  $finish(); end\n";
    }

    CtrlS << "if (mem0en_r) begin $display(\"" << getName() << " in "
          << Mod.getName()
          << " bad mem0en_r %b\\n\", mem0en_r);  $finish(); end\n";
    );
  }

  std::string SlotReady = std::string(getName()) + "Ready";
  CtrlS << "// Enable the active FUs.\n";
  for (VASTSlot::const_fu_ctrl_it I = enable_begin(), E = enable_end();
       I != E; ++I) {
    assert(!AliasEnables.count(I->first) && "Signal enabled by alias slot!");
    // No need to wait for the slot ready.
    // We may try to enable and disable the same port at the same slot.
    EmptySlotEnCnd.clear();
    EmptySlotEnCnd.push_back(getValue());
    VASTValPtr ReadyCnd
      = Builder.buildAndExpr(getReady()->getAsInlineOperand(false),
                             I->second->getAsInlineOperand(), 1);
    Mod.addAssignment(I->first, ReadyCnd, this, EmptySlotEnCnd, 0, false);
  }

  SmallVector<VASTValPtr, 4> DisableAndCnds;
  if (!disableEmpty()) {
    CtrlS << "// Disable the resources when the condition is true.\n";
    for (VASTSlot::const_fu_ctrl_it I = disable_begin(), E = disable_end();
         I != E; ++I) {
      // Look at the current enable set and alias enables set;
      // The port assigned at the current slot, and it will be disabled if
      // The slot is not ready or the enable condition is false. And it is
      // ok that the port is enabled.
      if (isEnabled(I->first)) continue;

      DisableAndCnds.push_back(getValue());
      // If the port enabled in alias slots, disable it only if others slots is
      // not active.
      bool AliasEnabled = AliasEnables.count(I->first);
      if (AliasEnabled) {
        for (unsigned s = alias_start(), e = alias_end(), ii = alias_ii();
             s < e; s += ii) {
          if (s == SlotNum) continue;

          VASTSlot *ASlot = Mod.getSlot(s);
          assert(!ASlot->isDiabled(I->first)
                 && "Same signal disabled in alias slot!");
          if (ASlot->isEnabled(I->first)) {
            DisableAndCnds.push_back(Builder.buildNotExpr(ASlot->getValue()));
            continue;
          }
        }
      }

      DisableAndCnds.push_back(*I->second);

      VASTSeqValue *En = I->first;
      Mod.addAssignment(En, Mod.getBoolImmediateImpl(false), this,
                        DisableAndCnds, 0, false);
      DisableAndCnds.clear();
    }
  }
  CtrlS.exit_block("\n\n");
}

//===----------------------------------------------------------------------===//
void VASTModule::buildSlotLogic(VASTExprBuilder &Builder) {
  bool IsFirstSlotInBB = false;
  for (SlotVecTy::const_iterator I = Slots.begin(), E = Slots.end();I != E;++I){
    if (VASTSlot *S = *I) {
      S->buildCtrlLogic(*this, Builder);

      // Create a profile counter for each BB.
      if (EnableBBProfile) writeProfileCounters(S, IsFirstSlotInBB);
      IsFirstSlotInBB = false;
      continue;
    }

    // We meet an end slot, The next slot is the first slot in new BB
    IsFirstSlotInBB = true;
  }
}

//===----------------------------------------------------------------------===//

void VASTSeqValue::addAssignment(VASTUse *Src, VASTWire *AssignCnd) {
  assert(AssignCnd->getWireType() == VASTWire::AssignCond
         && "Expect wire for assign condition!");
  bool inserted = Assigns.insert(std::make_pair(AssignCnd, Src)).second;
  assert(inserted &&  "Assignment condition conflict detected!");
}

VASTRegister::VASTRegister(const char *Name, unsigned BitWidth,
                           uint64_t initVal, VASTNode::SeqValType T,
                           unsigned RegData,  const char *Attr)
  : VASTNode(vastRegister), Value(Name, BitWidth, T, RegData, *this),
    InitVal(initVal), AttrStr(Attr) {}

void VASTRegister::printCondition(raw_ostream &OS, const VASTSlot *Slot,
                                  const AndCndVec &Cnds) {
  OS << '(';
  if (Slot) {
    VASTValPtr Active = Slot->getActive();
    Active.printAsOperand(OS);
    if (VASTWire *S = Active.getAsLValue<VASTWire>()) S->Pin();
  } else      OS << "1'b1";

  typedef AndCndVec::const_iterator and_it;
  for (and_it CI = Cnds.begin(), CE = Cnds.end(); CI != CE; ++CI) {
    OS << " & ";
    CI->printAsOperand(OS);
    if (VASTWire *S = CI->getAsLValue<VASTWire>()) S->Pin();
  }

  OS << ')';
}

bool VASTRegister::printReset(raw_ostream &OS) const {
  if (num_assigns() == 0) return false;

  OS << getName()  << " <= "
     << VASTImmediate::buildLiteral(InitVal, getBitWidth(), false) << ";";
  return true;
}

static void PrintSelector(raw_ostream &OS, const Twine &Name, unsigned BitWidth,
                          const std::map<VASTValPtr, std::vector<VASTValPtr> > &
                          SrcCSEMap) {
  typedef std::vector<VASTValPtr> OrVec;
  typedef std::map<VASTValPtr, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator it;

  // Create the temporary signal.
  OS << "// Combinational MUX\n"
     << "reg " << VASTValue::printBitRange(BitWidth, 0, false)
     << ' ' << Name << "_selector_wire;\n"
     << "reg " << ' ' << Name << "_selector_enable = 0;\n\n";

  // If there is no fanin, leave the signal alone.
  if (SrcCSEMap.empty()) return;

  // Print the mux logic.
  OS << "always @(*)begin  // begin mux logic\n";
  OS.indent(2) << VASTModule::ParallelCaseAttr << " case (1'b1)\n";

  for (it I = SrcCSEMap.begin(), E = SrcCSEMap.end(); I != E; ++I) {
    OS.indent(4) << '(';

    const OrVec &Ors = I->second;
    for (OrVec::const_iterator OI = Ors.begin(), OE = Ors.end(); OI != OE; ++OI)
    {
      OI->printAsOperand(OS);
      OS << '|';
    }

    OS << "1'b0): begin\n";
    // Print the assignment under the condition.
    OS.indent(6) << Name << "_selector_wire = ";
    I->first.printAsOperand(OS);
    OS << ";\n";
    // Print the enable.
    OS.indent(6) << Name << "_selector_enable = 1'b1;\n";
    OS.indent(4) << "end\n";
  }

  // Write the default condition, otherwise latch will be inferred.
  OS.indent(4) << "default: begin\n";
  OS.indent(6) << Name << "_selector_wire = " << BitWidth << "'bx;\n";
  OS.indent(6) << Name << "_selector_enable = 1'b0;\n";
  OS.indent(4) << "end\n";
  OS.indent(2) << "endcase\nend  // end mux logic\n\n";
}

void VASTSeqValue::buildCSEMap(std::map<VASTValPtr, std::vector<VASTValPtr> >
                               &CSEMap) const {
  for (assign_itertor I = begin(), E = end(); I != E; ++I)
    CSEMap[*I->second].push_back(I->first);
}

void VASTSeqValue::verifyAssignCnd(vlang_raw_ostream &OS, const Twine &Name,
                                   const VASTModule *Mod) const {
  // Concatenate all condition together to detect the case that more than one
  // case is activated.
  std::string AllPred;
  raw_string_ostream AllPredSS(AllPred);

  AllPredSS << '{';
  for (assign_itertor I = begin(), E = end(); I != E; ++I) {
    I->first->printAsOperand(AllPredSS, false);
    AllPredSS << ", ";
  }
  AllPredSS << "1'b0 }";
  AllPredSS.flush();

  // As long as $onehot0(expr) returns true if at most one bit of expr is high,
  // we can use it to detect if more one case condition is true at the same
  // time.
  OS << "if (!$onehot0(" << AllPred << "))"
        " begin\n $display(\"At time %t, register "
        << Name << " in module " << ( Mod ? Mod->getName() : "Unknown")
        << " has more than one active assignment: %b!\", $time(), "
        << AllPred << ");\n";

  // Display the conflicted condition and its slot.
  for (assign_itertor I = begin(), E = end(); I != E; ++I) {
    OS.indent(2) << "if (";
    I->first->printAsOperand(OS, false);
    OS << ") begin\n";

    OS.indent(4) << "$display(\"Condition: ";
    I->first->printAsOperand(OS, false);

    unsigned CndSlot = I->first->getSlotNum();
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

void VASTSeqValue::anchor() const {}

void
VASTBlockRAM::printSelector(raw_ostream &OS, const VASTSeqValue &Port) const {
  if (Port.empty()) return;

  typedef std::vector<VASTValPtr> OrVec;
  typedef std::map<VASTValPtr, OrVec> CSEMapTy;
  typedef CSEMapTy::const_iterator it;

  const std::string &BRAMArray = VFUBRAM::getArrayName(getBlockRAMNum());

  CSEMapTy AddrCSEMap, DataCSEMap;
  for (assign_itertor I = Port.begin(), E = Port.end(); I != E; ++I) {
    VASTExpr *Expr = cast<VASTExpr>(*I->second);

    // Add the address to the selector.
    AddrCSEMap[Expr->getOperand(0)].push_back(I->first);

    // Add the data to the selector.
    if (Expr->getOpcode() == VASTExpr::dpWrBRAM)
      DataCSEMap[Expr->getOperand(1)].push_back(I->first);
  }

  assert(!AddrCSEMap.empty() && "Unexpected zero address bus fanin!");
  unsigned AddrWidth = Log2_32_Ceil(getDepth());
  PrintSelector(OS, Twine(BRAMArray) + "_addr", AddrWidth, AddrCSEMap);

  // Only create the selector if there is any fanin to the data port.
  if (!DataCSEMap.empty())
    PrintSelector(OS, Twine(BRAMArray) + "_data", getWordSize(), DataCSEMap);
}

void VASTBlockRAM::printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod,
                                   const VASTSeqValue &Port) const {
  if (Port.empty()) return;

  const std::string &BRAMArray = VFUBRAM::getArrayName(getBlockRAMNum());

  unsigned AddrWidth = Log2_32_Ceil(getDepth());
  // The block RAM is active if the address bus is active.
  OS.if_begin(Twine(BRAMArray) + "_addr" + "_selector_enable");
  // Check if there is any write to the block RAM.
  bool HasAnyWrite = false;

  for (assign_itertor I = Port.begin(), E = Port.end(); I != E; ++I) {
    VASTExpr *Expr = cast<VASTExpr>(*I->second);
    if (Expr->getOpcode() == VASTExpr::dpWrBRAM) {
      HasAnyWrite = true;
      break;
    }
  }

  // Only print the write port if there is any write.
  if (HasAnyWrite) {
    // It is a write if the data bus is active.
    OS.if_begin(Twine(BRAMArray) + "_data" + "_selector_enable");
    OS << BRAMArray << '[' << BRAMArray << "_addr_selector_wire"
      << VASTValue::printBitRange(AddrWidth, 0, false) << ']' << " <= "
      << BRAMArray << "_data_selector_wire"
      << VASTValue::printBitRange(getWordSize(), 0, false) << ";\n";
    OS.exit_block();
  }

  // Else is is a read, write to the output port.
  // To let the synthesis tools correctly infer a block RAM, the write to
  // result register is active even the current operation is a write access.
  OS << VFUBRAM::getOutDataBusName(getBlockRAMNum())
    << VASTValue::printBitRange(getWordSize(), 0, false) << " <= "
    << BRAMArray << '[' << BRAMArray << "_addr_selector_wire"
    << VASTValue::printBitRange(AddrWidth, 0, false) << "];\n";
  OS.exit_block();

  OS << "// synthesis translate_off\n";
  Port.verifyAssignCnd(OS, BRAMArray, Mod);
  OS << "// synthesis translate_on\n\n";
}

void VASTRegister::printSelector(raw_ostream &OS) const {
  if (Value.empty()) return;

  typedef std::vector<VASTValPtr> OrVec;
  typedef std::map<VASTValPtr, OrVec> CSEMapTy;
  CSEMapTy SrcCSEMap;

  Value.buildCSEMap(SrcCSEMap);

  PrintSelector(OS, Twine(getName()), getBitWidth(), SrcCSEMap);
}

void
VASTRegister::printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  if (Value.empty()) return;

  OS.if_begin(Twine(getName()) + Twine("_selector_enable"));
  OS << getName() << " <= " << getName() << "_selector_wire"
     << VASTValue::printBitRange(getBitWidth(), 0, false) << ";\n";
  OS.exit_block();

  OS << "// synthesis translate_off\n";
  Value.verifyAssignCnd(OS, getName(), Mod);
  OS << "// synthesis translate_on\n\n";
}

void VASTRegister::dumpAssignment() const {
  vlang_raw_ostream S(dbgs());
  printAssignment(S, 0);
}