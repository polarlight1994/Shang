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
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "vast-lua-bases"
#include "llvm/Support/Debug.h"

using namespace llvm;

static cl::opt<unsigned>
ExprInlineThreshold("vtm-expr-inline-thredhold",
                    cl::desc("Inline the expression which has less than N "
                    "operand  (16 by default)"),
                    cl::init(2));
//----------------------------------------------------------------------------//
void VASTNode::dump() const {
  print(dbgs());
  dbgs() << '\n';
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

VASTModule::VASTModule(Function &F)
  : VASTNode(vastModule), Ports(NumSpecialPort), Name(F.getName().str()),
    FUPortOffsets(VFUs::NumCommonFUs), NumArgPorts(0), F(F) {
  createStartSlot();
}

VASTSeqValue *
VASTModule::createSeqValue(const Twine &Name, unsigned BitWidth,
                           VASTNode::SeqValType T, unsigned Idx, VASTNode *P) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  VASTSeqValue *V = new VASTSeqValue(Entry.getKeyData(), BitWidth, T, Idx, *P);
  Entry.second = V;
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

VASTSeqCode *VASTModule::addSeqCode(const char *Name) {
  VASTSeqCode *Code = new (Allocator) VASTSeqCode(Name);
  SeqCode.push_back(Code);
  return Code;
}

VASTWire *VASTModule::addWire(const Twine &Name, unsigned BitWidth,
                              const char *Attr, bool IsPinned) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  assert(Entry.second == 0 && "Symbol already exist!");
  // Allocate the wire and the use.
  void *P =  Allocator.Allocate(sizeof(VASTWire) + sizeof(VASTUse),
                                alignOf<VASTWire>());

  VASTWire *Wire = reinterpret_cast<VASTWire*>(P);
  // Create the uses in the list.
  VASTUse *U = reinterpret_cast<VASTUse*>(Wire + 1);
  new (Wire) VASTWire(Entry.getKeyData(), BitWidth, U, Attr, IsPinned);
  Entry.second = Wire;

  return Wire;
}

VASTValPtr VASTModule::getOrCreateSymbol(const Twine &Name,
                                         unsigned BitWidth,
                                         bool CreateWrapper) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  VASTNamedValue *&V = Entry.second;
  if (V == 0) {
    const char *S = Entry.getKeyData();
    // If we are going to create a wrapper, apply the bitwidth to the wrapper.
    unsigned SymbolWidth = CreateWrapper ? 0 : BitWidth;
    V = new (Allocator.Allocate<VASTSymbol>()) VASTSymbol(S, SymbolWidth);
    if (CreateWrapper) {
      // Create the wire for the symbol, and assign the symbol to the wire.
      VASTWire *Wire = addWire(VBEMangle(Name.str() + "_s"), BitWidth);
      Wire->assign(V);
      // Remember the wire.
      V = Wire;
    }
  }

  assert(V->getBitWidth() == BitWidth
          && "Getting symbol with wrong bitwidth!");

  return V;
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

  VASTSlot *Slot = new VASTSlot(SlotNum, ParentBB, this);
  // Insert the newly created slot before the finish slot.
  Slots.insert(Slots.back(), Slot);

  return Slot;
}

VASTSlot *VASTModule::createStartSlot() {
  VASTSlot *StartSlot = new VASTSlot(0, 0, this);
  Slots.push_back(StartSlot);
  // Also create the finish slot.
  Slots.push_back(new VASTSlot(0, StartSlot));
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

void VASTModule::reset() {
  DatapathContainer::reset();

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
}

VASTModule::~VASTModule() {}

namespace {
struct DatapathNamer {
  std::map<VASTExpr*, unsigned> &ExprSize;

  DatapathNamer(std::map<VASTExpr*, unsigned> &ExprSize) : ExprSize(ExprSize) {}

  void nameExpr(VASTExpr *Expr) const {
    // The size of named expression is 1.
    ExprSize[Expr] = 1;
    // Dirty hack: Do not name the MUX, they should be print with a wire.
    if (Expr->getOpcode() != VASTExpr::dpMux) Expr->nameExpr();
  }

  void operator()(VASTNode *N) const {
    VASTExpr *Expr = dyn_cast<VASTExpr>(N);

    if (Expr == 0) return;

    // Remove the naming, we will recalculate them.
    if (Expr->hasName()) Expr->unnameExpr();

    if (!Expr->isInlinable()) {
      nameExpr(Expr);
      return;
    }

    unsigned Size = 0;

    // Visit all the operand to accumulate the expression size.
    typedef VASTExpr::op_iterator op_iterator;
    for (op_iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
      if (VASTExpr *SubExpr = dyn_cast<VASTExpr>(*I)) {
        std::map<VASTExpr*, unsigned>::const_iterator at = ExprSize.find(SubExpr);
        assert(at != ExprSize.end() && "SubExpr not visited?");
        Size += at->second;
        continue;
      }

      Size += 1;
    }

    if (Size >= ExprInlineThreshold) nameExpr(Expr);
    else                             ExprSize[Expr] = Size;
  }
};
}

void VASTModule::nameDatapath() const{
  std::set<VASTOperandList*> Visited;
  std::map<VASTExpr*, unsigned> ExprSize;

  for (const_slot_iterator SI = slot_begin(), SE = slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    typedef VASTSlot::const_op_iterator op_iterator;

    // Print the logic of slot ready and active.
    VASTOperandList::visitTopOrder(S->getActive(), Visited, DatapathNamer(ExprSize));

    // Print the logic of the datapath used by the SeqOps.
    for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
      VASTSeqOp *L = *I;

      typedef VASTOperandList::op_iterator op_iterator;
      for (op_iterator OI = L->op_begin(), OE = L->op_end(); OI != OE; ++OI) {
        VASTValue *V = OI->unwrap().get();
        VASTOperandList::visitTopOrder(V, Visited, DatapathNamer(ExprSize));
      }
    }
  }

  // Also print the driver of the wire outputs.
  for (const_port_iterator I = ports_begin(), E = ports_end(); I != E; ++I) {
    VASTPort *P = *I;

    if (P->isInput() || P->isRegister()) continue;
    VASTWire *W = cast<VASTWire>(P->getValue());
    VASTOperandList::visitTopOrder(W, Visited, DatapathNamer(ExprSize));
  }
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
        E->unnameExpr();
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

    OS << "\n// At slot " << S->SlotNum << '\n';

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

VASTWire *VASTModule::assign(VASTWire *W, VASTValPtr V) {
  if (W->getDriver() != V) W->assign(V);

  return W;
}

void VASTModule::addAssignment(VASTSeqValue *V, VASTValPtr Src, VASTSlot *Slot,
                               VASTValPtr GuardCnd, Instruction *Inst,
                               bool AddSlotActive) {
  assert(Src && "Bad assignment source!");
  createSeqOp(Slot, GuardCnd, 1, Inst, AddSlotActive)->addSrc(Src, 0, true, V);
}

VASTSeqOp *VASTModule::createSeqOp(VASTSlot *Slot, VASTValPtr Pred,
                                   unsigned NumOps, Instruction *Inst,
                                   bool AddSlotActive) {
  VASTUse *UseBegin = Allocator.Allocate<VASTUse>(NumOps + 1);

  // Create the uses in the list.
  VASTSeqOp *Def = new VASTSeqOp(Slot, AddSlotActive, Inst, UseBegin, NumOps);
  // Create the predicate operand.
  new (UseBegin) VASTUse(Def, Pred);

  // Add the SeqOp to the the all SeqOp list.
  SeqOps.push_back(Def);

  return Def;
}

VASTSeqOp *VASTModule::createVirtSeqOp(VASTSlot *Slot, VASTValPtr Pred,
                                       VASTSeqValue *D, Instruction *Inst,
                                       bool AddSlotActive) {
  VASTUse *UseBegin = Allocator.Allocate<VASTUse>(1);

  // Create the uses in the list.
  VASTSeqOp *Def = new VASTSeqOp(Slot, AddSlotActive, Inst, UseBegin, 0, true);
  // Create the predicate operand.
  new (UseBegin) VASTUse(Def, Pred);
  // Add the defined value.
  if (D) Def->addDefDst(D);

  // Add the SeqOp to the the all SeqOp list.
  SeqOps.push_back(Def);

  return Def;
}

void VASTModule::print(raw_ostream &OS) const {
  for (const_slot_iterator SI = slot_begin(), SE = slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    S->print(OS);
  }
}

namespace llvm {
template <>
struct GraphTraits<const VASTModule*> : public GraphTraits<const VASTSlot*> {
  typedef VASTModule::const_slot_iterator nodes_iterator;
  static nodes_iterator nodes_begin(const VASTModule *G) {
    return G->slot_begin();
  }
  static nodes_iterator nodes_end(const VASTModule *G) {
    return G->slot_end();
  }
};


template<>
struct DOTGraphTraits<const VASTModule*> : public DefaultDOTGraphTraits{
  typedef const VASTSlot NodeTy;
  typedef const VASTModule GraphTy;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  std::string getNodeLabel(NodeTy *Node, GraphTy *Graph) {
    std::string Str;
    raw_string_ostream ss(Str);
    ss << Node->getName();
    DEBUG(Node->print(ss));
    return ss.str();
  }

  static std::string getNodeAttributes(NodeTy *Node, GraphTy *Graph) {
      return "shape=Mrecord";
  }
};
}

void VASTModule::viewGraph() const {
  ViewGraph(this, getName());
}

VASTPort *VASTModule::addPort(const Twine &Name, unsigned BitWidth,
                              bool isReg, bool isInput) {
  VASTNamedValue *V;
  if (isInput || isReg)
    V = addRegister(Name, BitWidth, 0, VASTNode::IO, 0, "// ")->getValue();
  else
    V = addWire(Name, BitWidth, "// ", true);

  VASTPort *Port = new (Allocator) VASTPort(V, isInput);

  return Port;
}

VASTPort *VASTModule::addInputPort(const Twine &Name, unsigned BitWidth,
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

VASTPort *VASTModule::addOutputPort(const Twine &Name, unsigned BitWidth,
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

VASTRegister *VASTModule::addRegister(const Twine &Name, unsigned BitWidth,
                                      unsigned InitVal, VASTNode::SeqValType T,
                                      uint16_t RegData, const char *Attr) {
  SymEntTy &Entry = SymbolTable.GetOrCreateValue(Name.str());
  assert(Entry.second == 0 && "Symbol already exist!");
  VASTRegister *Reg = Allocator.Allocate<VASTRegister>();
  VASTSeqValue *V = createSeqValue(Entry.getKeyData(), BitWidth, T, RegData, Reg);
  new (Reg) VASTRegister(V, InitVal, Attr);

  Registers.push_back(Reg);
  return Reg;
}

VASTRegister *VASTModule::addOpRegister(const Twine &Name, unsigned BitWidth,
                                        unsigned FUNum, const char *Attr) {
  return addRegister(Name, BitWidth, 0, VASTNode::Data, FUNum, Attr);
}

VASTRegister *VASTModule::addDataRegister(const Twine &Name, unsigned BitWidth,
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
  assert(!To->isDead() && "Replacing node by dead node!");
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
  // Do not use this node anymore.
  removeValueFromCSEMaps(From.get());
  // Sentence this Node to dead!
  From->setDead();
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
  VASTUse *UseBegin = reinterpret_cast<VASTUse*>(E + 1);

  for (unsigned i = 0; i < Ops.size(); ++i) {
    assert(Ops[i].get() && "Unexpected null VASTValPtr!");

    (void) new (UseBegin + i) VASTUse(E, Ops[i]);
  }

  UniqueExprs.InsertNode(E, IP);
  return E;
}

void DatapathContainer::reset() {
  UniqueExprs.clear();
  UniqueImms.clear();
  Allocator.Reset();

  // Reinsert the TRUE and False.
  UniqueImms.InsertNode(&True);
  UniqueImms.InsertNode(&False);
}

DatapathContainer::DatapathContainer() : True(APInt(1, 1)), False(APInt(1, 0)){
  UniqueImms.InsertNode(&True);
  UniqueImms.InsertNode(&False);
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
