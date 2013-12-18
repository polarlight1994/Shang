//===---- VASTSubModules.h - Submodules in VerilogAST -----------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the Submodules in the Verilog Abstract Syntax Tree.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_SUBMODULES_H
#define VAST_SUBMODULES_H

#include "vast/VASTNodeBases.h"

#include "llvm/ADT/StringMap.h"

namespace llvm {
class GlobalVariable;
class VASTWrapper;
class VASTSelector;
class Instruction;

class VASTSubModule : public VASTSubModuleBase {
  // Remember the input/output flag in the pointer.
  // typedef PointerIntPair<VASTNode*, 1, bool> VASTSubModulePortPtr;
  // StringMap<VASTSubModulePortPtr> PortMap;

  // Remember the instructions that access this SubModule.
  SmallVector<Instruction*, 4> Insts;

  // Special ports in the submodule.
  VASTSelector *StartPort, *FinPort, *RetPort;

  // The latency of the submodule.
  unsigned Latency;

  VASTSubModule(const char *Name, unsigned FNNum)
    : VASTSubModuleBase(vastSubmodule, Name, FNNum), StartPort(0), FinPort(0),
      RetPort(0), Latency(0) {}

  friend class VASTModule;
  void printSimpleInstantiation(vlang_raw_ostream &OS) const;
  void printSubModuleLogic(vlang_raw_ostream &OS) const;
public:
  unsigned getNum() const { return Idx; }
  const char *getName() const { return Contents64.Name; }

  void addInstuction(Instruction *I) { Insts.push_back(I); }

  void addFanin(VASTSelector *S) {
    // FIXME: Build the port mapping.
    VASTSubModuleBase::addFanin(S);
  }

  VASTSelector *createStartPort(VASTModule *VM);
  VASTSelector *getStartPort() const { return StartPort; }

  VASTSelector *createFinPort(VASTModule *VM);
  VASTSelector *getFinPort() const { return FinPort; }

  VASTSelector *createRetPort(VASTModule *VM, unsigned Bitwidth,
                              unsigned Latency = 0);
  VASTSelector *getRetPort() const { return RetPort; }

  void printDecl(raw_ostream &OS) const;

  // Get the latency of the submodule.
  unsigned getLatency() const { return Latency; }

  static std::string getPortName(unsigned FNNum, const Twine &PortName);
  std::string getPortName(const Twine &PortName) const {
    return getPortName(getNum(), PortName);
  }

  void print(vlang_raw_ostream &OS) const;
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VASTSubModule *A) { return true; }
  static inline bool classof(const VASTNode *A) {
    return A->getASTType() == vastSubmodule;
  }
};
}
#endif
