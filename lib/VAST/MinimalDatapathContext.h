//===-- VASTModuleDatapathContext.h - Minimal Datapath Context --*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the Datapath builder context based on VASTModule.
//
//===----------------------------------------------------------------------===//
#ifndef SHANG_VASTMODULE_DATAPATH_CONTEXT_H
#define SHANG_VASTMODULE_DATAPATH_CONTEXT_H

#include "IR2Datapath.h"

namespace llvm {
class DatapathContainer;
class DataLayout;

class MinimalDatapathContext : public DatapathBuilderContext {
  DatapathContainer &Datapath;

public:
  MinimalDatapathContext(DatapathContainer &Datapath, DataLayout *TD);
  ~MinimalDatapathContext();

  using VASTExprBuilderContext::getConstant;

  VASTConstant *getConstant(const APInt &Value);

  virtual
  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned Bitwidth);
  virtual
  VASTValPtr createBitSlice(VASTValPtr Op, unsigned UB, unsigned LB);
  virtual
  VASTValPtr createROMLookUp(VASTValPtr Addr, VASTMemoryBank *Bank,
                             unsigned BitWidth);

  virtual VASTValPtr getAsOperandImpl(Value *Op);

  virtual void replaceAllUseWith(VASTValPtr From, VASTValPtr To);
};
}

#endif
