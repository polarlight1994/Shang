//===- VASTModule.cpp - The Module of the synthesized hardware --*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements VASTModule.
//
//===----------------------------------------------------------------------===//
#include "LangSteam.h"

#include "vast/VASTMemoryBank.h"
#include "vast/VASTModule.h"
#include "vast/LuaI.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-lua-bases"
#include "llvm/Support/Debug.h"

using namespace llvm;
//----------------------------------------------------------------------------//
VASTPort::VASTPort(VASTTypes Type) : VASTNode(Type) {}

VASTPort::~VASTPort() {}

void VASTPort::print(raw_ostream &OS) const {
  if (isInput())
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
  return isa<VASTOutPort>(this);
}

const char *VASTPort::getNameImpl() const {
  llvm_unreachable("Subclass should override this!");
  return 0;
}

unsigned VASTPort::getBitWidthImpl() const {
  llvm_unreachable("Subclass should override this!");
  return 0;
}

VASTOutPort::VASTOutPort(VASTSelector *Sel) : VASTPort(vastOutPort) {
  Contents.Sel = Sel;
  Sel->setParent(this);
}

VASTSelector *VASTOutPort::getSelector() const {
  return Contents.Sel;
}

const char *VASTOutPort::getNameImpl() const {
  return getSelector()->getName();
}

unsigned VASTOutPort::getBitWidthImpl() const {
  return getSelector()->getBitWidth();
}

void VASTOutPort::print(vlang_raw_ostream &OS) const {
  getSelector()->printRegisterBlock(OS, 0);
}

VASTInPort::VASTInPort(VASTNode *Node) : VASTPort(vastInPort) {
  Contents.Node = Node;
  if (VASTSelector *Sel = dyn_cast<VASTSelector>(Node))
    Sel->setParent(this);
}

VASTWrapper *VASTInPort::getValue() const {
  return cast<VASTWrapper>(Contents.Node);
}

const char *VASTInPort::getNameImpl() const {
  if (VASTNamedValue *V = dyn_cast<VASTNamedValue>(Contents.Node))
    return V->getName();

  return cast<VASTSelector>(Contents.Node)->getName();
}

unsigned VASTInPort::getBitWidthImpl() const {
  if (VASTNamedValue *V = dyn_cast<VASTNamedValue>(Contents.Node))
    return V->getBitWidth();

  return cast<VASTSelector>(Contents.Node)->getBitWidth();
}

void VASTPort::printExternalDriver(raw_ostream &OS, uint64_t InitVal) const {
  if (isInput())
    // We need a reg to drive input port.
    OS << "reg";
  else
    // We need a wire to accept the output value from dut.
    OS << "wire";

  if (getBitWidth() > 1)
    OS << "[" << (getBitWidth() - 1) << ":0]";

  OS << ' ' << getName();

  if (isInput())
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
VASTModule::createSeqValue(VASTSelector *Selector, unsigned Idx, Value *V) {
  VASTSeqValue *SeqVal = new VASTSeqValue(Selector, Idx, V);
  SeqVals.push_back(SeqVal);
                              
  return SeqVal;
}

VASTMemoryBank *VASTModule::createDefaultMemBus() {
  VFUMemBus *Desc = LuaI::Get<VFUMemBus>();
  return createMemBus(0, Desc->getAddrWidth(), Desc->getDataWidth(),
                      true, false, false);
}

VASTMemoryBank *
VASTModule::createMemBus(unsigned Num, unsigned AddrWidth, unsigned DataWidth,
                         bool RequireByteEnable, bool IsDualPort,
                         bool IsCombinationalROM) {
  VASTMemoryBank *Bus = new VASTMemoryBank(Num, AddrWidth, DataWidth,
                                          RequireByteEnable, IsDualPort,
                                          IsCombinationalROM);
  Bus->addPorts(this);
  Submodules.push_back(Bus);
  return Bus;
}

VASTSubModule *VASTModule::addSubmodule(const Twine &Name, unsigned Num) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  assert(Entry.second == 0 && "Symbol already exist!");
  VASTSubModule *M = new VASTSubModule(Entry.getKeyData(), Num);
  Entry.second = M;
  Submodules.push_back(M);
  return M;
}

VASTWrapper *VASTModule::getOrCreateWrapper(const Twine &Name, unsigned BitWidth,
                                            Value *LLVMValue) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  if (Entry.second != 0) {
    VASTWrapper *W = cast<VASTWrapper>(Entry.second);
    assert(W->getLLVMValue() == LLVMValue
           && "Symbol already exist, with different value!");
    return W;
  }

  VASTWrapper *Wire = new VASTWrapper(Entry.getKeyData(), BitWidth, LLVMValue);
  Entry.second = Wire;
  Wires.push_back(Wire);

  return Wire;
}

VASTWrapper *VASTModule::getOrCreateWrapper(const Twine &Name, unsigned BitWidth,
                                            VASTNode *Node) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  if (Entry.second != 0) {
    VASTWrapper *W = cast<VASTWrapper>(Entry.second);
    assert(W->getVASTNode() == Node
           && "Symbol already exist, with different value!");
    return W;
  }

  VASTWrapper *Wire = new VASTWrapper(Entry.getKeyData(), BitWidth, Node);
  Entry.second = Wire;
  Wires.push_back(Wire);

  return Wire;
}

namespace {
  struct SlotNumEqual {
    unsigned SlotNum;
    SlotNumEqual(unsigned SlotNum) : SlotNum(SlotNum) {}

    bool operator()(const VASTSlot &S) const {
      return S.SlotNum == SlotNum;
    }
  };
}

VASTSlot *
VASTModule::createSlot(unsigned SlotNum, BasicBlock *ParentBB, unsigned Schedule,
                       VASTValPtr Pred, bool IsVirtual) {
  assert(std::find_if(Slots.begin(), Slots.end(), SlotNumEqual(SlotNum)) == Slots.end()
         && "The same slot had already been created!");

  VASTSlot *Slot = new VASTSlot(SlotNum, ParentBB, Pred, IsVirtual, Schedule);
  // Insert the newly created slot before the finish slot.
  Slots.insert(Slots.back(), Slot);

  return Slot;
}

VASTSlot *VASTModule::createStartSlot() {
  VASTSlot *StartSlot = new VASTSlot(0, 0, VASTImmediate::True, false, 0);
  Slots.push_back(StartSlot);
  // Also create the finish slot.
  Slots.push_back(new VASTSlot(-1));
  return StartSlot;
}

VASTSlot *VASTModule::getStartSlot() {
  return &Slots.front();
}

VASTSlot *VASTModule::getFinishSlot() {
  return &Slots.back();
}

const VASTSlot *VASTModule::getStartSlot() const {
  return &Slots.front();
}

const VASTSlot *VASTModule::getFinishSlot() const {
  return &Slots.back();
}

void VASTModule::setFunction(Function &F) {
  this->F = &F;
  Name = F.getName();
}

VASTModule::VASTModule()
  : VASTNode(vastModule), Ports(NumSpecialPort), Name(), BBX(0), BBY(0),
    BBWidth(0), BBHeight(0), F(0), NumArgPorts(0) {
  createStartSlot();
}

void VASTModule::setBoundingBoxConstraint(unsigned BBX, unsigned BBY,
                                          unsigned BBWidth, unsigned BBHeight) {
  this->BBX = BBX;
  this->BBY = BBY;
  this->BBWidth = BBWidth;
  this->BBHeight = BBHeight;
}

void VASTModule::reset() {
  Wires.clear();
  Registers.clear();
  SeqOps.clear();
  SeqVals.clear();
  Slots.clear();
  Submodules.clear();

  // Release all ports.
  Ports.clear();
  Selectors.clear();
  SymbolTable.clear();
  NumArgPorts = 0;
  RetPortIdx = 0;

  // Release the datapath after all other contexts released.
  DatapathContainer::reset();
  DeleteContainerPointers(Ports);
}

VASTModule::~VASTModule() {
  // DIRTY HACK: We need to explicitly release the annotation, otherwise the
  // destructor will visit the deleted nodes.
  for (selector_iterator I = selector_begin(), E = selector_end(); I != E; ++I)
    I->dropMux();

  // Unlink the uses in the operand list of seqop from the used value.
  for (seqop_iterator I = seqop_begin(), E = seqop_end(); I != E; ++I)
    I->dropOperands();

  // To prevent we releasing deleted uses in the destructor of VASTSlots, we
  // release the slots before all other values.
  Slots.clear();

  // Now release the datapath.
  DatapathContainer::gc();
  DeleteContainerPointers(Ports);
}

namespace {
struct DatapathPrinter {
  raw_ostream &OS;
  std::set<const char*> PrintedNames;

  DatapathPrinter(raw_ostream &OS) : OS(OS) {}

  void operator()(VASTNode *N) {
    if (VASTExpr *E = dyn_cast<VASTExpr>(N))
      if (const char *Name =  E->getTempName()) {
        // If the we get an expression with the same name as some other
        // expressions, do not print them as their are structural identical
        // expressions and one of them been printed before.
        if (!PrintedNames.insert(Name).second) return;

        if (E->getOpcode() == VASTExpr::dpKeep)
          OS << "(* keep *) ";

        OS << "wire ";

        if (E->getBitWidth() > 1)
          OS << "[" << (E->getBitWidth() - 1) << ":0] ";

        OS << E->getTempName();

        if (!E->getSubModName().empty()) {
          OS << ";\n";
          if (E->printFUInstantiation(OS))
            return;
        
          // If the FU instantiation is not printed, we need to print the
          // assignment.
          OS << "assign " << E->getTempName();
        }

        OS << " = ";

        // Temporary unname the expression so that we can print its logic.
        E->nameExpr(0);
        E->printAsOperand(OS, false);
        E->nameExpr(Name);

        OS << ";\n";
      }
  }
};
}

void VASTModule::printDatapath(raw_ostream &OS) const{
  std::set<VASTExpr*> Visited;
  DatapathPrinter Printer(OS);

  for (const_slot_iterator SI = slot_begin(), SE = slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    typedef VASTSlot::const_op_iterator op_iterator;

    // Write the expression of the slot ready signal.
    if (VASTExpr *Expr = dyn_cast<VASTExpr>(S->getActive().get()))
      Expr->visitConeTopOrder(Visited, Printer);

    OS << "\n// At slot " << S->SlotNum;
    if (S->IsSubGrp)
      OS << " (SubGrp)";
    if (BasicBlock *BB = S->getParent()) OS << ", BB: " << BB->getName();
    OS << '\n';

    // Print the logic of the datapath used by the SeqOps.
    for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
      VASTSeqOp *L = *I;

      typedef VASTOperandList::op_iterator op_iterator;
      for (op_iterator OI = L->op_begin(), OE = L->op_end(); OI != OE; ++OI) {
        if (VASTExpr *Expr = dyn_cast<VASTExpr>(OI->unwrap().get()))
          Expr->visitConeTopOrder(Visited, Printer);
      }

      OS << "/* ";
      L->print(OS);
      OS << "*/\n";
    }
  }

  // Print the logic for the selectors.
  for (const_selector_iterator I = selector_begin(), E = selector_end();
       I != E; ++I) {
    const VASTSelector *Sel = I;

    if (!Sel->isSelectorSynthesized()) continue;

    if (VASTExpr *Expr = Sel->getGuard().getAsLValue<VASTExpr>())
      Expr->visitConeTopOrder(Visited, Printer);
    if (VASTExpr *Expr = Sel->getFanin().getAsLValue<VASTExpr>())
      Expr->visitConeTopOrder(Visited, Printer);
  }
}

void VASTModule::printSubmodules(vlang_raw_ostream &OS) const {
  typedef SubmoduleList::const_iterator iterator;

  for (iterator I = Submodules.begin(), E = Submodules.end(); I != E; ++I) {
    const VASTSubModuleBase *S = I;

    // Print the data selector of the register.
    S->print(OS);
  }
}

void VASTModule::printRegisterBlocks(vlang_raw_ostream &OS) const {
  typedef const_selector_iterator iterator;

  std::set<const char *> Existednames;
  typedef RegisterVector::const_iterator const_reg_iterator;
  for (const_reg_iterator I = Registers.begin(), E = Registers.end();
       I != E; ++I) {
    if (!Existednames.insert(I->getSelector()->getName()).second)
      continue;

    I->print(OS);
  }

  for (const_port_iterator I = Ports.begin(), E = Ports.end(); I != E; ++I)
    if (VASTOutPort *P = dyn_cast<VASTOutPort>(*I))
      P->print(OS);
}

void VASTModule::printModuleDecl(raw_ostream &OS) const {
  OS << "module " << getName() << "(\n";
  Ports.front()->print(OS.indent(4));
  for (PortVector::const_iterator I = Ports.begin() + 1, E = Ports.end();
       I != E; ++I) {
    // Assign the ports to virtual pins.
    OS << ",\n (* altera_attribute = \"-name VIRTUAL_PIN on\" *)";
    (*I)->print(OS.indent(4));
  }
  OS << ");\n";
}

void VASTModule::printSignalDecl(raw_ostream &OS) const {
  typedef WireVector::const_iterator const_wire_iterator;
  typedef RegisterVector::const_iterator const_reg_iterator;

  std::set<const char *> Existednames;
  for (const_reg_iterator I = Registers.begin(), E = Registers.end();
       I != E; ++I) {
    if (!Existednames.insert(I->getSelector()->getName()).second)
      continue;

    I->printDecl(OS);
  }

  typedef SubmoduleList::const_iterator const_submod_iterator;
  for (const_submod_iterator I = Submodules.begin(),E = Submodules.end();
       I != E;++I)
    I->printDecl(OS);

  // Print the symbol of the global variable.
  for (const_wire_iterator I = Wires.begin(), E = Wires.end(); I != E; ++I)
    I->printDecl(OS);
}

VASTSymbol *VASTModule::getOrCreateSymbol(const Twine &Name, unsigned BitWidth) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  VASTNode *&V = Entry.second;
  if (V == 0) {
    const char *S = Entry.getKeyData();
    unsigned SymbolWidth = BitWidth;
    V = new (Allocator) VASTSymbol(S, SymbolWidth);
  }

  assert(cast<VASTSymbol>(V)->getBitWidth() == BitWidth
         && "Getting symbol with wrong bitwidth!");

  return cast<VASTSymbol>(V);
}

VASTSeqInst *
VASTModule::latchValue(VASTSeqValue *SeqVal, VASTValPtr Src,  VASTSlot *Slot,
                       VASTValPtr GuardCnd, Value *V, unsigned Latency) {
  assert(Src && "Bad assignment source!");
  VASTSeqInst *Inst = lauchInst(Slot, GuardCnd, 1, V, true);
  Inst->addSrc(Src, 0, SeqVal);
  Inst->setCyclesFromLaunch(Latency);

  return Inst;
}

VASTSeqInst *
VASTModule::lauchInst(VASTSlot *Slot, VASTValPtr Pred, unsigned NumOps, Value *V,
                      bool IsLatch) {
  // Create the uses in the list.
  VASTSeqInst *SeqInst = new VASTSeqInst(V, Slot, NumOps, IsLatch);
  // Create the predicate operand.
  new (SeqInst->Operands) VASTUse(SeqInst, Pred);

  // Add the SeqOp to the the all SeqOp list.
  SeqOps.push_back(SeqInst);

  return SeqInst;
}

VASTSeqCtrlOp *VASTModule::createCtrlLogic(VASTValPtr Src, VASTSlot *Slot,
                                           VASTValPtr GuardCnd,
                                           bool UseSlotActive) {
  VASTSeqCtrlOp *CtrlOp = new VASTSeqCtrlOp(Slot, UseSlotActive);
  // Create the predicate operand.
  new (CtrlOp->Operands) VASTUse(CtrlOp, GuardCnd);

  // Add the SeqOp to the the all SeqOp list.
  SeqOps.push_back(CtrlOp);
  return CtrlOp;

}

VASTSeqCtrlOp *VASTModule::assignCtrlLogic(VASTSeqValue *SeqVal, VASTValPtr Src,
                                           VASTSlot *Slot, VASTValPtr GuardCnd,
                                           bool UseSlotActive) {
  VASTSeqCtrlOp *CtrlOp = createCtrlLogic(Src, Slot, GuardCnd, UseSlotActive);
  // Create the source of the assignment
  CtrlOp->addSrc(Src, 0, SeqVal);
  return CtrlOp;
}

VASTSeqCtrlOp *VASTModule::assignCtrlLogic(VASTSelector *Selector, VASTValPtr Src,
                                           VASTSlot *Slot, VASTValPtr GuardCnd,
                                           bool UseSlotActive) {
  VASTSeqCtrlOp *CtrlOp = createCtrlLogic(Src, Slot, GuardCnd, UseSlotActive);
  // Create the source of the assignment
  CtrlOp->addSrc(Src, 0, Selector);
  return CtrlOp;
}

VASTSlotCtrl *VASTModule::createSlotCtrl(VASTNode *N, VASTSlot *Slot,
                                         VASTValPtr Pred) {
  VASTSlotCtrl *CtrlOp = new VASTSlotCtrl(Slot, N);
  // Create the predicate operand.
  new (CtrlOp->Operands) VASTUse(CtrlOp, Pred);

  // Add the SeqOp to the the all SeqOp list.
  SeqOps.push_back(CtrlOp);

  return CtrlOp;
}

void VASTModule::eraseSelector(VASTSelector *Sel) {
  assert(Sel->def_empty() && Sel->empty()
         && "Cannot erase the Sel that is still in use!");

  // Try to erase the corresponding register as well.
  if (VASTRegister *R = dyn_cast<VASTRegister>(Sel->getParent()))
    Registers.erase(R);

  // Also erase the selector from the symbol table.
  SymbolTable.erase(Sel->getName());

  Selectors.erase(Sel);
}

void VASTModule::eraseSeqVal(VASTSeqValue *Val) {
  assert(Val->use_empty() && "Val still stuck at some user!");
  assert(Val->fanin_empty() && "Val still using something!");
  // Also erase the selector if it become empty.
  VASTSelector *Sel = Val->getSelector();
  Val->changeSelector(0);

  // Do not delete the output of the FU, because it is always there.
  if (Sel->empty() && !Sel->isFUOutput()) eraseSelector(Sel);

  SeqVals.erase(Val);
}

void VASTModule::eraseSeqOp(VASTSeqOp *SeqOp) {
  if (SeqOp->getSlot())
    SeqOp->removeFromParent();

  for (unsigned i = 0, e = SeqOp->num_srcs(); i != e; ++i) {
    VASTLatch U = SeqOp->getSrc(i);
    U.removeFromParent();
  }

  SeqOp->dropUses();
  SeqOps.erase(SeqOp);
}

void VASTModule::print(raw_ostream &OS) const {
  for (const_slot_iterator SI = slot_begin(), SE = slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    S->print(OS);

    OS << '\n';
  }
}

VASTPort *VASTModule::createPort(VASTNode *Node, bool IsInput) {
  VASTPort *P = 0;

  // Create a output port for the selector.
  if (IsInput)
    P = new VASTInPort(Node);
  else
    // It must be a Selector for the output port.
    P = new VASTOutPort(cast<VASTSelector>(Node));

  return P;
}

VASTPort *VASTModule::addPort(VASTNode *Node, bool IsInput) {
  VASTPort *P = createPort(Node, IsInput);
  Ports.push_back(P);
  return P;
}

VASTInPort *VASTModule::addInputPort(const Twine &Name, unsigned BitWidth,
                                     PortTypes T /*= Others*/) {
  VASTWrapper *Wire = getOrCreateWrapper(Name, BitWidth, (VASTNode*)0);
  VASTPort *Port = createPort(Wire, true);

  if (T < SpecialInPortEnd) {
    assert(Ports[T] == 0 && "Special port exist!");
    Ports[T] = Port;
    return cast<VASTInPort>(Port);
  }

  // Return port is a output port.
  assert(T < RetPort && "Wrong port type!");
  if (T == ArgPort) {
    assert(NumArgPorts == Ports.size() - NumSpecialPort
           && "Unexpected port added before arg port!");
    ++NumArgPorts;
  }

  Ports.push_back(Port);
  return cast<VASTInPort>(Port);
}

VASTOutPort *VASTModule::addOutputPort(const Twine &Name, unsigned BitWidth,
                                       PortTypes T /*= Others*/) {
  VASTSelector::Type SelTy = VASTSelector::Temp;
  VASTSelector *Sel = createSelector(Name, BitWidth, 0, SelTy);
  VASTPort *Port = createPort(Sel, false);

  if (SpecialInPortEnd <= T && T < SpecialOutPortEnd) {
    assert(Ports[T] == 0 && "Special port exist!");
    Ports[T] = Port;
    return cast<VASTOutPort>(Port);
  }

  assert(T <= RetPort && "Wrong port type!");
  if (T == RetPort) {
    RetPortIdx = Ports.size();
    assert(RetPortIdx == NumArgPorts + NumSpecialPort
           && "Unexpected port added before return port!");
  }

  Ports.push_back(Port);
  return cast<VASTOutPort>(Port);
}

VASTRegister *VASTModule::createRegister(const Twine &Name, unsigned BitWidth,
                                         unsigned InitVal, VASTSelector::Type T)
{
  // Create the selector before we create the register.
  VASTSelector *Sel = createSelector(Name, BitWidth, 0, T);
  VASTRegister *Reg = new VASTRegister(Sel, InitVal);

  Registers.push_back(Reg);

  return Reg;
}

VASTSelector *VASTModule::createSelector(const Twine &Name, unsigned BitWidth,
                                         VASTNode *Parent, VASTSelector::Type T)
{
  // Allocate the entry from the symbol table.
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  // Make sure the each symbol is for 1 node only.
  assert(Entry.second == 0 && "Symbol had already taken!");
  VASTSelector *Sel
    = new VASTSelector(Entry.getKeyData(), BitWidth, T, Parent);
  Entry.second = Sel;
  // Put the newly create selector into the list, so that we can release it.
  Selectors.push_back(Sel);

  return Sel;
}

//===----------------------------------------------------------------------===//
void VASTModule::resetSelectorName() {
  typedef SymTabTy::iterator iterator;
  for (iterator I = SymbolTable.begin(), E = SymbolTable.end(); I != E; ++I) {
    VASTSelector *Sel = dyn_cast<VASTSelector>(I->second);
    if (Sel == 0)
      continue;

    Sel->setName(I->getKeyData());
  }
}

void VASTModule::printSubmodules(raw_ostream &OS) const {
  vlang_raw_ostream O(OS);
  O.enter_block("\n", "");
  printSubmodules(O);
  O.exit_block("\n", "");
}

void VASTModule::printRegisterBlocks(raw_ostream &OS) const {
  vlang_raw_ostream O(OS);
  O.enter_block("\n", "");
  printRegisterBlocks(O);
  O.exit_block("\n", "");
}

bool VASTModule::gc() {
  bool Changed = false;

  // Clear up the dead VASTSeqValues.
  for (seqval_iterator VI = seqval_begin(); VI != seqval_end(); /*++I*/) {
    VASTSeqValue *V = VI++;

    if (!V->use_empty()) continue;

    SmallVector<VASTLatch, 4> DeadOps(V->fanin_begin(), V->fanin_end());

    while (!DeadOps.empty()) {
      VASTLatch L = DeadOps.pop_back_val();
      if (L.Op->getNumDefs() == 1)
        eraseSeqOp(L.Op);
      else
        L.eraseOperand();
    }

    eraseSeqVal(V);

    Changed |= true;
  }

  // Release the dead seqops, this happen when we fold the SeqOp through the
  // false paths.
  for (seqop_iterator I = seqop_begin(); I != seqop_end(); /*++I*/) {
    VASTSeqOp *Op = I++;
    if (Op->getGuard() == VASTImmediate::False) {
      DEBUG(dbgs() << "Removing SeqOp whose predicate is always false:\n";
      Op->dump(););

      eraseSeqOp(Op);

      Changed |= true;
    }
  }

  // At last clear up the dead VASTExprs.
  return DatapathContainer::gc() || Changed;
}
