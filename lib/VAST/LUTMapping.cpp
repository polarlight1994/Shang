//===- LUTMapping.cpp - Perform LUT mapping on the RTL Netlist  -*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implementent the VerilogAST to LUT mapper. The mapper map the
// boalean expressions in the VerilogAST to LUTs with ABC logic synthesis.
//
//===----------------------------------------------------------------------===//
#include "MinimalDatapathContext.h"

#include "shang/VASTModule.h"
#include "shang/VASTModulePass.h"
#include "shang/Utilities.h"
#include "shang/FUInfo.h"
#include "shang/Passes.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-logic-synthesis"
#include "llvm/Support/Debug.h"

STATISTIC(NumAndExpand, "Number of binary And expanded from NAry And expanded");
STATISTIC(NumABCNodeBulit, "Number of ABC node built");
STATISTIC(NumLUTBulit, "Number of LUT node built");

// The header of ABC
#define ABC_DLL

#include "base/main/main.h"
#include "map/fpga/fpga.h"
extern "C" {
  extern Abc_Ntk_t *Abc_NtkFpga(Abc_Ntk_t *pNtk, float DelayTarget,
                                int fRecovery, int fSwitching, int fLatchPaths,
                                int fVerbose);
}

using namespace llvm;

static cl::opt<bool>
ExpandLUT("shang-lut-mapping-expand",
  cl::desc("Expand the lookup-table generated by ABC (For Debug Only)"),
  cl::init(false));

namespace {
struct ABCContext {
  ABCContext() {
    Abc_Start();
    // FIXME: Set complex library?
    Fpga_SetSimpleLutLib(VFUs::MaxLutSize);
  }

  ~ABCContext() {
    Abc_Stop();
  }
};

struct LogicNetwork {
  ABCContext &Context;

  Abc_Ntk_t *Ntk;
  VASTModule &VM;

  LogicNetwork(VASTModule &VM);

  ~LogicNetwork() {
    Abc_NtkDelete(Ntk);
  }

  // Map VASTValue to Abc_Obj_t for AIG construction.
  typedef std::map<VASTValue*, Abc_Obj_t*> ValueMapTy;
  // Nodes.
  ValueMapTy Nodes;

  // Map the Abc_Obj_t name to Instruction.
  typedef StringMap<VASTValPtr> ABCNameMapTy;
  ABCNameMapTy ValueNames;

  // Map the Abc_Obj_t to VASTValue for VAST datapath rewriting.
  typedef std::map<Abc_Obj_t*, VASTValPtr> AbcObjMapTy;
  AbcObjMapTy RewriteMap;

  // Convert a value to shortest string, the string must not containing \0 in
  // the middle. This can simply done by converting th value to 255-based digits
  // string and increase each digit by 1.
  static inline SmallString<9> &intToStr(uint64_t V, SmallString<9> &S) {
    assert(V && "Cannot convert 0 yet!");
    while (V) {
      unsigned char Digit = V % 255;
      Digit += 1;
      S += Digit;
      V /= 255;
    }

    return S;
  }

  Abc_Obj_t *getObj(VASTValue *V);
  Abc_Obj_t *getOrCreateObj(VASTValue *V);

  bool buildAIG(VASTExpr *E);
  bool buildAIG(VASTValue *Root, std::set<VASTValue*> &Visited);
  bool buildAIG(DatapathContainer &DP);

  VASTValPtr getAsOperand(Abc_Obj_t *O) const;

  bool isNodeVisited(Abc_Obj_t *Obj) const {
    return RewriteMap.count(Obj) || Abc_ObjIsPi(Abc_ObjFanin0(Obj));
  }

  VASTValPtr expandSOP(const char *sop, ArrayRef<VASTValPtr> Ops,
                       unsigned Bitwidth, DatapathBuilder &Builder);
  void buildLUTExpr(Abc_Obj_t *Obj, DatapathBuilder &Builder);
  void buildLUTTree(Abc_Obj_t *Root, DatapathBuilder &Builder);
  void buildLUTDatapath(DatapathBuilder &Builder);

  bool hasExternalUse(VASTValue * V) {
    typedef VASTValue::use_iterator use_iterator;
    for (use_iterator UI = V->use_begin(), UE = V->use_end(); UI != UE; ++UI) {
      if (VASTValue *U = dyn_cast<VASTValue>(*UI))
        if (getObj(U)) continue;

      // Any use that not have a corresponding logic network object is a
      // external use.
      return true;
    }

    return false;
  }

  void cleanUp();


  // Call abc routine to synthesis the logic network.
  void synthesis();

  void performLUTMapping();

  // Debug functions.
  void print(raw_ostream &OS) const {
    int i;
    Abc_Obj_t *Obj;

    Abc_NtkForEachNode(Ntk, Obj, i) {
      OS << "Node: " << Abc_ObjName(Abc_ObjRegular(Obj))
        << " Id: " << Abc_ObjId(Abc_ObjRegular(Obj)) << " FI: {";

      Abc_Obj_t *FI;
      int j;

      Abc_ObjForEachFanin(Obj, FI, j) {
        OS << Abc_ObjName(Abc_ObjRegular(FI)) << ", ";
      }

      OS << "} FO: " << Abc_ObjName(Abc_ObjRegular(Abc_ObjFanout0(Obj))) << '\n';
    }
  }

  void dump() const {
    print(dbgs());
  }
};
}

bool LogicNetwork::buildAIG(VASTValue *Root, std::set<VASTValue*> &Visited) {
  typedef VASTValue::dp_dep_it ChildIt;
  std::vector<std::pair<VASTValue*, ChildIt> > VisitStack;
  bool AnyNodeCreated = false;

  VisitStack.push_back(std::make_pair(Root, VASTValue::dp_dep_begin(Root)));

  while (!VisitStack.empty()) {
    VASTValue *Node = VisitStack.back().first;
    ChildIt It = VisitStack.back().second;

    // We have visited all children of current node.
    if (It == VASTValue::dp_dep_end(Node)) {
      VisitStack.pop_back();

      // Break down the current expression.
      if (VASTExpr *E = dyn_cast<VASTExpr>(Node))
        AnyNodeCreated |= buildAIG(E);

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    VASTValue *ChildNode = It->getAsLValue<VASTValue>();
    ++VisitStack.back().second;

    // Do not visit the same value twice.
    if (!Visited.insert(ChildNode).second) continue;

    if (!isa<VASTExpr>(ChildNode)) continue;

    VisitStack.push_back(std::make_pair(ChildNode,
                                        VASTValue::dp_dep_begin(ChildNode)));
  }

  return AnyNodeCreated;
}

bool LogicNetwork::buildAIG(DatapathContainer &DP) {
  // Remember the visited values so that we wont visit the same value twice.
  std::set<VASTValue*> Visited;
  bool AnyNodeCreated = false;

  typedef DatapathContainer::expr_iterator expr_iterator;
  for (expr_iterator I = DP.expr_begin(), E = DP.expr_end(); I != E; ++I)
    AnyNodeCreated |= buildAIG(I, Visited);

  return AnyNodeCreated;
}

void LogicNetwork::synthesis() {
  // FIXME: Do not synthesis if the network is very small.
  // FIXME: Call dispatch command to run user script?
  int res;
  // Use the resyn flow, which invoking:
  //  balance
  Ntk = Abc_NtkBalance(Ntk, false, false, false);
  //  rewrite
  res = Abc_NtkRewrite(Ntk, 0, 0, 0, 0, 0);
  assert(res && "Rewrite fail during logic synthesis!");
  //  rewrite -z
  res = Abc_NtkRewrite(Ntk, 0, 1, 0, 0, 0);
  assert(res && "Rewrite fail during logic synthesis!");
  //  balance
  Ntk = Abc_NtkBalance(Ntk, false, false, false);
  //  rewrite -z
  res = Abc_NtkRewrite(Ntk, 0, 1, 0, 0, 0);
  assert(res && "Rewrite fail during logic synthesis!");
  //  balance
  Ntk = Abc_NtkBalance(Ntk, false, false, false);
}

void LogicNetwork::performLUTMapping() {
  // Map the network to LUTs
  Ntk = Abc_NtkFpga(Ntk, 1, 0, 0, 0, 0);
  assert(Ntk && "Fail to perform LUT mapping!");

  // Translate the network to netlist.
  Ntk = Abc_NtkToNetlist(Ntk);
  assert(Ntk && "Network doese't exist!!!");
  assert(Abc_NtkHasBdd(Ntk) && "Expect Bdd after LUT mapping!");
  int res = Abc_NtkBddToSop(Ntk, 0);
  assert(res && "BddToSop fail!");
  (void) res;
}

void LogicNetwork::cleanUp() {
  // Build the POs
  typedef ValueMapTy::iterator node_iterator;

  for (node_iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
    VASTValue *V = I->first;
    Abc_Obj_t *&Obj = I->second;

    if (Abc_ObjIsPi(Obj) || !hasExternalUse(V)) continue;

    // Connect the node which is used by the node outside the network to a PO.
    Abc_Obj_t *PO = Abc_NtkCreatePo(Ntk);
    Abc_ObjAddFanin(PO, Obj);
    Obj = PO;

    SmallString<9> S;
    intToStr(Abc_ObjId(Abc_ObjRegular(Obj)), S);
    // DirtyHack: Terminate the string manually.
    S.push_back(0);
    Abc_ObjAssignName(Obj, S.data(), 0);

    // Remember the MO.
    ValueNames.GetOrCreateValue(Abc_ObjName(Abc_ObjRegular(Obj)), V);
  }

  // Clean up the aig.
  Abc_AigCleanup((Abc_Aig_t *)Ntk->pManFunc);

  // Create default names.
  //Abc_NtkAddDummyPiNames(Ntk);
  //Abc_NtkAddDummyPoNames(Ntk);
  // We do not have boxes.
  //Abc_NtkAddDummyBoxNames(Ntk);

  // Check the Aig
  assert(Abc_NtkCheck(Ntk) && "The AIG construction has failed!");
}

Abc_Obj_t *LogicNetwork::getObj(VASTValue *V) {
  ValueMapTy::iterator at = Nodes.find(V);

  if (at != Nodes.end()) return at->second;

  return 0;
}

Abc_Obj_t *LogicNetwork::getOrCreateObj(VASTValue *V) {
  Abc_Obj_t *Obj = 0;

  if (VASTImmediate *Imm = dyn_cast<VASTImmediate>(V)) {
    if (Imm->isAllOnes())
      Obj = Abc_AigConst1(Ntk);
    else if (Imm->isAllZeros())
      Obj = Abc_ObjNot(Abc_AigConst1(Ntk));

    // If we can handle the constant...
    if (Obj) return Obj;
  }

  Obj = getObj(V);

  // Object not existed, create a PI for the MO now.
  if (Obj == 0) {
    Obj = Abc_NtkCreatePi(Ntk);

    SmallString<9> S;
    intToStr(Abc_ObjId(Abc_ObjRegular(Obj)), S);
    // DirtyHack: Terminate the string manually.
    S.push_back(0);
    Abc_ObjAssignName(Obj, S.data(), 0);
    char *Name = Abc_ObjName(Abc_ObjRegular(Obj));

    // Map the PI to VASTValue.
    ValueNames.GetOrCreateValue(Name, V);
    Nodes.insert(std::make_pair(V, Obj));
  }

  return Obj;
}

bool LogicNetwork::buildAIG(VASTExpr *E) {
  // Only handle the boolean expressions.
  if (E->getOpcode() != VASTExpr::dpAnd) return false;

  assert(E->size() == 2 && "Bad Operand number!");

  VASTValPtr LHS = E->getOperand(0), RHS = E->getOperand(1);
  Abc_Obj_t *LHS_OBJ = getOrCreateObj(LHS.get()),
            *RHS_OBJ = getOrCreateObj(RHS.get());

  if (LHS.isInverted()) LHS_OBJ = Abc_ObjNot(LHS_OBJ);
  if (RHS.isInverted()) RHS_OBJ = Abc_ObjNot(RHS_OBJ);

  Abc_Obj_t *AndObj = Abc_AigAnd((Abc_Aig_t *)Ntk->pManFunc, LHS_OBJ, RHS_OBJ);
  Nodes.insert(std::make_pair(E, AndObj));

  ++NumABCNodeBulit;

  return true;
}

VASTValPtr LogicNetwork::getAsOperand(Abc_Obj_t *O) const {
  // Try to look up the VASTValue in the Fanin map.
  char *Name = Abc_ObjName(Abc_ObjRegular(O));

  VASTValPtr V = ValueNames.lookup(Name);
  if (V) {
    if (Abc_ObjIsComplement(O)) V = V.invert();

    return V;
  }

  // Otherwise this value should be rewritten.
  AbcObjMapTy::const_iterator at = RewriteMap.find(Abc_ObjRegular(O));
  assert(at != RewriteMap.end() && "Bad Abc_Obj_t visiting order!");

  return at->second;
}

VASTValPtr LogicNetwork::expandSOP(const char *sop, ArrayRef<VASTValPtr> Ops,
                                   unsigned Bitwidth, DatapathBuilder &Builder) {
  unsigned NInput = Ops.size();
  const char *p = sop;
  SmallVector<VASTValPtr, 8> ProductOps, SumOps;
  bool isComplement = false;

  while (*p) {
    // Interpret the product.
    ProductOps.clear();
    for (unsigned i = 0; i < NInput; ++i) {
      char c = *p++;
      switch (c) {
      default: llvm_unreachable("Unexpected SOP char!");
      case '-': /*Dont care*/ break;
      case '1': ProductOps.push_back(Ops[i]); break;
      case '0':
        ProductOps.push_back(Builder.buildNotExpr(Ops[i]));
        break;
      }
    }

    // Inputs and outputs are seperated by blank space.
    assert(*p == ' ' && "Expect the blank space!");
    ++p;

    // Create the product.
    // Add the product to the operand list of the sum.
    SumOps.push_back(Builder.buildAndExpr(ProductOps, Bitwidth));

    // Is the output inverted?
    char c = *p++;
    assert((c == '0' || c == '1') && "Unexpected SOP char!");
    isComplement = (c == '0');

    // Products are separated by new line.
    assert(*p == '\n' && "Expect the new line!");
    ++p;
  }

  // Or the products together to build the SOP (Sum of Product).
  VASTValPtr SOP = Builder.buildOrExpr(SumOps, Bitwidth);

  if (isComplement) SOP = Builder.buildNotExpr(SOP);

  return SOP;
}

void LogicNetwork::buildLUTExpr(Abc_Obj_t *Obj, DatapathBuilder &Builder) {
  SmallVector<VASTValPtr, 4> Ops;
  Abc_Obj_t *FO = Abc_ObjFanout0(Obj);
  unsigned Bitwidth = 0;

  assert(Abc_ObjIsNode(Obj) && "Unexpected Obj Type!");

  Abc_Obj_t *FI;
  int j;
  Abc_ObjForEachFanin(Obj, FI, j) {
    DEBUG(dbgs() << "\tBuilt MO for FI: " << Abc_ObjName(FI) << '\n');

    VASTValPtr Operand = getAsOperand(FI);
    assert((Bitwidth == 0 || Bitwidth == Operand->getBitWidth())
           && "Bitwidth mismatch!");

    if (!Bitwidth) Bitwidth = Operand->getBitWidth();
    Ops.push_back(Operand);
  }

  assert(Bitwidth && "We got a node without fanin?");

  char *sop = (char*)Abc_ObjData(Obj);

  if (ExpandLUT) {
    // Expand the SOP back to SOP if user ask to.
    VASTValPtr V = expandSOP(sop, Ops, Bitwidth, Builder);

    bool Inserted = RewriteMap.insert(std::make_pair(FO, V)).second;
    assert(Inserted && "The node is visited?");
    return;
  }

  // Do not need to build the LUT for a simple invert.
  if (Abc_SopIsInv(sop)) {
    assert(Ops.size() == 1 && "Bad operand size for invert!");
    VASTValPtr V = Builder.buildNotExpr(Ops[0]);
    bool Inserted = RewriteMap.insert(std::make_pair(FO, V)).second;
    assert(Inserted && "The node is visited?");
    return;
  }

  // Do not need to build the LUT for a simple buffer.
  if (Abc_SopIsBuf(sop)) {
    assert(Ops.size() == 1 && "Bad operand size for invert!");
    VASTValPtr V = Ops[0];
    bool Inserted = RewriteMap.insert(std::make_pair(FO, V)).second;
    assert(Inserted && "The node is visited?");
    return;
  }

  // Do not need to build the LUT for a simple and.
  //if (Abc_SopIsAndType(sop)) {
  //  VASTValPtr V = Builder.buildAndExpr(Ops, Bitwidth);
  //  bool Inserted = RewriteMap.insert(std::make_pair(FO, V)).second;
  //  assert(Inserted && "The node is visited?");
  //  return;
  //}

  //// Do not need to build the LUT for a simple and.
  //if (Abc_SopIsOrType(sop)) {
  //  VASTValPtr V = Builder.buildOrExpr(Ops, Bitwidth);
  //  bool Inserted = RewriteMap.insert(std::make_pair(FO, V)).second;
  //  assert(Inserted && "The node is visited?");
  //  return;
  //}

  // Otherwise simple construct the LUT expression.
  VASTValPtr SOP = VM.getOrCreateSymbol(sop, 0);
  // Encode the comment flag of the SOP into the invert flag of the LUT string.
  if (Abc_SopIsComplement(sop)) SOP = SOP.invert();
  Ops.push_back(SOP);

  VASTValPtr V = Builder.buildExpr(VASTExpr::dpLUT, Ops, Bitwidth);
  ++NumLUTBulit;

  bool Inserted = RewriteMap.insert(std::make_pair(FO, V)).second;
  assert(Inserted && "The node is visited?");
}

void LogicNetwork::buildLUTTree(Abc_Obj_t *Root, DatapathBuilder &Builder) {
  std::vector<std::pair<Abc_Obj_t*, int> > VisitStack;
  VisitStack.push_back(std::make_pair(Abc_ObjRegular(Root), 0));

  while (!VisitStack.empty()) {
    assert(Abc_ObjIsNet(VisitStack.back().first) && "Bad object type!");
    Abc_Obj_t *CurNode = Abc_ObjRegular(Abc_ObjFanin0(VisitStack.back().first));
    int &FIIdx = VisitStack.back().second;

    if (FIIdx == Abc_ObjFaninNum(CurNode)) {
      VisitStack.pop_back();

      DEBUG(dbgs().indent(VisitStack.size() * 2) << "Visiting "
            << Abc_ObjName(Abc_ObjRegular(CurNode)) << '\n');

      // All fanin visited, visit the current node.
      buildLUTExpr(CurNode, Builder);
      continue;
    }

    Abc_Obj_t *ChildNode = Abc_ObjRegular(Abc_ObjFanin(CurNode, FIIdx));
    ++FIIdx;

    // Fanin had already visited.
    if (isNodeVisited(ChildNode)) continue;

    VisitStack.push_back(std::make_pair(Abc_ObjRegular(ChildNode), 0));
  }
}

void LogicNetwork::buildLUTDatapath(DatapathBuilder &Builder) {
  int i;
  Abc_Obj_t *Obj;

  DEBUG(dump());

  Abc_NtkForEachPo(Ntk,Obj, i) {
    Abc_Obj_t *FI = Abc_ObjFanin0(Obj);
    // The Fanin of the PO maybe visited.
    if (isNodeVisited(FI)) continue;

    // Rewrite the LUT tree rooted FI.
    buildLUTTree(FI, Builder);

    AbcObjMapTy::const_iterator at = RewriteMap.find(Abc_ObjRegular(FI));
    assert(at != RewriteMap.end() && "Bad Abc_Obj_t visiting order!");
    VASTValPtr NewVal = at->second;
    if (Abc_ObjIsComplement(FI)) NewVal = NewVal.invert();

    VASTValPtr &OldVal = ValueNames[Abc_ObjName(Abc_ObjRegular(FI))];
    // Update the mapping if the mapped value changed.
    if (OldVal != NewVal) {
      Builder.replaceAllUseWith(OldVal, NewVal);
      OldVal = NewVal;
    }
  }
}

static ManagedStatic<ABCContext> GlobalContext;

LogicNetwork::LogicNetwork(VASTModule &VM) : Context(*GlobalContext), VM(VM) {
  Ntk = Abc_NtkAlloc(ABC_NTK_STRASH, ABC_FUNC_AIG, 1);
  Ntk->pName = Extra_UtilStrsav(VM.getName().c_str());
}

namespace {
struct LUTMapping : public VASTModulePass {
  DatapathBuilder *Builder;

  static char ID;
  LUTMapping() : VASTModulePass(ID), Builder(0) {
    initializeLUTMappingPass(*PassRegistry::getPassRegistry());
  }

  bool runOnVASTModule(VASTModule &VM);
};

}

static unsigned GetSameWidth(VASTValPtr LHS, VASTValPtr RHS) {
  unsigned BitWidth = LHS->getBitWidth();
  assert(BitWidth == RHS->getBitWidth() && "Bitwidth not match!");
  return BitWidth;
}

static bool BreakDownNAryExpr(VASTExpr *Expr, DatapathBuilder &Builder) {
  VASTExpr::Opcode Opcode = Expr->getOpcode();

  if (Opcode != VASTExpr::dpAnd) return false;

  // Already binary expressions no need to break them down.
  if (Expr->size() <= 2) return false;

  SmallVector<VASTValPtr, 8> Ops;
  for (unsigned i = 0; i < Expr->size(); ++i)
    Ops.push_back(Expr->getOperand(i));

  // Construct the expression tree for the NAry expression.
  while (Ops.size() > 1) {
    unsigned ResultPos = 0;
    unsigned OperandPos = 0;
    unsigned NumOperand = Ops.size();
    while (OperandPos + 1 < NumOperand) {
      VASTValPtr LHS = Ops[OperandPos];
      VASTValPtr RHS = Ops[OperandPos + 1];
      OperandPos += 2;
      unsigned ResultWidth = GetSameWidth(LHS, RHS);
      // Create the BinExpr without optimizations.
      VASTValPtr BinExpr = Builder.createExpr(Expr->getOpcode(), LHS, RHS,
                                               ResultWidth);

      Ops[ResultPos++] = BinExpr;
      ++NumAndExpand;
    }

    // Move the rest of the operand.
    while (OperandPos < NumOperand)
      Ops[ResultPos++] = Ops[OperandPos++];

    // Only preserve the results.
    Ops.resize(ResultPos);
  }

  // Replace the original Expr by the broken down Expr.
  Builder.replaceAllUseWith(Expr, Ops.back());
  return true;
}

static void BreakNAryExpr(DatapathContainer &DP, DatapathBuilder &Builder) {
  std::vector<VASTExpr*> Worklist;

  typedef DatapathContainer::expr_iterator iterator;
  for (iterator I = DP.expr_begin(), E = DP.expr_end(); I != E; ++I)
    if (I->getOpcode() == VASTExpr::dpAnd) Worklist.push_back(I);

  while (!Worklist.empty()) {
    VASTExpr *E = Worklist.back();
    Worklist.pop_back();

    BreakDownNAryExpr(E, Builder);
  }
}

bool LUTMapping::runOnVASTModule(VASTModule &VM) {
  DatapathContainer &DP = VM;

  MinimalDatapathContext Context(DP, getAnalysisIfAvailable<DataLayout>());
  Builder = new DatapathBuilder(Context);

  BreakNAryExpr(DP, *Builder);

  LogicNetwork Ntk(VM);

  if (!Ntk.buildAIG(DP)) return true;

  Ntk.cleanUp();

  // Synthesis the logic network.
  Ntk.synthesis();

  // Map the logic network to LUTs
  Ntk.performLUTMapping();

  Ntk.buildLUTDatapath(*Builder);

  delete Builder;
  return true;
}

char LUTMapping::ID = 0;

INITIALIZE_PASS(LUTMapping, "vast-lut-mapping", "Map Logic Operation to LUTs",
                false, true)

Pass *llvm::createLUTMappingPass() {
  return new LUTMapping();
}
