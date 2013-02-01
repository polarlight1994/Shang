//===------------- VLang.h - Verilog HDL writing engine ---------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implement the VLang class, with provide funtions to complete
// common Verilog HDL writing task.
//
//===----------------------------------------------------------------------===//
#include "shang/VASTDatapathNodes.h"
#include "shang/VASTModule.h"
#include "shang/Utilities.h"

#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/CommandLine.h"
#define DEBUG_TYPE "vast-misc"
#include "llvm/Support/Debug.h"

#include <sstream>

using namespace llvm;

static cl::opt<unsigned>
ExprInlineThreshold("vtm-expr-inline-thredhold",
                    cl::desc("Inline the expression which has less than N "
                    "operand  (16 by default)"),
                    cl::init(2));

//===----------------------------------------------------------------------===//
// Value and type printing
std::string
VASTImmediate::buildLiteral(uint64_t Value, unsigned bitwidth, bool isMinValue) {
  std::string ret;
  ret = utostr_32(bitwidth) + '\'';
  if (bitwidth == 1) ret += "b";
  else               ret += "h";
  // Mask the value that small than 4 bit to prevent printing something
  // like 1'hf out.
  if (bitwidth < 4) Value &= (1 << bitwidth) - 1;

  if(isMinValue) {
    ret += utohexstr(Value);
    return ret;
  }

  std::string ss = utohexstr(Value);
  unsigned int uselength = (bitwidth/4) + (((bitwidth&0x3) == 0) ? 0 : 1);
  if(uselength < ss.length())
    ss = ss.substr(ss.length() - uselength, uselength);
  ret += ss;

  return ret;
}

void VASTImmediate::printAsOperandImpl(raw_ostream &OS, unsigned UB,
                                       unsigned LB) const {
  assert(UB == getBitWidth() && LB == 0 && "Cannot print bitslice of Expr!");
  OS << getBitWidth() << "'h" << Int.toString(16, false);
}

void VASTImmediate::Profile(FoldingSetNodeID& ID) const {
  Int.Profile(ID);
}

VASTSymbol::VASTSymbol(const char *Name, unsigned BitWidth)
  : VASTNamedValue(VASTNode::vastSymbol, Name, BitWidth) {}

VASTSignal::VASTSignal(VASTTypes DeclType, const char *Name, unsigned BitWidth)
  : VASTNamedValue(DeclType, Name, BitWidth) {}

void VASTSignal::anchor() const {}

VASTUse::VASTUse(VASTNode *U, VASTValPtr V) : User(*U), V(V) {
  linkUseToUser();
}

void VASTUse::unlinkUseFromUser() {
  get()->removeUseFromList(this);
}

void VASTUse::linkUseToUser() {
  if (VASTValue *Use = V.get()) {
    assert(Use != &User && "Unexpected cycle!");
    Use->addUseToList(this);
  }
}

bool VASTUse::operator==(const VASTValPtr RHS) const {
  return V == RHS;
}

void VASTUse::PinUser() const {
  if (VASTWire *S = getAsLValue<VASTWire>())
    S->Pin();
}

ArrayRef<VASTUse> VASTOperandList::getOperands() const {
  return ArrayRef<VASTUse>(Operands, Size);
}


VASTOperandList *VASTOperandList::GetDatapathOperandList(VASTNode *N) {
  if (VASTExpr *E = dyn_cast_or_null<VASTExpr>(N))
    return E;

  return dyn_cast_or_null<VASTWire>(N);
}

//===----------------------------------------------------------------------===//


namespace {
struct DatapathNamer {
  std::map<VASTExpr*, unsigned> &ExprSize;

  DatapathNamer(std::map<VASTExpr*, unsigned> &ExprSize) : ExprSize(ExprSize) {}

  void nameExpr(VASTExpr *Expr) const {
    // The size of named expression is 1.
    ExprSize[Expr] = 1;
    // Dirty hack: Do not name the MUX, they should be print with a wire.
    if (Expr->getOpcode() != VASTExpr::dpMux) Expr->nameExpr();
  }

  void operator()(VASTNode *N) const {
    VASTExpr *Expr = dyn_cast<VASTExpr>(N);

    if (Expr == 0) return;

    // Remove the naming, we will recalculate them.
    if (Expr->hasName()) Expr->unnameExpr();

    if (!Expr->isInlinable()) {
      nameExpr(Expr);
      return;
    }

    unsigned Size = 0;

    // Visit all the operand to accumulate the expression size.
    typedef VASTExpr::op_iterator op_iterator;
    for (op_iterator I = Expr->op_begin(), E = Expr->op_end(); I != E; ++I) {
      if (VASTExpr *SubExpr = dyn_cast<VASTExpr>(*I)) {
        std::map<VASTExpr*, unsigned>::const_iterator at = ExprSize.find(SubExpr);
        assert(at != ExprSize.end() && "SubExpr not visited?");
        Size += at->second;
        continue;
      }

      Size += 1;
    }

    if (Size >= ExprInlineThreshold) nameExpr(Expr);
    else                             ExprSize[Expr] = Size;
  }
};
}

void VASTModule::nameDatapath() const{
  std::set<VASTOperandList*> Visited;
  std::map<VASTExpr*, unsigned> ExprSize;

  for (const_slot_iterator SI = slot_begin(), SE = slot_end(); SI != SE; ++SI) {
    const VASTSlot *S = SI;

    typedef VASTSlot::const_op_iterator op_iterator;

    // Print the logic of slot ready and active.
    VASTOperandList::visitTopOrder(S->getActive(), Visited, DatapathNamer(ExprSize));

    // Print the logic of the datapath used by the SeqOps.
    for (op_iterator I = S->op_begin(), E = S->op_end(); I != E; ++I) {
      VASTSeqOp *L = *I;

      typedef VASTOperandList::op_iterator op_iterator;
      for (op_iterator OI = L->op_begin(), OE = L->op_end(); OI != OE; ++OI) {
        VASTValue *V = OI->unwrap().get();
        VASTOperandList::visitTopOrder(V, Visited, DatapathNamer(ExprSize));
      }
    }
  }

  // Also print the driver of the wire outputs.
  for (const_port_iterator I = ports_begin(), E = ports_end(); I != E; ++I) {
    VASTPort *P = *I;

    if (P->isInput() || P->isRegister()) continue;
    VASTWire *W = cast<VASTWire>(P->getValue());
    VASTOperandList::visitTopOrder(W, Visited, DatapathNamer(ExprSize));
  }
}

namespace llvm {
template <>
struct GraphTraits<const VASTModule*> : public GraphTraits<const VASTSlot*> {
  typedef VASTModule::const_slot_iterator nodes_iterator;
  static nodes_iterator nodes_begin(const VASTModule *G) {
    return G->slot_begin();
  }
  static nodes_iterator nodes_end(const VASTModule *G) {
    return G->slot_end();
  }
};


template<>
struct DOTGraphTraits<const VASTModule*> : public DefaultDOTGraphTraits{
  typedef const VASTSlot NodeTy;
  typedef const VASTModule GraphTy;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  std::string getNodeLabel(NodeTy *Node, GraphTy *Graph) {
    std::string Str;
    raw_string_ostream ss(Str);
    ss << Node->getName();
    DEBUG(Node->print(ss));
    return ss.str();
  }

  static std::string getNodeAttributes(NodeTy *Node, GraphTy *Graph) {
      return "shape=Mrecord";
  }
};
}

void VASTModule::viewGraph() const {
  ViewGraph(this, getName());
}
