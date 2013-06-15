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
protected:
  virtual void onReplaceAllUseWith(VASTValPtr From, VASTValPtr To);
public:
  MinimalDatapathContext(DatapathContainer &Datapath, DataLayout *TD);
  ~MinimalDatapathContext();

  using VASTExprBuilderContext::getOrCreateImmediate;

  VASTImmediate *getOrCreateImmediate(const APInt &Value);

  VASTValPtr createExpr(VASTExpr::Opcode Opc, ArrayRef<VASTValPtr> Ops,
                        unsigned UB, unsigned LB);

  virtual VASTValPtr getAsOperandImpl(Value *Op);

  virtual void replaceAllUseWith(VASTValPtr From, VASTValPtr To);
};
}

#endif
