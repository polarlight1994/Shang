//===----------- VASTModule.h - Modules in VerilogAST -----------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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

#include "vast/VASTNodeBases.h"
#include "vast/VASTDatapathNodes.h"
#include "vast/VASTSeqValue.h"
#include "vast/VASTCtrlRgn.h"

#include "vast/FUInfo.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <map>
namespace llvm {
class Value;
}

namespace vast {
using namespace llvm;

class VASTWrapper;
class VASTRegister;
class VASTBlockRAM;
class VASTSubModule;
class VASTSeqInst;
class DatapathContainer;
class VASTExprBuilder;
class VASTMemoryBank;
class CachedStrashTable;
class vlang_raw_ostream;

class VASTPort : public VASTNode {

protected:
  VASTPort(VASTTypes Type);

  virtual const char *getNameImpl() const;
  virtual unsigned getBitWidthImpl() const;
public:
  virtual ~VASTPort();

  const char *getName() const { return getNameImpl(); }
  bool isInput() const { return getASTType() == vastInPort; }
  bool isRegister() const;
  unsigned getBitWidth() const { return getBitWidthImpl(); }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTPort *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastInPort || A->getASTType() == vastOutPort;
  }

  void print(raw_ostream &OS) const;
  void printExternalDriver(raw_ostream &OS, uint64_t InitVal = 0) const;
  std::string getExternalDriverStr(unsigned InitVal = 0) const;
};

class VASTOutPort : public VASTPort {
  const char *getNameImpl() const;
  unsigned getBitWidthImpl() const;
public:
  explicit VASTOutPort(VASTSelector *Sel);

  VASTSelector *getSelector() const;
  void print(vlang_raw_ostream &OS) const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTOutPort *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastOutPort;
  }
};

class VASTInPort : public VASTPort {
  const char *getNameImpl() const;
  unsigned getBitWidthImpl() const;
public:
  explicit VASTInPort(VASTNode *N);
  VASTWrapper *getValue() const;

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTInPort *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastInPort;
  }
};

// The class that represent Verilog modulo.
class VASTModule : public VASTCtrlRgn, public DatapathContainer {
public:
  typedef SmallVector<VASTPort*, 16> PortVector;
  typedef PortVector::iterator port_iterator;
  typedef PortVector::const_iterator const_port_iterator;

  typedef ilist<VASTWrapper> WireVector;
  typedef WireVector::iterator wire_iterator;

  typedef ilist<VASTRegister> RegisterVector;

  typedef ilist<VASTSubModuleBase> SubmoduleList;

  typedef ilist<VASTSelector> SelectorVector;
  typedef SelectorVector::iterator selector_iterator;
  typedef SelectorVector::const_iterator const_selector_iterator;

  typedef ilist<VASTSeqValue> SeqValueVector;
  typedef SeqValueVector::iterator seqval_iterator;
  typedef SeqValueVector::const_iterator const_seqval_iterator;

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
  WireVector Wires;

  // The slots vector, each slot represent a state in the FSM of the design.
  SelectorVector Selectors;
  RegisterVector Registers;
  SeqValueVector SeqVals;

  // Input/Output ports of the design.
  PortVector Ports;
  SubmoduleList Submodules;

  typedef StringMap<VASTNode*> SymTabTy;
  SymTabTy SymbolTable;
  typedef StringMapEntry<VASTNode*> SymEntTy;

  // The Name of the Design.
  std::string Name;
  // The placement constraint of the bounding box. The constraint is in the
  unsigned BBX, BBY, BBWidth, BBHeight;
  // The corresponding function for this module.
  Function *F;

  unsigned NumArgPorts, RetPortIdx;

  VASTPort *createPort(VASTNode *Node, bool IsInput);

  /// Perform the Garbage Collection to release the dead objects on the
  /// VASTModule
  bool gcImpl();

public:
  // TEMORARY HACK before hierarchy CFG is finished
  void setFunction(Function &F);
  Function &getLLVMFunction() { return *F; }

  VASTModule();

  ~VASTModule();

  void reset();

  const std::string &getName() const { return Name; }

  bool hasBoundingBoxConstraint() const {
    return BBX && BBY && BBWidth && BBHeight;
  }

  unsigned getBBX() const { return BBX; }
  unsigned getBBY() const { return BBY; }
  unsigned getBBWidth() const { return BBWidth; }
  unsigned getBBHeight() const { return BBHeight; }

  void setBoundingBoxConstraint(unsigned BBX, unsigned BBY, unsigned BBWidth,
                                unsigned BBHeight);

  // Functions to generate verilog code.
  void printDatapath(raw_ostream &OS, bool PrintSelfVerification) const;
  void printRegisterBlocks(vlang_raw_ostream &OS) const;
  void printRegisterBlocks(raw_ostream &OS) const;
  void printSubmodules(vlang_raw_ostream &OS) const;
  void printSubmodules(raw_ostream &OS) const;
  void printModuleDecl(raw_ostream &OS) const;
  void printSignalDecl(raw_ostream &OS) const;

  template<class T>
  T *lookupSymbol(const Twine &Name) const {
    SymTabTy::const_iterator at = SymbolTable.find(Name.str());
    if (at == SymbolTable.end()) return 0;

    return cast_or_null<T>(at->second);
  }

  // Allow user to add ports.
  VASTPort *addPort(VASTNode *Node, bool IsInput);
  VASTInPort *addInputPort(const Twine &Name, unsigned BitWidth,
                           PortTypes T = Others);

  VASTOutPort *addOutputPort(const Twine &Name, unsigned BitWidth,
                             PortTypes T = Others);

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

  VASTSelector *createSelector(const Twine &Name, unsigned BitWidth,
                               VASTNode *Parent,
                               VASTSelector::Type T = VASTSelector::Temp);
  VASTSeqValue *createSeqValue(VASTSelector *Selector, unsigned Idx, Value *V = 0);

  VASTMemoryBank *createDefaultMemBus();
  VASTMemoryBank *createMemBus(unsigned Num, unsigned AddrWidth,
                              unsigned DataWidth, bool RequireByteEnable,
                              bool IsDualPort, bool IsCombinationalROM,
                              unsigned ReadLatency);

  VASTSubModule *addSubmodule(const Twine &Name, unsigned Num);

  VASTRegister *createRegister(const Twine &Name, unsigned BitWidth,
                               unsigned InitVal = 0,
                               VASTSelector::Type T = VASTSelector::Temp);

  VASTWrapper *getOrCreateWrapper(const Twine &Name, unsigned BitWidth,
                                  Value* LLVMValue);
  VASTWrapper *getOrCreateWrapper(const Twine &Name, unsigned BitWidth,
                                  VASTNode* Node);

  selector_iterator selector_begin() { return Selectors.begin(); }
  selector_iterator selector_end() { return Selectors.end(); }

  const_selector_iterator selector_begin() const { return Selectors.begin(); }
  const_selector_iterator selector_end() const { return Selectors.end(); }

  /// Remove the VASTSeqOp from the module and delete it. Please note that
  /// the SeqOp should be remove from its parent slot before we erase it.
  void eraseSelector(VASTSelector *Sel);
  void eraseSeqVal(VASTSeqValue *Val);

  // Iterate over all SeqVals in the module.
  seqval_iterator seqval_begin()  { return SeqVals.begin(); }
  seqval_iterator seqval_end()    { return SeqVals.end(); }
  const_seqval_iterator seqval_begin() const { return SeqVals.begin(); }
  const_seqval_iterator seqval_end()   const { return SeqVals.end(); }
  unsigned num_seqvals() const { return SeqVals.size(); }

  void print(raw_ostream &OS) const;

  bool gc() {
    bool changed = false;

    // Iteratively release the dead objects.
    while (gcImpl())
      changed = true;

    return changed;
  }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTModule *A) LLVM_DELETED_FUNCTION;
  static inline bool classof(const VASTNode *A) LLVM_DELETED_FUNCTION;

  static const std::string GetFinPortName() {
    return "fin";
  }
};
} // end namespace

#endif
