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
  Contents64.Sel = Sel;
  Sel->setParent(this);
}

VASTSelector *VASTOutPort::getSelector() const {
  return Contents64.Sel;
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
  Contents64.Node = Node;
  if (VASTSelector *Sel = dyn_cast<VASTSelector>(Node))
    Sel->setParent(this);
}

VASTWrapper *VASTInPort::getValue() const {
  return cast<VASTWrapper>(Contents64.Node);
}

const char *VASTInPort::getNameImpl() const {
  if (VASTNamedValue *V = dyn_cast<VASTNamedValue>(Contents64.Node))
    return V->getName();

  return cast<VASTSelector>(Contents64.Node)->getName();
}

unsigned VASTInPort::getBitWidthImpl() const {
  if (VASTNamedValue *V = dyn_cast<VASTNamedValue>(Contents64.Node))
    return V->getBitWidth();

  return cast<VASTSelector>(Contents64.Node)->getBitWidth();
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
    OS << " = " << VASTConstant::buildLiteral(InitVal, getBitWidth(), false);

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
                      true, false, false, 0);
}

VASTMemoryBank *
VASTModule::createMemBus(unsigned Num, unsigned AddrWidth, unsigned DataWidth,
                         bool RequireByteEnable, bool IsDualPort,
                         bool IsCombinationalROM, unsigned ReadLatency) {
  VASTMemoryBank *Bus = new VASTMemoryBank(Num, AddrWidth, DataWidth,
                                          RequireByteEnable, IsDualPort,
                                          IsCombinationalROM, ReadLatency);
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

void VASTModule::setFunction(Function &F) {
  this->F = &F;
  Name = F.getName();
}

VASTModule::VASTModule()
  : VASTCtrlRgn(), Ports(NumSpecialPort), Name(), BBX(0), BBY(0),
    BBWidth(0), BBHeight(0), F(0), NumArgPorts(0) {}

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

  VASTCtrlRgn::finalize();

  // Now release the datapath.
  DatapathContainer::gc();
  DeleteContainerPointers(Ports);
}

namespace {
struct DatapathPrinter {
  raw_ostream &OS;
  std::set<VASTExpr*> Visited;
  std::set<unsigned> PrintedNames;
  const bool PrintSelfVerification;

  DatapathPrinter(raw_ostream &OS, bool PrintSelfVerification)
    : OS(OS), PrintSelfVerification(PrintSelfVerification) {}

  void print(VASTValPtr V) {
    if (VASTExpr *E = V.getAsLValue<VASTExpr>())
      E->visitConeTopOrder(Visited, *this);
  }

  void operator()(VASTNode *N) {
    VASTExpr *E = dyn_cast<VASTExpr>(N);

    if (E == NULL || !E->hasNameID())
      return;

    unsigned NameID = E->getNameID();
    // If the we get an expression with the same name as some other
    // expressions, do not print them as their are structural identical
    // expressions and one of them been printed before.
    if (!PrintedNames.insert(NameID).second)
      return;

    if (E->getOpcode() == VASTExpr::dpKeep)
      OS << "(* keep *) ";

    OS << "wire ";

    unsigned Bitwidth = E->getBitWidth();
    OS << VASTValue::BitRange(Bitwidth, 0, Bitwidth > 1) << ' ';

    E->printName(OS);

    if (E->isInstantiatedAsSubModule()) {
      OS << ";\n";
      if (E->printFUInstantiation(OS))
        return;
        
      // If the FU instantiation is not printed, we need to print the
      // assignment.
      OS << "assign ";
      E->printName(OS);
    }

    OS << " = ((";

    // Temporary unname the expression so that we can print its logic.
    E->assignNameID(0);
    E->printAsOperand(OS, false);
    // Mask away the known zeros, if there is any.
    if (E->anyKnownZero()) {
      APInt KnownZeros = E->getKnownZeros();
      SmallString<128> Str;
      (~KnownZeros).toString(Str, 2, false, false);
      OS << " & " << Bitwidth << "'b" << Str;
    }
    OS << ")";

    // Set the known ones, if there is any
    if (E->anyKnownOne()) {
      SmallString<128> Str;
      E->getKnownOnes().toString(Str, 2, false, false);
      OS << " | " << Bitwidth << "'b" << Str;
    }
    OS << ");\n";

    if (PrintSelfVerification)
      E->printMaskVerification(OS);

    E->assignNameID(NameID);
  }
};
}

void
VASTModule::printDatapath(raw_ostream &OS, bool PrintSelfVerification) const {
  std::set<VASTExpr*> Visited;
  DatapathPrinter Printer(OS, PrintSelfVerification);

  // Print the logic for the selectors.
  for (const_selector_iterator I = selector_begin(), E = selector_end();
       I != E; ++I) {
    const VASTSelector *Sel = I;

    if (!Sel->isSelectorSynthesized())
      continue;

    Printer.print(Sel->getGuard());
    Printer.print(Sel->getFanin());
  }

  if (!PrintSelfVerification)
    return;

  OS << "// synthesis translate_off\n";
  // Print the logic for the selectors.
  for (const_selector_iterator I = selector_begin(), E = selector_end();
    I != E; ++I) {
    const VASTSelector *Sel = I;

    typedef VASTSelector::const_iterator const_iterator;
    for (const_iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
      const VASTLatch &L = *I;
      Printer.print(L);
      Printer.print(L.getGuard());
      Printer.print(L.getSlotActive());
    }
  }
  OS << "// synthesis translate_on\n\n";
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

bool VASTModule::gcImpl() {
  bool Changed = false;

  // Clear up the dead VASTSeqValues.
  for (seqval_iterator I = seqval_begin(); I != seqval_end(); /*++I*/) {
    VASTSeqValue *V = I++;

    if (!V->use_empty()) continue;

    SmallVector<VASTLatch, 4> DeadOps(V->fanin_begin(), V->fanin_end());

    while (!DeadOps.empty()) {
      VASTLatch L = DeadOps.pop_back_val();
      if (L.Op->getNumDefs() == 1)
        L.Op->eraseFromParent();
      else
        L.eraseOperand();
    }

    eraseSeqVal(V);

    Changed |= true;
  }

  typedef SubmoduleList::iterator submodule_iterator;
  for (submodule_iterator I = Submodules.begin(), E = Submodules.end();
       I != E; ++I) {
    if (VASTCtrlRgn *R = dyn_cast<VASTCtrlRgn>(I))
      Changed |= R->gc();
  }

  Changed |= VASTCtrlRgn::gc();

  Changed |= DatapathContainer::gc();

  // At last clear up the dead VASTExprs.
  return Changed;
}
