//===- VASTLuaBases.cpp - The classes in VAST need to be bound --*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the classes that need to be bound.
//
//===----------------------------------------------------------------------===//
#include "vtm/VASTSubModules.h"
#include "vtm/VASTModule.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#define DEBUG_TYPE "vast-lua-bases"
#include "llvm/Support/Debug.h"

using namespace llvm;

//----------------------------------------------------------------------------//
void VASTNode::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

//----------------------------------------------------------------------------//
VASTValue::dp_dep_it VASTValue::dp_dep_begin(const VASTValue *V) {
  switch (V->getASTType()) {
  case VASTNode::vastExpr: return cast<VASTExpr>(V)->op_begin();
  case VASTNode::vastWire: return cast<VASTWire>(V)->op_begin();
  default:  return VASTValue::dp_dep_it(0);
  }

}

VASTValue::dp_dep_it VASTValue::dp_dep_end(const VASTValue *V) {
  switch (V->getASTType()) {
  case VASTNode::vastExpr: return cast<VASTExpr>(V)->op_end();
  case VASTNode::vastWire: return cast<VASTWire>(V)->op_end();
  default:  return VASTValue::dp_dep_it(0);
  }
}

void VASTValue::print(raw_ostream &OS) const {
  printAsOperandImpl(OS);
}

void VASTValue::printAsOperand(raw_ostream &OS, unsigned UB, unsigned LB,
                               bool isInverted) const{
  if (isInverted) OS << "(~";
  OS << '(';
  printAsOperandImpl(OS, UB, LB);
  OS << ')';
  if (isInverted) OS << ')';
}

void VASTValue::printAsOperand(raw_ostream &OS, bool isInverted) const {
  if (isInverted) OS << "(~";
  OS << '(';
  printAsOperandImpl(OS);
  OS << ')';
  if (isInverted) OS << ')';
}

void VASTValue::printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                   unsigned LB) const {
  assert(0 && "VASTValue::printAsOperand should not be called!");
}

std::string VASTValue::printBitRange(unsigned UB, unsigned LB, bool printOneBit){
  std::string ret;
  assert(UB && UB >= LB && "Bad bit range!");
  --UB;
  if (UB != LB)
    ret = "[" + utostr_32(UB) + ":" + utostr_32(LB) + "]";
  else if(printOneBit)
    ret = "[" + utostr_32(LB) + "]";

  return ret;
}

//----------------------------------------------------------------------------//
VASTPort::VASTPort(VASTNamedValue *V, bool isInput)
  : VASTNode(vastPort), IsInput(isInput)
{
  Contents.Value = V;
}

void VASTPort::print(raw_ostream &OS) const {
  if (IsInput)
    OS << "input ";
  else
    OS << "output ";

  if (isRegister())
    OS << "reg";
  else
    OS << "wire";

  if (getBitWidth() > 1) OS << "[" << (getBitWidth() - 1) << ":0]";

  OS << ' ' << getName();
}

bool VASTPort::isRegister() const {
  return !isInput() && !isa<VASTWire>(getValue());
}

VASTSeqValue *VASTPort::getSeqVal() const {
  return cast<VASTSeqValue>(getValue());
}

void VASTPort::printExternalDriver(raw_ostream &OS, uint64_t InitVal) const {
  if (IsInput)
    // We need a reg to drive input port.
    OS << "reg";
  else
    // We need a wire to accept the output value from dut.
    OS << "wire";

  if (getBitWidth() > 1)
    OS << "[" << (getBitWidth() - 1) << ":0]";

  OS << ' ' << getName();

  if (IsInput)
    OS << " = " << VASTImmediate::buildLiteral(InitVal, getBitWidth(), false);

  OS << ';';
}

std::string VASTPort::getExternalDriverStr(unsigned InitVal) const {
  std::string ret;
  raw_string_ostream ss(ret);
  printExternalDriver(ss, InitVal);
  ss.flush();
  return ret;
}

//----------------------------------------------------------------------------//
VASTSeqValue *
VASTModule::createSeqValue(const std::string &Name, unsigned BitWidth,
                           VASTNode::SeqValType T, unsigned Idx, VASTNode *P) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name);
  VASTSeqValue *V
    = new (Allocator) VASTSeqValue(Entry.getKeyData(), BitWidth, T, Idx, *P);
  SeqVals.push_back(V);

  return V;
}

VASTBlockRAM *VASTModule::addBlockRAM(unsigned BRamNum, unsigned Bitwidth,
                                      unsigned Size, const GlobalVariable *Init){
  VASTBlockRAM *RAM
    = new (Allocator) VASTBlockRAM("", BRamNum, Bitwidth, Size, Init);
  // Build the ports.
  RAM->addPorts(this);
  Submodules.push_back(RAM);
  return RAM;
}

VASTSubModule *VASTModule::addSubmodule(const char *Name, unsigned Num) {
  VASTSubModule *M = new (Allocator) VASTSubModule(Name, Num);
  Submodules.push_back(M);
  return M;
}

VASTWire *VASTModule::addWire(const std::string &Name, unsigned BitWidth,
                              const char *Attr, bool IsPinned) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name);
  assert(Entry.second == 0 && "Symbol already exist!");
  VASTWire *Wire = Allocator.Allocate<VASTWire>();
  new (Wire) VASTWire(Entry.getKeyData(), BitWidth, Attr, IsPinned);
  Entry.second = Wire;
  Wires.push_back(Wire);

  return Wire;
}

VASTValPtr VASTModule::getOrCreateSymbol(const std::string &Name,
                                         unsigned BitWidth,
                                         bool CreateWrapper) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name);
  VASTNamedValue *&V = Entry.second;
  if (V == 0) {
    const char *S = Entry.getKeyData();
    // If we are going to create a wrapper, apply the bitwidth to the wrapper.
    unsigned SymbolWidth = CreateWrapper ? 0 : BitWidth;
    V = new (Allocator.Allocate<VASTSymbol>()) VASTSymbol(S, SymbolWidth);
    if (CreateWrapper) {
      // Create the wire for the symbol, and assign the symbol to the wire.
      VASTWire *Wire = addWire(VBEMangle(Name + "_s"), BitWidth);
      Wire->assign(V);
      // Remember the wire.
      V = Wire;
    }
  }

  assert(V->getBitWidth() == BitWidth
          && "Getting symbol with wrong bitwidth!");

  return V;
}

VASTSlot *VASTModule::getOrCreateSlot(unsigned SlotNum,
                                      MachineInstr *BundleStart) {
  VASTSlot *&Slot = Slots[SlotNum];
  if(Slot == 0) {
    Slot = Allocator.Allocate<VASTSlot>();
    assert((BundleStart == 0 || BundleStart->getOpcode() == VTM::CtrlStart)
           && "Bad BundleStart!");
    new (Slot) VASTSlot(SlotNum, BundleStart, this);
  }

  return Slot;
}

void VASTModule::writeProfileCounters(VASTSlot *S, bool isFirstSlot) {
  MachineBasicBlock *BB = S->getParentBB();
  std::string BBCounter = "cnt"+ utostr_32(BB ? BB->getNumber() : 0);
  std::string FunctionCounter = "cnt" + getName();
  vlang_raw_ostream &CtrlS = getControlBlockBuffer();

  // Create the profile counter.
  // Write the counter for the function.
  if (S->SlotNum == 0) {
    addRegister(FunctionCounter, 64)/*->Pin()*/;
    addRegister(BBCounter, 64)/*->Pin()*/;
    CtrlS.if_begin(getPortName(VASTModule::Finish));
    CtrlS << "$display(\"Module: " << getName();

    CtrlS << " total cycles" << "->%d\"," << FunctionCounter << ");\n";
    CtrlS.exit_block() << "\n";
  } else { // Dont count the ilde state at the moment.
    if (isFirstSlot) {
      addRegister(BBCounter, 64)/*->Pin()*/;

      CtrlS.if_begin(getPortName(VASTModule::Finish));
      CtrlS << "$display(\"Module: " << getName();
      // Write the parent MBB name.
      if (BB)
        CtrlS << " MBB#" << BB->getNumber() << ": " << BB->getName();

      CtrlS << ' ' << "->%d\"," << BBCounter << ");\n";
      CtrlS.exit_block() << "\n";
    }

    // Increase the profile counter.
    if (S->isLeaderSlot()) {
      CtrlS.if_() << S->getName();
      if (S->hasAliasSlot()) {
        for (unsigned i = S->alias_start(), e = S->alias_end(),
          k = S->alias_ii(); i < e; i += k) {
            CtrlS << '|' << getSlot(i)->getName();
        }
      }

      CtrlS._then();
      CtrlS << BBCounter << " <= " << BBCounter << " +1;\n";
      CtrlS << FunctionCounter << " <= " << FunctionCounter << " +1;\n";
      CtrlS.exit_block() << "\n";
    }
  }
}

void VASTModule::reset() {
  DatapathContainer::reset();

  // Release all ports.
  Slots.clear();
  Ports.clear();
  Wires.clear();
  Registers.clear();
  SymbolTable.clear();
  FUPortOffsets.clear();
  NumArgPorts = 0;
  RetPortIdx = 0;
}

VASTModule::~VASTModule() {
  reset();

  delete &(ControlBlock.str());
}
std::string VASTModule::DirectClkEnAttr = "";
std::string VASTModule::ParallelCaseAttr = "";
std::string VASTModule::FullCaseAttr = "";

void VASTModule::printDatapath(raw_ostream &OS) const{
  for (WireVector::const_iterator I = Wires.begin(), E = Wires.end();
       I != E; ++I) {
    VASTWire *W = *I;
    // Do not print the trivial dead data-path.
    if (W->getDriver() && (W->isPinned() || !W->use_empty()))
      W->printAssignment(OS);
  }
}

void VASTModule::printSubmodules(vlang_raw_ostream &OS) const {
  typedef SubmoduleVector::const_iterator iterator;

  for (iterator I = Submodules.begin(), E = Submodules.end(); I != E; ++I) {
    VASTSubModuleBase *S = *I;

    // Print the data selector of the register.
    S->print(OS, this);
  }
}

void VASTModule::printRegisterBlocks(vlang_raw_ostream &OS) const {
  typedef RegisterVector::const_iterator iterator;

  for (iterator I = Registers.begin(), E = Registers.end(); I != E; ++I) {
    VASTRegister *R = *I;
    R->print(OS, this);
  }
}

void VASTModule::printModuleDecl(raw_ostream &OS) const {
  OS << "module " << getName() << "(\n";
  Ports.front()->print(OS.indent(4));
  for (PortVector::const_iterator I = Ports.begin() + 1, E = Ports.end();
       I != E; ++I) {
    OS << ",\n";
    (*I)->print(OS.indent(4));
  }
  OS << ");\n";
}

template<typename T>
static
raw_ostream &printDecl(raw_ostream &OS, T *V, bool declAsRegister,
                       const char *AttrStr) {
  OS << AttrStr << ' ';

  if (declAsRegister)
    OS << "reg";
  else
    OS << "wire";

  if (V->getBitWidth() > 1)
    OS << "[" << (V->getBitWidth() - 1) << ":0]";

  OS << ' ' << V->getName();

  if (isa<VASTRegister>(V))
    OS << " = " << VASTImmediate::buildLiteral(0, V->getBitWidth(), false);

  OS << ";";

  return OS;
}

void VASTModule::printSignalDecl(raw_ostream &OS) {
  for (wire_iterator I = Wires.begin(), E = Wires.end(); I != E; ++I) {
    VASTWire *W = *I;

    // Print the declaration.
    if (W->use_empty() && !W->isPinned()) OS << "//";
    printDecl(OS, W, false, W->AttrStr);
    OS << "// uses " << W->num_uses() << " pinned " << W->isPinned() << '\n';
  }

  for (reg_iterator I = Registers.begin(), E = Registers.end(); I != E; ++I) {
    VASTRegister *R = *I;
    printDecl(OS, R, true, R->AttrStr) << "\n";
  }

  for (submod_iterator I = Submodules.begin(),E = Submodules.end();I != E;++I) {
    if (VASTBlockRAM *R = dyn_cast<VASTBlockRAM>(*I))
      printDecl(OS, R->getRAddr(0), true, "") << "\n";
  }
}

VASTWire *VASTModule::assign(VASTWire *W, VASTValPtr V, VASTNode::WireType T) {
  if (W->getDriver() != V) W->assign(V, T);

  return W;
}

VASTWire *VASTModule::createAssignPred(VASTSlot *Slot, MachineInstr *DefMI) {
  return new (Allocator) VASTWire(Slot->SlotNum, DefMI);
}

void VASTModule::addAssignment(VASTSeqValue *V, VASTValPtr Src, VASTSlot *Slot,
                               SmallVectorImpl<VASTValPtr> &Cnds,
                               MachineInstr *DefMI, bool AddSlotActive) {
  if (Src) {
    VASTWire *Cnd = createAssignPred(Slot, DefMI);
    Cnd = addPredExpr(Cnd, Cnds, AddSlotActive);
    VASTUse *U = new (Allocator.Allocate<VASTUse>()) VASTUse(V, Src);
    V->addAssignment(U, Cnd);
  }
}

VASTWire *VASTModule::assignWithExtraDelay(VASTWire *W, VASTValPtr V,
                                           unsigned latency) {
  if (W->getExpr() != V)
    W->assignWithExtraDelay(V, latency);

  return W;
}

void VASTModule::print(raw_ostream &OS) const {
  // Print the verilog module?
}

VASTPort *VASTModule::addPort(const std::string &Name, unsigned BitWidth,
                              bool isReg, bool isInput) {
  VASTNamedValue *V;
  if (isInput || isReg)
    V = addRegister(Name, BitWidth, 0, VASTNode::IO, 0, "// ")->getValue();
  else
    V = addWire(Name, BitWidth, "// ", true);

  VASTPort *Port = new (Allocator) VASTPort(V, isInput);

  return Port;
}

VASTPort *VASTModule::addInputPort(const std::string &Name, unsigned BitWidth,
                                   PortTypes T /*= Others*/) {
  VASTPort *Port = addPort(Name, BitWidth, false, true);

  if (T < SpecialInPortEnd) {
    assert(Ports[T] == 0 && "Special port exist!");
    Ports[T] = Port;
    return Port;
  }

  // Return port is a output port.
  assert(T < RetPort && "Wrong port type!");
  if (T == ArgPort) {
    assert(NumArgPorts == Ports.size() - NumSpecialPort
           && "Unexpected port added before arg port!");
    ++NumArgPorts;
  }

  Ports.push_back(Port);
  return Port;
}

VASTPort *VASTModule::addOutputPort(const std::string &Name, unsigned BitWidth,
                                    PortTypes T /*= Others*/,
                                    bool isReg /*= true*/) {
  VASTPort *Port = addPort(Name, BitWidth, isReg, false);

  if (SpecialInPortEnd <= T && T < SpecialOutPortEnd) {
    assert(Ports[T] == 0 && "Special port exist!");
    Ports[T] = Port;
    return Port;
  }

  assert(T <= RetPort && "Wrong port type!");
  if (T == RetPort) {
    RetPortIdx = Ports.size();
    assert(RetPortIdx == NumArgPorts + NumSpecialPort
           && "Unexpected port added before return port!");
  }

  Ports.push_back(Port);
  return Port;
}

VASTRegister *VASTModule::addRegister(const std::string &Name, unsigned BitWidth,
                                      unsigned InitVal, VASTNode::SeqValType T,
                                      uint16_t RegData, const char *Attr) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name);
  assert(Entry.second == 0 && "Symbol already exist!");
  VASTRegister *Reg = Allocator.Allocate<VASTRegister>();
  new (Reg) VASTRegister(Entry.getKeyData(), BitWidth, InitVal, T, RegData,Attr);
  Entry.second = Reg->getValue();

  Registers.push_back(Reg);
  SeqVals.push_back(Reg->getValue());

  return Reg;
}

VASTRegister *VASTModule::addOpRegister(const std::string &Name, unsigned BitWidth,
                                        unsigned FUNum, const char *Attr) {
  return addRegister(Name, BitWidth, 0, VASTNode::Data, FUNum, Attr);
}

VASTRegister *VASTModule::addDataRegister(const std::string &Name, unsigned BitWidth,
                                          unsigned RegNum, const char *Attr) {
  return addRegister(Name, BitWidth, 0, VASTNode::Data, RegNum, Attr);
}

//----------------------------------------------------------------------------//
void DatapathContainer::removeValueFromCSEMaps(VASTNode *N) {
  if (VASTImmediate *Imm = dyn_cast<VASTImmediate>(N)) {
    UniqueImms.RemoveNode(Imm);
    return;
  }

  if (VASTExpr *Expr = dyn_cast<VASTExpr>(N)) {
    UniqueExprs.RemoveNode(Expr);
    return;
  }

  // Otherwise V is not in the CSEMap, do nothing.
}

template<typename T>
void DatapathContainer::addModifiedValueToCSEMaps(T *V, FoldingSet<T> &CSEMap) {
  T *Existing = CSEMap.GetOrInsertNode(V);

  if (Existing != V) {
    // If there was already an existing matching node, use ReplaceAllUsesWith
    // to replace the dead one with the existing one.  This can cause
    // recursive merging of other unrelated nodes down the line.
    replaceAllUseWithImpl(V, Existing);
  }
}

void DatapathContainer::addModifiedValueToCSEMaps(VASTNode *N) {
  if (VASTImmediate *Imm = dyn_cast<VASTImmediate>(N)) {
    addModifiedValueToCSEMaps(Imm, UniqueImms);
    return;
  }

  if (VASTExpr *Expr = dyn_cast<VASTExpr>(N)) {
    addModifiedValueToCSEMaps(Expr, UniqueExprs);
    return;
  }

  // Otherwise V is not in the CSEMap, do nothing.
}

void DatapathContainer::replaceAllUseWithImpl(VASTValPtr From, VASTValPtr To) {
  assert(From && To && From != To && "Unexpected VASTValPtr value!");
  assert(From->getBitWidth() == To->getBitWidth() && "Bitwidth not match!");
  VASTValue::use_iterator UI = From->use_begin(), UE = From->use_end();

  while (UI != UE) {
    VASTNode *User = *UI;

    // This node is about to morph, remove its old self from the CSE maps.
    removeValueFromCSEMaps(User);

    // A user can appear in a use list multiple times, and when this
    // happens the uses are usually next to each other in the list.
    // To help reduce the number of CSE recomputations, process all
    // the uses of this user that we can find this way.
    do {
      VASTUse *Use = UI.get();
      VASTValPtr UsedValue = Use->get();
      VASTValPtr Replacement = To;
      // If a inverted value is used, we must also invert the replacement.
      if (UsedValue != From) {
        assert(UsedValue.invert() == From && "Use not using 'From'!");
        Replacement = Replacement.invert();
      }

      ++UI;
      // Move to new list.
      Use->replaceUseBy(Replacement);

    } while (UI != UE && *UI == User);

    // Now that we have modified User, add it back to the CSE maps.  If it
    // already exists there, recursively merge the results together.
    addModifiedValueToCSEMaps(User);
  }

  assert(From->use_empty() && "Incompleted replacement!");
  // From is dead now, unlink it from all its use.
  From->dropUses();
  // TODO: Delete From.
}

VASTValPtr DatapathContainer::createExprImpl(VASTExpr::Opcode Opc,
                                             ArrayRef<VASTValPtr> Ops,
                                             unsigned UB, unsigned LB) {
  assert(!Ops.empty() && "Unexpected empty expression");
  if (Ops.size() == 1) {
    switch (Opc) {
    default: break;
    case VASTExpr::dpAnd: case VASTExpr::dpAdd: case VASTExpr::dpMul:
      return Ops[0];
    }
  }

  FoldingSetNodeID ID;

  // Profile the elements of VASTExpr.
  ID.AddInteger(Opc);
  ID.AddInteger(UB);
  ID.AddInteger(LB);
  for (unsigned i = 0; i < Ops.size(); ++i)
    ID.AddPointer(Ops[i]);

  void *IP = 0;
  if (VASTExpr *E = UniqueExprs.FindNodeOrInsertPos(ID, IP))
    return E;

  // If the Expression do not exist, allocate a new one.
  // Place the VASTUse array right after the VASTExpr.
  void *P = Allocator.Allocate(sizeof(VASTExpr) + Ops.size() * sizeof(VASTUse),
                               alignOf<VASTExpr>());
  VASTExpr *E = new (P) VASTExpr(Opc, Ops.size(), UB, LB);

  // Initialize the use list and compute the actual size of the expression.
  unsigned ExprSize = 0;

  for (unsigned i = 0; i < Ops.size(); ++i) {
    assert(Ops[i].get() && "Unexpected null VASTValPtr!");

    if (VASTExpr *E = Ops[i].getAsLValue<VASTExpr>()) ExprSize += E->ExprSize;
    else                                              ++ExprSize;

    (void) new (E->ops() + i) VASTUse(E, Ops[i]);
  }

  E->ExprSize = ExprSize;

  UniqueExprs.InsertNode(E, IP);
  return E;
}

void DatapathContainer::reset() {
  UniqueExprs.clear();
  UniqueImms.clear();
  Allocator.Reset();
}

VASTImmediate *DatapathContainer::getOrCreateImmediateImpl(const APInt &Value) {
  FoldingSetNodeID ID;

  Value.Profile(ID);

  void *IP = 0;
  if (VASTImmediate *V = UniqueImms.FindNodeOrInsertPos(ID, IP))
    return V;

  void *P = Allocator.Allocate(sizeof(VASTImmediate), alignOf<VASTImmediate>());
  VASTImmediate *V = new (P) VASTImmediate(Value);
  UniqueImms.InsertNode(V, IP);

  return V;
}

void VASTNamedValue::printAsOperandImpl(raw_ostream &OS, unsigned UB,
  unsigned LB) const{
    OS << getName();
    if (UB) OS << VASTValue::printBitRange(UB, LB, getBitWidth() > 1);
}
