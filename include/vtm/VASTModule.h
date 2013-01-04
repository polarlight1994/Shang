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

#include "vtm/VASTDatapathNodes.h"
#include "vtm/VASTControlPathNodes.h"
#include "vtm/LangSteam.h"
#include "vtm/FUInfo.h"
#include "vtm/VerilogBackendMCTargetDesc.h"

#include "llvm/ADT/StringMap.h"

namespace llvm {
class VASTBlockRAM : public VASTNode {
  unsigned Depth;
  unsigned WordSize;
  unsigned BRamNum;
public:
  VASTSeqValue WritePortA;
private:
  void printSelector(raw_ostream &OS, const VASTSeqValue &Port) const;
  void printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod,
                       const VASTSeqValue &Port) const;

  VASTBlockRAM(const char *Name, unsigned BRamNum, unsigned WordSize,
               unsigned Depth)
    : VASTNode(vastBlockRAM), Depth(Depth), WordSize(WordSize),
      BRamNum(BRamNum),
      WritePortA(Name, WordSize, VASTSeqValue::BRAM, BRamNum, *this)
  {}

  friend class VASTModule;
public:
  typedef VASTSeqValue::assign_itertor assign_itertor;
  unsigned getBlockRAMNum() const { return BRamNum; }

  unsigned getWordSize() const { return WordSize; }
  unsigned getDepth() const { return Depth; }

  void printSelector(raw_ostream &OS) const {
    printSelector(OS, WritePortA);
  }

  void printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod) const {
    printAssignment(OS, Mod, WritePortA);
  }

  void print(raw_ostream &OS) const {}

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTBlockRAM *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastBlockRAM;
  }

};

class VASTRegister : public VASTNode {
  VASTSeqValue Value;
  uint64_t InitVal;

  VASTRegister(const char *Name, unsigned BitWidth, uint64_t InitVal,
               VASTSeqValue::Type T = VASTSeqValue::Data, unsigned RegData = 0,
               const char *Attr = "");
  friend class VASTModule;

  void dropUses() {
    assert(0 && "Function not implemented!");
  }

public:
  const char *const AttrStr;

  VASTSeqValue *getValue() { return &Value; }
  VASTSeqValue *operator->() { return getValue(); }

  const char *getName() const { return Value.getName(); }
  unsigned getBitWidth() const { return Value.getBitWidth(); }

  typedef VASTSeqValue::assign_itertor assign_itertor;
  assign_itertor assign_begin() const { return Value.begin(); }
  assign_itertor assign_end() const { return Value.end(); }
  unsigned num_assigns() const { return Value.size(); }

  void printSelector(raw_ostream &OS) const;

  // Print data transfer between registers.
  void printAssignment(vlang_raw_ostream &OS, const VASTModule *Mod) const;
  // Return true if the reset is actually printed.
  bool printReset(raw_ostream &OS) const;
  void dumpAssignment() const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTRegister *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastRegister;
  }

  typedef VASTSeqValue::AndCndVec AndCndVec;
  static void printCondition(raw_ostream &OS, const VASTSlot *Slot,
                             const AndCndVec &Cnds);

  void print(raw_ostream &OS) const {}
};

class VASTPort : public VASTNode {

public:
  const bool IsInput;

  VASTPort(VASTNamedValue *V, bool isInput);

  VASTNamedValue *getValue() const { return Contents.Value; }
  VASTSeqValue *getSeqVal() const { return cast<VASTSeqValue>(getValue()); }

  const char *getName() const { return getValue()->getName(); }
  bool isInput() const { return IsInput; }
  bool isRegister() const { return !isInput() && !isa<VASTWire>(getValue()); }
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
class VASTModule : public VASTNode, public DatapathContainer {
public:
  typedef SmallVector<VASTPort*, 16> PortVector;
  typedef PortVector::iterator port_iterator;
  typedef PortVector::const_iterator const_port_iterator;

  typedef SmallVector<VASTWire*, 128> WireVector;
  typedef WireVector::iterator wire_iterator;

  typedef SmallVector<VASTRegister*, 128> RegisterVector;
  typedef RegisterVector::iterator reg_iterator;

  typedef SmallVector<VASTBlockRAM*, 16> BlockRAMVector;
  typedef BlockRAMVector::iterator bram_iterator;

  typedef SmallVector<VASTSeqValue*, 128> SeqValueVector;
  typedef SeqValueVector::iterator seqval_iterator;

  typedef std::vector<VASTSlot*> SlotVecTy;
  typedef SlotVecTy::iterator slot_iterator;
private:
  // Dirty Hack:
  // Buffers
  raw_string_ostream DataPath, ControlBlock;
  vlang_raw_ostream LangControlBlock;
  // The slots vector, each slot represent a state in the FSM of the design.
  SlotVecTy Slots;
  SeqValueVector SeqVals;

  // Input/Output ports of the design.
  PortVector Ports;
  // Wires and Registers of the design.
  WireVector Wires;
  RegisterVector Registers;
  BlockRAMVector BlockRAMs;

  typedef StringMap<VASTNamedValue*> SymTabTy;
  SymTabTy SymbolTable;
  typedef StringMapEntry<VASTNamedValue*> SymEntTy;

  // The Name of the Design.
  std::string Name;
  VASTExprBuilder *Builder;

  // The port starting offset of a specific function unit.
  SmallVector<std::map<unsigned, unsigned>, VFUs::NumCommonFUs> FUPortOffsets;
  unsigned NumArgPorts, RetPortIdx;

  VASTPort *addPort(const std::string &Name, unsigned BitWidth, bool isReg,
                    bool isInput);
public:
  static std::string DirectClkEnAttr, ParallelCaseAttr, FullCaseAttr;

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

  VASTModule(const std::string &Name, VASTExprBuilder *Builder)
    : VASTNode(vastModule),
    DataPath(*(new std::string())),
    ControlBlock(*(new std::string())),
    LangControlBlock(ControlBlock),
    Name(Name), Builder(Builder),
    FUPortOffsets(VFUs::NumCommonFUs),
    NumArgPorts(0) {
    Ports.append(NumSpecialPort, 0);
  }

  ~VASTModule();

  void setBuilder(VASTExprBuilder *Builder) {
    this->Builder = Builder;
  }

  void reset();

  const std::string &getName() const { return Name; }

  void printDatapath(raw_ostream &OS) const;
  void printRegisterBlocks(vlang_raw_ostream &OS) const;
  void printBlockRAMBlocks(vlang_raw_ostream &OS) const;

  // Print the slot control flow.
  void buildSlotLogic(VASTExprBuilder &Builder);
  void writeProfileCounters(VASTSlot *S, bool isFirstSlot);

  VASTValue *getSymbol(const std::string &Name) const {
    SymTabTy::const_iterator at = SymbolTable.find(Name);
    assert(at != SymbolTable.end() && "Symbol not found!");
    return at->second;
  }

  VASTValue *lookupSymbol(const std::string &Name) const {
    SymTabTy::const_iterator at = SymbolTable.find(Name);
    if (at == SymbolTable.end()) return 0;

    return at->second;
  }

  template<class T>
  T *lookupSymbol(const std::string &Name) const {
    return cast_or_null<T>(lookupSymbol(Name));
  }

  template<class T>
  T *getSymbol(const std::string &Name) const {
    return cast<T>(getSymbol(Name));
  }

  // Create wrapper to allow us get a bitslice of the symbol.
  VASTValPtr getOrCreateSymbol(const std::string &Name, unsigned BitWidth,
                               bool CreateWrapper);

  void allocaSlots(unsigned TotalSlots) {
    Slots.assign(TotalSlots, 0);
  }

  virtual void *Allocate(size_t Num, size_t Alignment){
    return Allocator.Allocate(Num, Alignment);
  }

  VASTSlot *getOrCreateSlot(unsigned SlotNum, MachineInstr *BundleStart);

  VASTSlot *getSlot(unsigned SlotNum) const {
    VASTSlot *S = Slots[SlotNum];
    assert(S && "Slot not exist!");
    return S;
  }

  VASTUse *allocateUse() { return Allocator.Allocate<VASTUse>(); }
  // Allow user to add ports.
  VASTPort *addInputPort(const std::string &Name, unsigned BitWidth,
                         PortTypes T = Others);

  VASTPort *addOutputPort(const std::string &Name, unsigned BitWidth,
                          PortTypes T = Others, bool isReg = true);

  void setFUPortBegin(FuncUnitId ID) {
    unsigned offset = Ports.size();
    std::pair<unsigned, unsigned> mapping
      = std::make_pair(ID.getFUNum(), offset);
    std::map<unsigned, unsigned> &Map = FUPortOffsets[ID.getFUType()];
    assert(!Map.count(mapping.first) && "Port begin mapping existed!");
    FUPortOffsets[ID.getFUType()].insert(mapping);
  }

  unsigned getFUPortOf(FuncUnitId ID) const {
    typedef std::map<unsigned, unsigned> MapTy;
    const MapTy &Map = FUPortOffsets[ID.getFUType()];
    MapTy::const_iterator at = Map.find(ID.getFUNum());
    assert(at != Map.end() && "FU do not existed!");
    return at->second;
  }

  const_port_iterator getFUPortItBegin(FuncUnitId ID) const {
    unsigned PortBegin = getFUPortOf(ID);
    return Ports.begin() + PortBegin;
  }

  void printModuleDecl(raw_ostream &OS) const;

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

  VASTBlockRAM *addBlockRAM(unsigned BRamNum, unsigned Bitwidth, unsigned Size);

  VASTRegister *addRegister(const std::string &Name, unsigned BitWidth,
                            unsigned InitVal = 0,
                            VASTSeqValue::Type T = VASTSeqValue::Data,
                            uint16_t RegData = 0, const char *Attr = "");

  VASTRegister *addOpRegister(const std::string &Name, unsigned BitWidth,
                              unsigned FUNum, const char *Attr = "") {
    return addRegister(Name, BitWidth, 0, VASTSeqValue::Data, FUNum, Attr);
  }

  VASTRegister *addDataRegister(const std::string &Name, unsigned BitWidth,
                                unsigned RegNum = 0, const char *Attr = "") {
    return addRegister(Name, BitWidth, 0, VASTSeqValue::Data, RegNum, Attr);
  }

  VASTWire *addWire(const std::string &Name, unsigned BitWidth,
                    const char *Attr = "", bool IsPinned = false);

  reg_iterator reg_begin() { return Registers.begin(); }
  reg_iterator reg_end() { return Registers.end(); }

  seqval_iterator seqval_begin()  { return SeqVals.begin(); }
  seqval_iterator seqval_end()    { return SeqVals.end(); }

  slot_iterator slot_begin() { return Slots.begin(); }
  slot_iterator slot_end() { return Slots.end(); }

  VASTWire *createAssignPred(VASTSlot *Slot, MachineInstr *DefMI);

  void addAssignment(VASTSeqValue *V, VASTValPtr Src, VASTSlot *Slot,
                     SmallVectorImpl<VASTValPtr> &Cnds, MachineInstr *DefMI = 0,
                     bool AddSlotActive = true);

  VASTWire *addPredExpr(VASTWire *CndWire, SmallVectorImpl<VASTValPtr> &Cnds,
                        bool AddSlotActive = true);

  VASTWire *assign(VASTWire *W, VASTValPtr V, VASTWire::Type T = VASTWire::Common);
  VASTWire *assignWithExtraDelay(VASTWire *W, VASTValPtr V, unsigned latency);

  void printSignalDecl(raw_ostream &OS);

  void print(raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTModule *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastModule;
  }

  vlang_raw_ostream &getControlBlockBuffer() {
    return LangControlBlock;
  }

  std::string &getControlBlockStr() {
    LangControlBlock.flush();
    return ControlBlock.str();
  }

  raw_ostream &getDataPathBuffer() {
    return DataPath;
  }

  std::string &getDataPathStr() {
    return DataPath.str();
  }

  // Out of line virtual function to provide home for the class.
  virtual void anchor();

  static const std::string GetMemBusEnableName(unsigned FUNum) {
    return VFUMemBus::getEnableName(FUNum) + "_r";
  }

  static const std::string GetFinPortName() {
    return "fin";
  }
};

} // end namespace

#endif
