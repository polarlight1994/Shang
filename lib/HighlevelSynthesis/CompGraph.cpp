//===----- CompGraph.cpp - Compatibility Graph for Binding ------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the interfaces of the Compatibility Graph. The
// compatibility graph represents the compatibilities between live intervals in
// the STG. Based on the compatibilities we can bind the variables with
// compatible live intervals to the same physical unit (register or functional
// unit).
//===----------------------------------------------------------------------===//

#include "CompGraph.h"

#include "shang/VASTSeqValue.h"

#include "llvm/Analysis/Dominators.h"

#define DEBUG_TYPE "shang-compatibility-graph"
#include "llvm/Support/Debug.h"
using namespace llvm;

void CompGraphNode::print(raw_ostream &OS) const {
  ::dump(Alives, OS);
}

void CompGraphNode::dump() const {
  print(dbgs());
}

void CompGraphNode::merge(const CompGraphNode *RHS, DominatorTree *DT) {
  Defs        |= RHS->Defs;
  Alives      |= RHS->Alives;
  Kills       |= RHS->Kills;
  DomBlock = DT->findNearestCommonDominator(DomBlock, RHS->DomBlock);
}

bool CompGraphNode::isCompatibleWith(const CompGraphNode *RHS) const {
  if (Sels.size() != RHS->Sels.size())
    return false;

  // Bitwidth should be the same.
  for (unsigned i = 0, e = size(); i != e; ++i)
    if (getSelector(i)->getBitWidth() != RHS->getSelector(i)->getBitWidth())
      return false;

  // Defines should not intersects.
  if (intersects(Defs, RHS->Defs))
    return false;

  // Defines and alives should not intersects.
  if (intersects(Defs, RHS->Alives))
    return false;

  if (intersects(Alives, RHS->Defs))
    return false;

  // Alives should not intersects.
  if (intersects(Alives, RHS->Alives))
    return false;

  // Kills should not intersects.
  // Not need to check the DefKills because they are a part of Defs.
  if (intersects(Kills, RHS->Kills))
    return false;

  // Kills and Alives should not intersects.
  // TODO: Ignore the dead slots.
  if (intersects(Kills, RHS->Alives))
    return false;

  if (intersects(Alives, RHS->Kills))
    return false;

  return true;
}

bool CompGraphNode::lt(CompGraphNode *Src, CompGraphNode *Dst,
                         DominatorTree *DT) {
  assert(!Src->isTrivial() && !Dst->isTrivial() && "Unexpected trivial node!");
  if (DT->properlyDominates(Src->DomBlock, Dst->DomBlock))
    return true;

  if (DT->properlyDominates(Dst->DomBlock, Src->DomBlock))
    return true;

  return Src < Dst;
}
