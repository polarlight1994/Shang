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

#include "MFDatapathContainer.h"
#include "vtm/Utilities.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#define DEBUG_TYPE "vtm-logic-synthesis"
#include "llvm/Support/Debug.h"

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

  LogicNetwork(const Twine &Name);

  ~LogicNetwork() {
    Abc_NtkDelete(Ntk);
  }

  // Map VASTValue to Abc_Obj_t for AIG construction.
  typedef std::map<VASTValue*, Abc_Obj_t*> ValueMapTy;
  // Nodes.
  ValueMapTy Nodes;

  // Map the Abc_Obj_t name to Instruction.
  typedef StringMap<VASTValue*> ABCNameMapTy;
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
  bool buildAIG(MFDatapathContainer &Datapath);

  VASTValPtr getAsOperand(Abc_Obj_t *O) const;

  void buildLUTExpr(Abc_Obj_t *Obj, DatapathBuilder &Builder);
  void buildLUTDatapath(DatapathBuilder &Builder);

  bool hasExternalUse(VASTValue * V) {
    typedef VASTValue::use_iterator use_iterator;
    for (use_iterator UI = V->use_begin(), UE = V->use_end(); UI != UE; ++UI)
      if (VASTValue *U = dyn_cast<VASTValue>(*UI))
        // Some User outside the logic network found.
        if (getObj(U) == 0) {
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

bool LogicNetwork::buildAIG(MFDatapathContainer &Datapath) {
  // Remember the visited values so that we wont visit the same value twice.
  std::set<VASTValue*> Visited;
  bool AnyNodeCreated = false;
  typedef MFDatapathContainer::FanoutIterator iterator;

  for (iterator I = Datapath.fanout_begin(), E = Datapath.fanout_end();
       I != E; ++I)
    AnyNodeCreated |= buildAIG(I->second->getDriver().get(), Visited);

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

  assert(E->NumOps == 2 && "Bad Operand number!");

  VASTValPtr LHS = E->getOperand(0), RHS = E->getOperand(1);
  Abc_Obj_t *LHS_OBJ = getOrCreateObj(LHS.get()),
            *RHS_OBJ = getOrCreateObj(RHS.get());

  if (LHS.isInverted()) LHS_OBJ = Abc_ObjNot(LHS_OBJ);
  if (RHS.isInverted()) RHS_OBJ = Abc_ObjNot(RHS_OBJ);

  Abc_Obj_t *AndObj = Abc_AigAnd((Abc_Aig_t *)Ntk->pManFunc, LHS_OBJ, RHS_OBJ);
  Nodes.insert(std::make_pair(E, AndObj));

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

static char *utobin_buffer(uint64_t X, char *BufferEnd, unsigned NumDigit) {
  char *BufPtr = BufferEnd;
  *--BufPtr = 0;      // Null terminate buffer.
  if (X == 0) {
    *--BufPtr = '0';  // Handle special case.
    return BufPtr;
  }

  while (X) {
    unsigned char Mod = static_cast<unsigned char>(X) & 1;
    *--BufPtr = hexdigit(Mod);
    X >>= 1;
  }

  // Fill the MSB by 0 until we get NumDigits in the returned string.
  while (BufferEnd <= BufPtr + NumDigit);
    *--BufPtr = hexdigit(0);

  return BufPtr;
}

const char *llvm::TruthToSop(uint64_t Truth, unsigned NInput) {
  assert(NInput < 65 && "Too many inputs!");
  char buffer[65];
  unsigned NumDigit = 1 << NInput;
  utobin_buffer(Truth, buffer + 65, NumDigit);
  char *TruthStr = buffer + 64 - NumDigit;

  const char *SopStr = Abc_SopFromTruthBin(TruthStr);
  assert(SopStr && "Bad SopStr, may due to bad Input number.");
  return SopStr;
}

void LogicNetwork::buildLUTExpr(Abc_Obj_t *Obj, DatapathBuilder &Builder) {
  SmallVector<VASTValPtr, 4> Ops;
  Abc_Obj_t *FO = Abc_ObjFanout0(Obj);
  unsigned Bitwidth = 0;

  // No need to handle constant.
  if (Abc_NodeIsConst(Obj)) {
    return;
  }

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

  unsigned NInput = Ops.size();
  char *sop = (char*)Abc_ObjData(Obj);
  unsigned Truth = Abc_SopToTruth(sop, NInput);

  Ops.push_back(Builder.getOrCreateImmediate(Truth, 1 << NInput));

  VASTValPtr V = Builder.buildExpr(VASTExpr::dpLUT, Ops, Bitwidth);
  bool Inserted = RewriteMap.insert(std::make_pair(FO, V)).second;
  assert(Inserted && "The node is visited?");
}

void LogicNetwork::buildLUTDatapath(DatapathBuilder &Builder) {
  int i;
  Abc_Obj_t *Obj;

  DEBUG(dump());

  Abc_NtkForEachNode(Ntk, Obj, i) {
    buildLUTExpr(Obj, Builder);
  }

  Abc_NtkForEachPo(Ntk,Obj, i) {
    Abc_Obj_t *FI = Abc_ObjFanin0(Obj);

    AbcObjMapTy::const_iterator at = RewriteMap.find(Abc_ObjRegular(FI));
    assert(at != RewriteMap.end() && "Bad Abc_Obj_t visiting order!");
    VASTValPtr NewVal = at->second;
    if (Abc_ObjIsComplement(FI)) NewVal = NewVal.invert();
    
    VASTValPtr OldVal = ValueNames.lookup(Abc_ObjName(Abc_ObjRegular(FI)));
    Builder.replaceAllUseWith(OldVal, NewVal);
  }
}

void MFDatapathContainer::performLUTMapping(const Twine &Name) {
  LogicNetwork Ntk(Name);

  DEBUG(writeVerilog(dbgs(), Name));

  // Build the AIG, if nothing built
  if (!Ntk.buildAIG(*this)) return;

  // Clean up the network, prepare for logic optimization.
  Ntk.cleanUp();

  // Synthesis the logic network.
  Ntk.synthesis();

  // Map the logic network to LUTs
  Ntk.performLUTMapping();

  Ntk.buildLUTDatapath(Builder);

  DEBUG(writeVerilog(dbgs(), Name));
}

static ManagedStatic<ABCContext> GlobalContext;

LogicNetwork::LogicNetwork(const Twine &Name) : Context(*GlobalContext) {
  Ntk = Abc_NtkAlloc(ABC_NTK_STRASH, ABC_FUNC_AIG, 1);
  Ntk->pName = Extra_UtilStrsav(Name.str().c_str());
}
