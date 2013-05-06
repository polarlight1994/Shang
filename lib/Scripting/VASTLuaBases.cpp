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
#include "shang/VASTMemoryPort.h"
#include "shang/VASTModule.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "vast-lua-bases"
#include "llvm/Support/Debug.h"

using namespace llvm;
//----------------------------------------------------------------------------//
void VASTNode::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

void VASTNode::dropUses() {
  dbgs() << "Current Type " << unsigned(getASTType()) << '\n';
  llvm_unreachable("Subclass should implement this function!");
}

std::string VASTNode::DirectClkEnAttr = "";
std::string VASTNode::ParallelCaseAttr = "";
std::string VASTNode::FullCaseAttr = "";

//----------------------------------------------------------------------------//
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
  assert(UB && UB > LB && "Bad bit range!");
  --UB;
  if (UB != LB)
    ret = "[" + utostr_32(UB) + ":" + utostr_32(LB) + "]";
  else if(printOneBit)
    ret = "[" + utostr_32(LB) + "]";

  return ret;
}

bool VASTValue::extractSupporingSeqVal(std::set<VASTSeqValue*> &SeqVals) {
  VASTValue *Root = this;

  std::set<VASTOperandList*> Visited;
  VASTOperandList *L = VASTOperandList::GetDatapathOperandList(Root);
  // The entire tree had been visited.
  if (!L) {
    // If ChildNode is a not data-path operand list, it may be the SeqVal.
    if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(Root))
      SeqVals.insert(SeqVal);

    return !SeqVals.empty();
  }

  typedef VASTOperandList::op_iterator ChildIt;
  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;

  VisitStack.push_back(std::make_pair(Root, L->op_begin()));

  while (!VisitStack.empty()) {
    VASTValue *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == VASTOperandList::GetDatapathOperandList(Node)->op_end()) {
      VisitStack.pop_back();
      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->unwrap().get();
    ++VisitStack.back().second;

    if (VASTOperandList *L = VASTOperandList::GetDatapathOperandList(ChildNode)) {
      // ChildNode has a name means we had already visited it.
      if (!Visited.insert(L).second) continue;

      VisitStack.push_back(std::make_pair(ChildNode, L->op_begin()));
    }

    // If ChildNode is a not data-path operand list, it may be the SeqVal.
    if (VASTSeqValue *SeqVal = dyn_cast_or_null<VASTSeqValue>(ChildNode))
      SeqVals.insert(SeqVal);
  }

  return !SeqVals.empty();
}

VASTValue::VASTValue(VASTTypes T, unsigned BitWidth)
  : VASTNode(T), BitWidth(BitWidth) {
  assert(T >= vastFirstValueType && T <= vastLastValueType
    && "Bad DeclType!");
  UseList
    = reinterpret_cast<iplist<VASTUse>*>(::operator new(sizeof(iplist<VASTUse>)));
  new (UseList) iplist<VASTUse>();
}

VASTValue::~VASTValue() {
  // Do not call the destructor of UseList. They are deleted by the
  // VASTOperandList.
  // We should check if use list is empty if necessary.
  ::operator delete(UseList);
}

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

void VASTOutPort::print(vlang_raw_ostream &OS, const VASTModule *Mod) const {
  getSelector()->printRegisterBlock(OS, Mod, 0);
}

VASTInPort::VASTInPort(VASTWire *Wire) : VASTPort(vastInPort) {
  Contents.Wire = Wire;
}

VASTWire *VASTInPort::getValue() const {
  return Contents.Wire;
}

const char *VASTInPort::getNameImpl() const {
  return getValue()->getName();
}

unsigned VASTInPort::getBitWidthImpl() const {
  return getValue()->getBitWidth();
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
VASTModule::createSeqValue(VASTSelector *Selector, VASTSeqValue::Type T,
                           unsigned Idx, Value *V) {
  VASTSeqValue *SeqVal = new VASTSeqValue(Selector, T, Idx, V);
  SeqVals.push_back(SeqVal);
                              
  return SeqVal;
}

VASTMemoryBus *VASTModule::createDefaultMemBus() {
  VFUMemBus *Desc = getFUDesc<VFUMemBus>();
  return createMemBus(0, Desc->getAddrWidth(), Desc->getDataWidth());
}

VASTMemoryBus *VASTModule::createMemBus(unsigned Num, unsigned AddrWidth,
                                        unsigned DataWidth) {
  VASTMemoryBus *Bus = Datapath->getAllocator().Allocate<VASTMemoryBus>();
  new (Bus) VASTMemoryBus(Num, AddrWidth, DataWidth);
  Bus->addPorts(this);
  Submodules.push_back(Bus);
  return Bus;
}

VASTBlockRAM *VASTModule::addBlockRAM(unsigned BRamNum, unsigned Bitwidth,
                                      unsigned Size, const GlobalVariable *Init){
  VASTBlockRAM *RAM = new VASTBlockRAM("", BRamNum, Bitwidth, Size, Init);
  // Build the ports.
  RAM->addPorts(this);
  Submodules.push_back(RAM);
  return RAM;
}

VASTSubModule *VASTModule::addSubmodule(const char *Name, unsigned Num) {
  VASTSubModule *M = new VASTSubModule(Name, Num);
  Submodules.push_back(M);
  return M;
}

VASTWire *VASTModule::createWrapperWire(const Twine &Name, unsigned SizeInBits,
                                        VASTValPtr V) {
  Twine WrapperName = Name + "_wrapper";
  // Reuse the old wire if we had create one.
  VASTWire *W = lookupSymbol<VASTWire>(WrapperName);
  if (W == 0) {
    W = addWire(WrapperName, SizeInBits, true);
    if (V) W->assign(V);
  }

  return W;
}

VASTWire *VASTModule::createWrapperWire(GlobalVariable *GV, unsigned SizeInBits){
  std::string WrapperName = "gv_" + ShangMangle(GV->getName());
  VASTLLVMValue *ValueOp
    = new (Datapath->getAllocator()) VASTLLVMValue(GV, SizeInBits);
  return createWrapperWire(WrapperName, SizeInBits, ValueOp);
}

VASTUDef *VASTModule::createUDef(unsigned Size) {
  VASTUDef *&UDef = UDefMap[Size];

  if (UDef == 0) UDef = new VASTUDef(Size);

  return UDef;
}

VASTWire *VASTModule::addWire(const Twine &Name, unsigned BitWidth,
                              bool IsWrapper) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  assert(Entry.second == 0 && "Symbol already exist!");
  // Allocate the wire and the use.

  VASTWire *Wire = new VASTWire(Entry.getKeyData(), BitWidth, IsWrapper);
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

VASTSlot *VASTModule::createSlot(unsigned SlotNum, BasicBlock *ParentBB,
                                 VASTValPtr Pred, bool IsVirtual) {
  assert(std::find_if(Slots.begin(), Slots.end(), SlotNumEqual(SlotNum)) == Slots.end()
         && "The same slot had already been created!");

  VASTSlot *Slot = new VASTSlot(SlotNum, ParentBB, Pred, IsVirtual);
  // Insert the newly created slot before the finish slot.
  Slots.insert(Slots.back(), Slot);

  return Slot;
}

VASTSlot *VASTModule::createStartSlot() {
  VASTSlot *StartSlot = new VASTSlot(0, 0, VASTImmediate::True, false);
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


VASTModule::VASTModule(Function &F)
  : VASTNode(vastModule), Datapath(new DatapathContainer()),
    Ports(NumSpecialPort), Name(F.getName().str()), F(F),
    NumArgPorts(0) {
  createStartSlot();
}

void VASTModule::reset() {
  Wires.clear();
  Registers.clear();
  SeqOps.clear();
  SeqVals.clear();
  Slots.clear();

  // Release all ports.
  Ports.clear();
  Selectors.clear();
  SymbolTable.clear();
  NumArgPorts = 0;
  RetPortIdx = 0;

  // Release the datapath after all other contexts released.
  Datapath->reset();
  DeleteContainerSeconds(UDefMap);
  DeleteContainerPointers(Ports);
  DeleteContainerPointers(Submodules);
}

VASTModule::~VASTModule() {
  delete Datapath;
  DeleteContainerSeconds(UDefMap);
}

namespace {
struct DatapathPrinter {
  raw_ostream &OS;

  DatapathPrinter(raw_ostream &OS) : OS(OS) {}

  void operator()(VASTNode *N) const {
    if (VASTWire *W = dyn_cast<VASTWire>(N))  {
      // Declare the wire if necessary.
      W->printDecl(OS, false, "");

      if (VASTValPtr V= W->getDriver()) {
        OS << " = ";
        V.printAsOperand(OS);
      }

      OS << ";\n";
    }

    if (VASTExpr *E = dyn_cast<VASTExpr>(N))
      if (E->hasName()) {
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

        // Temporary unname the rexpression so that we can print its logic.
        E->nameExpr(false);
        E->printAsOperand(OS, false);
        E->nameExpr();

        OS << ";\n";
      }
  }
};
}

void VASTModule::printDatapath(raw_ostream &OS) const{
  std::set<VASTOperandList*> Visited;
  DatapathPrinter Printer(OS);

  for (const_slot_iterator SI = slot_begin(), SE = slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    typedef VASTSlot::const_op_iterator op_iterator;

    OS << "\n// At slot " << S->SlotNum;
    if (BasicBlock *BB = S->getParent()) OS << ", BB: " << BB->getName();
    OS << '\n';

    // Print the logic of slot ready and active.
    VASTOperandList::visitTopOrder(S->getActive(), Visited, Printer);

    // Print the logic of the datapath used by the SeqOps.
    for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
      VASTSeqOp *L = *I;

      typedef VASTOperandList::op_iterator op_iterator;
      for (op_iterator OI = L->op_begin(), OE = L->op_end(); OI != OE; ++OI) {
        VASTValue *V = OI->unwrap().get();
        VASTOperandList::visitTopOrder(V, Visited, Printer);
      }

      OS << "// ";
      L->print(OS);
    }
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
  typedef const_selector_iterator iterator;
  
  typedef RegisterVector::const_iterator const_reg_iterator;
  for (const_reg_iterator I = Registers.begin(), E = Registers.end();
       I != E; ++I)
    I->print(OS, this);

  for (const_port_iterator I = Ports.begin(), E = Ports.end(); I != E; ++I)
    if (VASTOutPort *P = dyn_cast<VASTOutPort>(*I))
      P->print(OS, this);
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

void VASTModule::printSignalDecl(raw_ostream &OS) const {
  typedef RegisterVector::const_iterator const_reg_iterator;
  for (const_reg_iterator I = Registers.begin(), E = Registers.end();
       I != E; ++I)
    I->printDecl(OS);

  typedef SubmoduleVector::const_iterator const_submod_iterator;
  for (const_submod_iterator I = Submodules.begin(),E = Submodules.end();
       I != E;++I)
    (*I)->printDecl(OS);
}

VASTSymbol *VASTModule::getOrCreateSymbol(const Twine &Name, unsigned BitWidth) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  VASTNode *&V = Entry.second;
  if (V == 0) {
    const char *S = Entry.getKeyData();
    unsigned SymbolWidth = BitWidth;
    V = new (Datapath->getAllocator()) VASTSymbol(S, SymbolWidth);
  }

  assert(cast<VASTSymbol>(V)->getBitWidth() == BitWidth
         && "Getting symbol with wrong bitwidth!");

  return cast<VASTSymbol>(V);
}

VASTWire *VASTModule::assign(VASTWire *W, VASTValPtr V) {
  // TODO: Replace the W by the new value.
  if (W->getDriver() != V) W->assign(V);

  return W;
}

VASTSeqInst *
VASTModule::latchValue(VASTSeqValue *SeqVal, VASTValPtr Src,  VASTSlot *Slot,
                       VASTValPtr GuardCnd, Value *V, unsigned Latency) {
  assert(Src && "Bad assignment source!");
  VASTSeqInst *Inst = lauchInst(Slot, GuardCnd, 1, V, VASTSeqInst::Latch);
  Inst->addSrc(Src, 0, SeqVal);
  Inst->setCyclesFromLaunch(Latency);

  return Inst;
}

VASTSeqInst *
VASTModule::lauchInst(VASTSlot *Slot, VASTValPtr Pred, unsigned NumOps, Value *V,
                      VASTSeqInst::Type T) {
  // Create the uses in the list.
  VASTSeqInst *SeqInst = new VASTSeqInst(V, Slot, NumOps, T);
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

void VASTModule::eraseSeqVal(VASTSeqValue *Val) {
  assert(Val->use_empty() && "Val still stuck at some user!");
  assert(Val->fanin_empty() && "Val still using something!");
  // Also erase the selector if it become empty.
  VASTSelector *Sel = Val->getSelector();
  if (Sel->empty()) {
    // Try to erase the corresponding register as well.
    if (VASTRegister *R = dyn_cast<VASTRegister>(Sel->getParent()))
      Registers.erase(R);

    Selectors.erase(Sel);
  }

  SeqVals.erase(Val);
}

void VASTModule::eraseSeqOp(VASTSeqOp *SeqOp) {
  assert(SeqOp->getSlot() == 0
         && "The VASTSeqOp should be erase from its parent slot first!");

  // Do not delete the dead SeqVal of VASTSeqSlotCtrl, their are not actually
  // assigning the Dst.
  if (SeqOp->getASTType() != VASTNode::vastSlotCtrl)
    for (unsigned i = 0, e = SeqOp->getNumSrcs(); i != e; ++i) {
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

VASTInPort *VASTModule::addInputPort(const Twine &Name, unsigned BitWidth,
                                     PortTypes T /*= Others*/) {
  VASTWire *Wire = addWire(Name, BitWidth, false);
  VASTPort *Port = createPort(Wire);

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
  VASTSelector *Sel = createSelector(Name, BitWidth, T == VASTModule::Finish, 0);
  VASTPort *Port = createPort(Sel);

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
                                         unsigned InitVal /* = 0 */,
                                         bool IsEnable /* = false */) {
  // Create the selector before we create the register.
  VASTSelector *Sel = createSelector(Name, BitWidth, IsEnable, 0);
  VASTRegister *Reg = new VASTRegister(Sel, InitVal);

  Registers.push_back(Reg);

  return Reg;
}

VASTSelector *VASTModule::createSelector(const Twine &Name, unsigned BitWidth,
                                         bool IsEnable, VASTNode *Parent) {
  // Allocate the entry from the symbol table.
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  // Make sure the each symbol is for 1 node only.
  assert(Entry.second == 0 && "Symbol had already taken!");
  VASTSelector *Sel
    = new VASTSelector(Entry.getKeyData(), BitWidth, IsEnable, Parent);
  Entry.second = Sel;
  // Put the newly create selector into the list, so that we can release it.
  Selectors.push_back(Sel);

  return Sel;
}

VASTPort *VASTModule::createPort(VASTNode *Node) {
  VASTPort *P = 0;

  // Create a output port for the selector.
  if (VASTSelector *Sel = dyn_cast<VASTSelector>(Node))
    P = new VASTOutPort(Sel);
  else {
    // Else it must be a wire for the input port.
    VASTWire *Wire = cast<VASTWire>(Node);
    P = new VASTInPort(Wire);
  }

  return P;
}

VASTPort *VASTModule::addPort(VASTNode *Node) {
  VASTPort *P = createPort(Node);
  Ports.push_back(P);
  return P;
}
