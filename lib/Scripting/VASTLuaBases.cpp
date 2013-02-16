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
#include "shang/VASTSubModules.h"
#include "shang/VASTModule.h"

#include "llvm/IR/Function.h"
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
  assert(UB && UB > LB && "Bad bit range!");
  --UB;
  if (UB != LB)
    ret = "[" + utostr_32(UB) + ":" + utostr_32(LB) + "]";
  else if(printOneBit)
    ret = "[" + utostr_32(LB) + "]";

  return ret;
}

void VASTValue::extractSupporingSeqVal(std::set<VASTSeqValue*> &SeqVals) {
  VASTValue *Root = this;

  std::set<VASTOperandList*> Visited;
  VASTOperandList *L = VASTOperandList::GetDatapathOperandList(Root);
  // The entire tree had been visited.
  if (!L) {
    // If ChildNode is a not data-path operand list, it may be the SeqVal.
    if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(Root))
      SeqVals.insert(SeqVal);

    return;
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
    if (VASTSeqValue *SeqVal = dyn_cast<VASTSeqValue>(ChildNode))
      SeqVals.insert(SeqVal);
  }
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
VASTPort::VASTPort(VASTNamedValue *V, bool isInput)
  : VASTNode(vastPort), IsInput(isInput)
{
  Contents.NamedValue = V;
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
VASTModule::createSeqValue(const Twine &Name, unsigned BitWidth,
                           VASTSeqValue::Type T, unsigned Idx, VASTNode *P) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  VASTSeqValue *V = new VASTSeqValue(Entry.getKeyData(), BitWidth, T, Idx, P);
  Entry.second = V;
  SeqVals.push_back(V);

  return V;
}

VASTBlockRAM *VASTModule::addBlockRAM(unsigned BRamNum, unsigned Bitwidth,
                                      unsigned Size, const GlobalVariable *Init){
  VASTBlockRAM *RAM
    = new (getAllocator()) VASTBlockRAM("", BRamNum, Bitwidth, Size, Init);
  // Build the ports.
  RAM->addPorts(this);
  Submodules.push_back(RAM);
  return RAM;
}

VASTSubModule *VASTModule::addSubmodule(const char *Name, unsigned Num) {
  VASTSubModule *M = new (getAllocator()) VASTSubModule(Name, Num);
  Submodules.push_back(M);
  return M;
}

VASTSeqCode *VASTModule::addSeqCode(const char *Name) {
  VASTSeqCode *Code = new (getAllocator()) VASTSeqCode(Name);
  SeqCode.push_back(Code);
  return Code;
}

VASTWire *VASTModule::addWire(const Twine &Name, unsigned BitWidth,
                              const char *Attr, bool IsPinned) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  assert(Entry.second == 0 && "Symbol already exist!");
  // Allocate the wire and the use.


  VASTWire *Wire = new VASTWire(Entry.getKeyData(), BitWidth, Attr, IsPinned);
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

VASTSlot *VASTModule::createSlot(unsigned SlotNum, BasicBlock *ParentBB) {
  assert(std::find_if(Slots.begin(), Slots.end(), SlotNumEqual(SlotNum)) == Slots.end()
         && "The same slot had already been created!");

  VASTSlot *Slot = new VASTSlot(SlotNum, ParentBB);
  // Insert the newly created slot before the finish slot.
  Slots.insert(Slots.back(), Slot);

  return Slot;
}

VASTSlot *VASTModule::createStartSlot() {
  VASTSlot *StartSlot = new VASTSlot(0, 0);
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
    FUPortOffsets(VFUs::NumCommonFUs), NumArgPorts(0) {
  createStartSlot();
}

void VASTModule::reset() {
  Wires.clear();
  SeqOps.clear();
  SeqVals.clear();
  Slots.clear();

  // Release all ports.
  Ports.clear();
  Registers.clear();
  SymbolTable.clear();
  FUPortOffsets.clear();
  NumArgPorts = 0;
  RetPortIdx = 0;

  // Release the datapath after all other contexts released.
  Datapath->reset();
}

VASTModule::~VASTModule() {
  delete Datapath;
}

template<typename T>
static raw_ostream &printDecl(raw_ostream &OS, T *V, bool declAsRegister,
                              const char *Terminator = ";\n") {
  if (declAsRegister)
    OS << "reg";
  else
    OS << "wire";

  if (V->getBitWidth() > 1)
    OS << "[" << (V->getBitWidth() - 1) << ":0]";

  OS << ' ' << V->getName();

  if (isa<VASTRegister>(V))
    OS << " = " << VASTImmediate::buildLiteral(0, V->getBitWidth(), false);

  OS << Terminator;

  return OS;
}

namespace {
struct DatapathPrinter {
  raw_ostream &OS;

  DatapathPrinter(raw_ostream &OS) : OS(OS) {}

  void operator()(VASTNode *N) const {
    if (VASTWire *W = dyn_cast<VASTWire>(N))  {
      // Declare the wire if necessary.
      printDecl(OS, W, false, "");

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

  for (const_slot_iterator SI = slot_begin(), SE = slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    typedef VASTSlot::const_op_iterator op_iterator;

    OS << "\n// At slot " << S->SlotNum;
    if (BasicBlock *BB = S->getParent()) OS << ", BB: " << BB->getName();
    OS << '\n';

    // Print the logic of slot ready and active.
    VASTOperandList::visitTopOrder(S->getActive(), Visited, DatapathPrinter(OS));

    // Print the logic of the datapath used by the SeqOps.
    for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
      VASTSeqOp *L = *I;

      typedef VASTOperandList::op_iterator op_iterator;
      for (op_iterator OI = L->op_begin(), OE = L->op_end(); OI != OE; ++OI) {
        VASTValue *V = OI->unwrap().get();
        VASTOperandList::visitTopOrder(V, Visited, DatapathPrinter(OS));
      }

      OS << "// ";
      L->print(OS);
    }
  }

  // Also print the driver of the wire outputs.
  for (const_port_iterator I = ports_begin(), E = ports_end(); I != E; ++I) {
    VASTPort *P = *I;

    if (P->isInput() || P->isRegister()) continue;
    VASTWire *W = cast<VASTWire>(P->getValue());
    VASTOperandList::visitTopOrder(W->getDriver().get(), Visited, DatapathPrinter(OS));
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

  typedef SeqCodeVector::const_iterator code_iterator;
  for (code_iterator I = SeqCode.begin(), E = SeqCode.end(); I != E; ++I) {
    VASTSeqCode *C = *I;

    // Print the sequential code.
    C->print(OS);
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

void VASTModule::printSignalDecl(raw_ostream &OS) {
  for (reg_iterator I = Registers.begin(), E = Registers.end(); I != E; ++I) {
    VASTRegister *R = *I;
    OS << R->AttrStr << ' ';
    printDecl(OS, R, true);
  }

  for (submod_iterator I = Submodules.begin(),E = Submodules.end();I != E;++I) {
    // Declare the output register of the block RAM.
    if (VASTBlockRAM *R = dyn_cast<VASTBlockRAM>(*I)) {
      printDecl(OS, R->getRAddr(0), true);
      continue;
    }

    if (VASTSubModule *S = dyn_cast<VASTSubModule>(*I)) {
      // Declare the output of submodule.
      if (VASTSeqValue *Ret = S->getRetPort())
        printDecl(OS, Ret, false);

      // Declare the finish signal of submodule.
      if (VASTSeqValue *Fin = S->getFinPort())
        printDecl(OS, Fin, false);
    }
  }
}

VASTNamedValue *VASTModule::getOrCreateSymbol(const Twine &Name,
                                              unsigned BitWidth) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  VASTNamedValue *&V = Entry.second;
  if (V == 0) {
    const char *S = Entry.getKeyData();
    // If we are going to create a wrapper, apply the bitwidth to the wrapper.
    unsigned SymbolWidth = BitWidth;
    V = new (getAllocator()) VASTSymbol(S, SymbolWidth);
  }

  assert(V->getBitWidth() == BitWidth && "Getting symbol with wrong bitwidth!");

  return V;
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
  Inst->addSrc(Src, 0, true, SeqVal);
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

void VASTModule::assignCtrlLogic(VASTSeqValue *SeqVal, VASTValPtr Src,
                                 VASTSlot *Slot, VASTValPtr GuardCnd,
                                 bool UseSlotActive,bool ExportDefine) {
  VASTSeqCtrlOp *CtrlOp = new VASTSeqCtrlOp(Slot, UseSlotActive);
  // Create the predicate operand.
  new (CtrlOp->Operands) VASTUse(CtrlOp, GuardCnd);
  // Create the source of the assignment
  CtrlOp->addSrc(Src, 0, ExportDefine, SeqVal);

  // Add the SeqOp to the the all SeqOp list.
  SeqOps.push_back(CtrlOp);
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

  // Also try to erase the parent registr.
  if (VASTRegister *R = dyn_cast<VASTRegister>(Val->getParent()))
    Registers.erase(std::find(reg_begin(), reg_end(), R));

  SeqVals.erase(Val);
}

void VASTModule::eraseSeqOp(VASTSeqOp *SeqOp) {
  assert(SeqOp->getSlot() == 0
         && "The VASTSeqOp should be erase from its parent slot first!");

  // Do not delete the dead SeqVal of VASTSeqSlotCtrl, their are not actually
  // assigning the Dst.
  if (SeqOp->getASTType() != VASTNode::vastSlotCtrl)
    for (unsigned i = 0, e = SeqOp->getNumSrcs(); i != e; ++i) {
      VASTSeqUse U = SeqOp->getSrc(i);
      VASTSeqValue *V = U.getDst();
      V->eraseUse(U);
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

VASTPort *VASTModule::addPort(const Twine &Name, unsigned BitWidth,
                              bool isReg, bool isInput, VASTSeqValue::Type T) {
  VASTNamedValue *V;
  if (isInput || isReg)
    V = addRegister(Name, BitWidth, 0, T, 0, "// ")->getValue();
  else
    V = addWire(Name, BitWidth, "// ", true);

  VASTPort *Port = new (getAllocator()) VASTPort(V, isInput);

  return Port;
}

VASTPort *VASTModule::addInputPort(const Twine &Name, unsigned BitWidth,
                                   PortTypes T /*= Others*/) {
  VASTPort *Port = addPort(Name, BitWidth, false, true, VASTSeqValue::IO);

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

VASTPort *VASTModule::addOutputPort(const Twine &Name, unsigned BitWidth,
                                    PortTypes T /*= Others*/,
                                    bool isReg /*= true*/) {
  VASTPort *Port = addPort(Name, BitWidth, isReg, false,
                           T == VASTModule::Finish ? VASTSeqValue::Enable
                                                   : VASTSeqValue::IO);

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

VASTRegister *VASTModule::addRegister(const Twine &Name, unsigned BitWidth,
                                      unsigned InitVal, VASTSeqValue::Type T,
                                      uint16_t RegData, const char *Attr) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  assert(Entry.second == 0 && "Symbol already exist!");
  VASTRegister *Reg = getAllocator().Allocate<VASTRegister>();
  VASTSeqValue *V = createSeqValue(Entry.getKeyData(), BitWidth, T, RegData, Reg);
  new (Reg) VASTRegister(V, InitVal, Attr);

  Registers.push_back(Reg);
  return Reg;
}

VASTRegister *VASTModule::addIORegister(const Twine &Name, unsigned BitWidth,
                                        unsigned FUNum, const char *Attr) {
  return addRegister(Name, BitWidth, 0, VASTSeqValue::IO, FUNum, Attr);
}

VASTRegister *VASTModule::addDataRegister(const Twine &Name, unsigned BitWidth,
                                          unsigned RegNum, const char *Attr) {
  return addRegister(Name, BitWidth, 0, VASTSeqValue::Data, RegNum, Attr);
}

BumpPtrAllocator &VASTModule::getAllocator() {
  return Datapath->getAllocator();
}
