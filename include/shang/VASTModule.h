//===----------- VASTModule.h - Modules in VerilogAST -----------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the Modules in the Verilog Abstract Syntax Tree.
//
//===----------------------------------------------------------------------===//
#ifndef VTM_VAST_MODULE_H
#define VTM_VAST_MODULE_H

#include "shang/VASTNodeBases.h"
#include "shang/VASTDatapathNodes.h"
#include "shang/VASTSeqOp.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTSlot.h"

#include "shang/FUInfo.h"

#include "llvm/ADT/StringMap.h"

#include <map>

namespace llvm {
class Value;
class VASTWire;
class VASTRegister;
class VASTBlockRAM;
class VASTSubModule;
class VASTSeqInst;
class DatapathContainer;
class VASTExprBuilder;
class VASTMemoryBus;
class vlang_raw_ostream;

class VASTPort : public VASTNode {

public:
  const bool IsInput;

  VASTPort(VASTNamedValue *V, bool isInput);

  VASTNamedValue *getValue() const { return Contents.NamedValue; }
  VASTSeqValue *getSeqVal() const;

  const char *getName() const { return getValue()->getName(); }
  bool isInput() const { return IsInput; }
  bool isRegister() const;
  unsigned getBitWidth() const { return getValue()->getBitWidth(); }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTPort *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastPort;
  }

  void print(raw_ostream &OS) const;
  void printExternalDriver(raw_ostream &OS, uint64_t InitVal = 0) const;
  std::string getExternalDriverStr(unsigned InitVal = 0) const;
};

// The class that represent Verilog modulo.
class VASTModule : public VASTNode {
public:
  typedef SmallVector<VASTPort*, 16> PortVector;
  typedef PortVector::iterator port_iterator;
  typedef PortVector::const_iterator const_port_iterator;

  typedef ilist<VASTWire> WireVector;
  typedef WireVector::iterator wire_iterator;

  typedef SmallVector<VASTRegister*, 128> RegisterVector;
  typedef RegisterVector::iterator reg_iterator;

  typedef SmallVector<VASTSubModuleBase*, 16> SubmoduleVector;
  typedef SubmoduleVector::iterator submod_iterator;

  typedef ilist<VASTSeqValue> SeqValueVector;
  typedef SeqValueVector::iterator seqval_iterator;
  typedef SeqValueVector::const_iterator const_seqval_iterator;

  typedef ilist<VASTSlot> SlotVecTy;
  typedef SlotVecTy::iterator slot_iterator;
  typedef SlotVecTy::const_iterator const_slot_iterator;

  typedef DenseMap<unsigned, VASTUDef*> UDefMapTy;
  UDefMapTy UDefMap;

  enum PortTypes {
    Clk = 0,
    RST,
    Start,
    SpecialInPortEnd,
    Finish = SpecialInPortEnd,
    SpecialOutPortEnd,
    NumSpecialPort = SpecialOutPortEnd,
    ArgPort, // Ports for function arguments.
    Others,   // Likely ports for function unit.
    RetPort // Port for function return value.
  };
private:
  DatapathContainer *Datapath;
  WireVector Wires;

  // The slots vector, each slot represent a state in the FSM of the design.
  SlotVecTy Slots;
  SeqValueVector SeqVals;
  ilist<VASTSeqOp> SeqOps;

  // Input/Output ports of the design.
  PortVector Ports;
  // Wires and Registers of the design.
  // RegisterVector Registers;
  SubmoduleVector Submodules;

  typedef StringMap<VASTNamedValue*> SymTabTy;
  SymTabTy SymbolTable;
  typedef StringMapEntry<VASTNamedValue*> SymEntTy;

  // The Name of the Design.
  std::string Name;
  // The corresponding function for this module.
  Function &F;

  unsigned NumArgPorts, RetPortIdx;

  BumpPtrAllocator &getAllocator();

  VASTPort *createPort(const Twine &Name, unsigned BitWidth, bool isReg,
                       bool isInput, VASTSeqValue::Type T);
public:

  VASTModule(Function &F);

  ~VASTModule();

  void reset();

  const std::string &getName() const { return Name; }

  // Functions to generate verilog code.
  void printDatapath(raw_ostream &OS) const;
  void printRegisterBlocks(vlang_raw_ostream &OS) const;
  void printSubmodules(vlang_raw_ostream &OS) const;
  void printModuleDecl(raw_ostream &OS) const;
  void printSignalDecl(raw_ostream &OS);

  VASTValue *getSymbol(const Twine &Name) const {
    SymTabTy::const_iterator at = SymbolTable.find(Name.str());
    assert(at != SymbolTable.end() && "Symbol not found!");
    return at->second;
  }

  VASTValue *lookupSymbol(const Twine &Name) const {
    SymTabTy::const_iterator at = SymbolTable.find(Name.str());
    if (at == SymbolTable.end()) return 0;

    return at->second;
  }

  template<class T>
  T *lookupSymbol(const Twine &Name) const {
    return cast_or_null<T>(lookupSymbol(Name));
  }

  template<class T>
  T *getSymbol(const Twine &Name) const {
    return cast<T>(getSymbol(Name));
  }

  /// getOrCreateSymbol - Get the symbol with the specified name, create a new
  /// one if it does not exists.
  VASTNamedValue *getOrCreateSymbol(const Twine &Name, unsigned Bitwidth);

  VASTSlot *createSlot(unsigned SlotNum, BasicBlock *ParentBB,
                       VASTValPtr Pred = VASTImmediate::True,
                       bool IsVirtual = false);

  VASTSlot *createStartSlot();
  VASTSlot *getStartSlot();
  VASTSlot *getFinishSlot();
  const VASTSlot *getStartSlot() const;
  const VASTSlot *getFinishSlot() const;

  ilist<VASTSlot> &getSLotList() { return Slots; }

  operator DatapathContainer &() const { return *Datapath; }
  operator Function &() { return F; }
  DatapathContainer *operator->() const { return Datapath; }


  // Allow user to add ports.
  VASTPort *createPort(VASTSeqValue *SeqVal, bool IsInput, PortTypes T = Others);
  VASTPort *addInputPort(const Twine &Name, unsigned BitWidth,
                         PortTypes T = Others);

  VASTPort *addOutputPort(const Twine &Name, unsigned BitWidth,
                          PortTypes T = Others, bool isReg = true);

  // Get all ports of this moudle.
  const PortVector &getPorts() const { return Ports; }
  unsigned getNumPorts() const { return Ports.size(); }

  VASTPort &getPort(unsigned i) const {
    // FIXME: Check if out of range.
    return *Ports[i];
  }

  const char *getPortName(unsigned i) const {
    return getPort(i).getName();
  }

  port_iterator ports_begin() { return Ports.begin(); }
  const_port_iterator ports_begin() const { return Ports.begin(); }

  port_iterator ports_end() { return Ports.end(); }
  const_port_iterator ports_end() const { return Ports.end(); }

  // Argument ports and return port.
  const VASTPort &getArgPort(unsigned i) const {
    // FIXME: Check if out of range.
    return getPort(i + VASTModule::SpecialOutPortEnd);
  }

  unsigned getNumArgPorts() const { return NumArgPorts; }
  unsigned getRetPortIdx() const { return RetPortIdx; }
  VASTPort &getRetPort() const {
    assert(getRetPortIdx() && "No return port in this module!");
    return getPort(getRetPortIdx());
  }

  unsigned getNumCommonPorts() const {
    return getNumPorts() - VASTModule::SpecialOutPortEnd;
  }

  const VASTPort &getCommonPort(unsigned i) const {
    // FIXME: Check if out of range.
    return getPort(i + VASTModule::SpecialOutPortEnd);
  }

  port_iterator common_ports_begin() {
    return Ports.begin() + VASTModule::SpecialOutPortEnd;
  }
  const_port_iterator common_ports_begin() const {
    return Ports.begin() + VASTModule::SpecialOutPortEnd;
  }

  VASTSeqValue *createSeqValue(const Twine &Name, unsigned BitWidth,
                               VASTSeqValue::Type T, unsigned Idx,
                               VASTNode *Parent, uint64_t InitialValue = 0);

  VASTMemoryBus *createDefaultMemBus();
  VASTMemoryBus *createMemBus(unsigned Num, unsigned AddrWidth, unsigned DataWidth);

  VASTBlockRAM *addBlockRAM(unsigned BRamNum, unsigned Bitwidth, unsigned Size,
                            const GlobalVariable *Initializer);

  VASTSubModule *addSubmodule(const char *Name, unsigned Num);

  VASTSeqValue *addRegister(const Twine &Name, unsigned BitWidth,
                            unsigned InitVal = 0,
                            VASTSeqValue::Type T = VASTSeqValue::Data,
                            uint16_t RegData = 0, const char *Attr = "");

  VASTSeqValue *addIORegister(const Twine &Name, unsigned BitWidth,
                              unsigned FUNum, const char *Attr = "");

  VASTSeqValue *addDataRegister(const Twine &Name, unsigned BitWidth,
                                unsigned RegNum = 0, unsigned InitVal = 0,
                                const char *Attr = "");

  VASTWire *addWire(const Twine &Name, unsigned BitWidth,
                    const char *Attr = "", bool IsWrapper = false);
  VASTWire *createWrapperWire(const Twine &Name, unsigned SizeInBits,
                              VASTValPtr V = VASTValPtr());
  VASTWire *createWrapperWire(GlobalVariable *GV, unsigned SizeInBits);

  VASTUDef *createUDef(unsigned Size);

  slot_iterator slot_begin() { return Slots.begin(); }
  slot_iterator slot_end() { return Slots.end(); }

  const_slot_iterator slot_begin() const { return Slots.begin(); }
  const_slot_iterator slot_end() const { return Slots.end(); }

  void viewGraph() const;

  VASTWire *assign(VASTWire *W, VASTValPtr V);

  // Fine-grain Control-flow creation functions.
  // Create a SeqOp that contains NumOps operands, please note that the predicate
  // operand is excluded from NumOps.
  VASTSeqInst *lauchInst(VASTSlot *Slot, VASTValPtr Pred, unsigned NumOps,
                         Value *V, VASTSeqInst::Type T);

  VASTSeqInst *latchValue(VASTSeqValue *SeqVal, VASTValPtr Src, VASTSlot *Slot,
                          VASTValPtr GuardCnd, Value *V, unsigned Latency = 0);

  /// Create an assignment on the control logic.
  VASTSeqCtrlOp *assignCtrlLogic(VASTSeqValue *SeqVal, VASTValPtr Src,
                                 VASTSlot *Slot, VASTValPtr GuardCnd,
                                 bool UseSlotActive, bool ExportDefine = true);
  /// Create an assignment on the control logic which may need further conflict
  /// resolution.
  VASTSlotCtrl *createSlotCtrl(VASTNode *N, VASTSlot *Slot, VASTValPtr GuardCnd);

  /// Remove the VASTSeqOp from the module and delete it. Please note that
  /// the SeqOp should be remove from its parent slot before we erase it.
  void eraseSeqOp(VASTSeqOp *SeqOp);

  void eraseSeqVal(VASTSeqValue *Val);

  // Iterate over all SeqOps in the module.
  typedef ilist<VASTSeqOp>::iterator seqop_iterator;
  seqop_iterator seqop_begin() { return SeqOps.begin(); }
  seqop_iterator seqop_end() { return SeqOps.end(); }

  // Iterate over all SeqVals in the module.
  seqval_iterator seqval_begin()  { return SeqVals.begin(); }
  seqval_iterator seqval_end()    { return SeqVals.end(); }
  const_seqval_iterator seqval_begin() const { return SeqVals.begin(); }
  const_seqval_iterator seqval_end()   const { return SeqVals.end(); }
  unsigned num_seqvals() const { return SeqVals.size(); }

  void print(raw_ostream &OS) const;

  /// Perform the Garbage Collection to release the dead objects on the
  /// VASTModule
  void gc();

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTModule *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastModule;
  }

  static const std::string GetFinPortName() {
    return "fin";
  }
};
} // end namespace

#endif
