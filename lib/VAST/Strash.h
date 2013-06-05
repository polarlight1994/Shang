//===-------- Strash.h - Structural Hash Table for Datapath Nodes ---------===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the interface of StrashTable, which calculate the ID of the
// nodes in datapath based on their "structure".
//
//===----------------------------------------------------------------------===//

#ifndef STRUCTRURAL_HASH_TABLE_H
#define STRUCTRURAL_HASH_TABLE_H

#include "shang/VASTNodeBases.h"
#include "shang/VASTModulePass.h"
#include "llvm/ADT/DenseMap.h"

namespace llvm {
struct Strash;
class CachedStrashTable : public VASTModulePass {
public:
  typedef DenseMap<VASTValPtr, unsigned> CacheTy;
private:
  CacheTy Cache;
  Strash *Table;
public:
  static char ID;

  CachedStrashTable();

  unsigned getOrCreateStrashID(VASTValPtr Ptr);

  void getAnalysisUsage(AnalysisUsage &AU) const;
  bool runOnVASTModule(VASTModule &VM);
  void releaseMemory();
};
}
#endif
