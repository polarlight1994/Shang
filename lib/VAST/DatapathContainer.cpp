//===- DatapathContainer.cpp - Implement the datapath container -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the Datapath Container.
//
//===----------------------------------------------------------------------===//
#include "shang/VASTDatapathNodes.h"

#define DEBUG_TYPE "vast-datapath-container"
#include "llvm/Support/Debug.h"

using namespace llvm;

//----------------------------------------------------------------------------//
void DatapathContainer::removeValueFromCSEMaps(VASTNode *N) {
  if (VASTImmediate *Imm = dyn_cast<VASTImmediate>(N)) {
    UniqueImms->RemoveNode(Imm);
    return;
  }

  if (VASTExpr *Expr = dyn_cast<VASTExpr>(N)) {
    UniqueExprs->RemoveNode(Expr);
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
    addModifiedValueToCSEMaps(Imm, *UniqueImms);
    return;
  }

  if (VASTExpr *Expr = dyn_cast<VASTExpr>(N)) {
    addModifiedValueToCSEMaps(Expr, *UniqueExprs);
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
  // Do not use this node anymore.
  removeValueFromCSEMaps(From.get());
  // Sentence this Node to dead!
  From->setDead();
  // Delete From.
  if (VASTExpr *E = dyn_cast<VASTExpr>(From.get()))
    recursivelyDeleteTriviallyDeadExprs(E);
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
    ID.AddPointer(Ops[i].getProfilePtr());

  void *IP = 0;
  if (VASTExpr *E = UniqueExprs->FindNodeOrInsertPos(ID, IP))
    return E;

  VASTExpr *E = new VASTExpr(Opc, Ops.size(), UB, LB);

  for (unsigned i = 0; i < Ops.size(); ++i) {
    assert(Ops[i].get() && "Unexpected null VASTValPtr!");

    (void) new (E->Operands + i) VASTUse(E, Ops[i]);
  }

  UniqueExprs->InsertNode(E, IP);
  Exprs.push_back(E);
  return E;
}

void DatapathContainer::reset() {
  UniqueExprs->clear();
  UniqueImms->clear();
  Exprs.clear();
  Allocator.Reset();

  // Reinsert the TRUE and False.
  VASTImmediate::True = getOrCreateImmediateImpl(1, 1);
  VASTImmediate::False = getOrCreateImmediateImpl(0, 1);
}

DatapathContainer::DatapathContainer() {
  UniqueImms = new FoldingSet<VASTImmediate>();
  UniqueExprs = new FoldingSet<VASTExpr>();
  VASTImmediate::True = getOrCreateImmediateImpl(1, 1);
  VASTImmediate::False = getOrCreateImmediateImpl(0, 1);
}

DatapathContainer::~DatapathContainer() {
  delete UniqueExprs;
  delete UniqueImms;
}

VASTImmediate *DatapathContainer::getOrCreateImmediateImpl(const APInt &Value) {
  FoldingSetNodeID ID;

  Value.Profile(ID);

  void *IP = 0;
  if (VASTImmediate *V = UniqueImms->FindNodeOrInsertPos(ID, IP))
    return V;

  void *P = Allocator.Allocate(sizeof(VASTImmediate), alignOf<VASTImmediate>());
  VASTImmediate *V = new (P) VASTImmediate(Value);
  UniqueImms->InsertNode(V, IP);

  return V;
}

void DatapathContainer::recursivelyDeleteTriviallyDeadExprs(VASTExpr *E) {
  if (E == 0 || !E->use_empty()) return;

  SmallVector<VASTExpr*, 16> DeadExprs;
  DeadExprs.push_back(E);

  do {
    VASTExpr *E = DeadExprs.pop_back_val();

    // Null out all of the instruction's operands to see if any operand becomes
    // dead as we go.
    for (unsigned i = 0, e = E->size(); i != e; ++i) {
      VASTValue *V = E->getOperand(i).unwrap().get();
      E->getOperand(i).unlinkUseFromUser();

      // If the operand is an instruction that became dead as we nulled out the
      // operand, and if it is 'trivially' dead, delete it in a future loop
      // iteration.
      if (VASTExpr *Child = dyn_cast<VASTExpr>(V))
        if (Child->use_empty()) DeadExprs.push_back(Child);
    }

    // Remove the value from the CSEMap before erasing it.
    removeValueFromCSEMaps(E);

    Exprs.erase(E);
  } while (!DeadExprs.empty());
}
