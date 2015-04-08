//===----------- SIRSubModuleBase.h - SubModules in SIR ---------*- C++ -*-===//
//
//                       The SIR HLS framework                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the classes for sub-modules in SIR.
//
//===----------------------------------------------------------------------===//

#ifndef SIR_SUB_MODULE_BASE_H
#define SIR_SUB_MODULE_BASE_H

#include "sir/SIR.h"

#include "llvm/ADT/None.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Casting.h"

namespace llvm {
class SIRSubModuleBase : public ilist_node<SIRSubModuleBase> {
	SmallVector<SIRRegister *, 8> Fanins;
	SmallVector<SIRRegister *, 8> Fanouts;
};
}

#endif
