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

#include "shang/VASTDatapathNodes.h"
#include "shang/VASTSeqValue.h"
#include "shang/VASTMemoryPort.h"
#include "shang/Strash.h"

#include "llvm/Analysis/Dominators.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/Format.h"
#define DEBUG_TYPE "shang-compatibility-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumNonTranEdgeBreak, "Number of non-transitive edges are broken");
STATISTIC(NumDecomposed,
          "Number of operand register of chained operation decomposed");
STATISTIC(NumMinCostFlowIteration,
          "Number of iterations in the min-cost flow solver");

// FIXME: Read the value from the scripting engine.
static const float mux_factor = 1.0f;
static const float area_factor = 1.0f;

const CompGraphNode::Cost &
CompGraphNode::getCostTo(const CompGraphNode *To) const {
  CostVecTy::const_iterator I = SuccCosts.find(const_cast<CompGraphNode*>(To));
  assert(I != SuccCosts.end() && "Not a Successor!");
  return I->second;
}

CompGraphNode::Cost &CompGraphNode::getCostToInternal(const CompGraphNode *To) {
  CostVecTy::iterator I = SuccCosts.find(const_cast<CompGraphNode*>(To));
  assert(I != SuccCosts.end() && "Not a Successor!");
  return I->second;
}

void CompGraphNode::Cost::accumulateDelta(CompGraphNode *Src, CompGraphNode *Dst,
                                          float Weight) {
  Deltas[NodePair(&Src->BindingIdx, &Dst->BindingIdx)] += Weight;
}

float CompGraphNode::Cost::getMergedDetaBenefit() const {
  float Benefit = 0.0f;
  for (delta_iterator I = delta_begin(), E = delta_end(); I != E; ++I) {
    if (*I->first.first == *I->first.second)
      Benefit += I->second;
  }

  return Benefit;
}

void CompGraphNode::print(raw_ostream &OS) const {
  OS << "LI" << Idx << " order " << Order;
  if (Instruction *I = Inst)
    OS << ' ' << *I << ' ';
  OS << '\n';

  OS.indent(2) << "Defs: ";
  ::dump(Defs, OS);
  OS.indent(2) << "Reachables: ";
  ::dump(Reachables, OS);
}

void CompGraphNode::dump() const {
  print(dbgs());
}

void CompGraphNode::merge(const CompGraphNode *RHS) {
  Defs        |= RHS->Defs;
  Reachables  |= RHS->Reachables;
}

bool CompGraphNode::isCompatibleWithStructural(const CompGraphNode *RHS) const {
  if (!VFUs::isFUCompatible(FUType, RHS->FUType))
    return false;

  if (Sels.size() != RHS->Sels.size())
    return false;

  // Bitwidth should be the same.
  for (unsigned i = 0, e = size(); i != e; ++i)
    if (getSelector(i)->getBitWidth() != RHS->getSelector(i)->getBitWidth())
      return false;

  return true;
}

bool CompGraphNode::isCompatibleWithInterval(const CompGraphNode *RHS) const {

  // FIXME: It looks like that we need to test intersection between defs and
  // alives. But intersection test between defs and kills are not required.
  // Defines should not intersects.
  if (intersects(Defs, RHS->Defs))
    return false;

  // Alives should not intersects.
  if (intersects(Reachables, RHS->Reachables))
    return false;

  return true;
}

BasicBlock *CompGraphNode::getDomBlock() const {
  return Inst->getParent();
}

void CompGraphBase::initalizeDomTreeLevel() {
  typedef DomTreeNode::iterator dt_child_iterator;
  if (!DomTreeLevels.empty()) return;

  SmallVector<DomTreeNode*, 32> Worklist;

  DomTreeNode *Root = DT.getRootNode();
  DomTreeLevels[Root->getBlock()] = 0;
  Worklist.push_back(Root);

  while (!Worklist.empty()) {
    DomTreeNode *Node = Worklist.pop_back_val();
    unsigned ChildLevel = DomTreeLevels[Node->getBlock()] + 1;
    for (dt_child_iterator CI = Node->begin(), CE = Node->end(); CI != CE; ++CI)
    {
      DomTreeLevels[(*CI)->getBlock()] = ChildLevel;
      Worklist.push_back(*CI);
    }
  }
}

bool
CompGraphBase::isBefore(CompGraphNode *Src, CompGraphNode *Dst) {
  assert(!Src->IsTrivial && !Dst->IsTrivial && "Unexpected trivial node!");
  BasicBlock *SrcBlock = Src->getDomBlock(), *DstBlock = Dst->getDomBlock();
  if (SrcBlock != DstBlock) {
    if (DT.dominates(SrcBlock, DstBlock))
      return true;

    if (DT.dominates(DstBlock, SrcBlock))
      return false;

    unsigned SrcLevel = getDomTreeLevel(SrcBlock),
             DstLevel = getDomTreeLevel(DstBlock);

    if (SrcLevel < DstLevel)
      return true;

    if (DstLevel < SrcLevel)
      return false;
  }

  if (Src->Order < Dst->Order)
    return true;

  if (Dst->Order < Src->Order)
    return false;


  return Src < Dst;
}

// Break the non-transitive edges, otherwise the network flow approach doesn't
// work. TODO: Develop a optimal edge breaking approach.
void CompGraphBase::fixTransitive() {
  SmallVector<NodeTy*, 4> NonTransitiveNodes;
  // Check a -> b and b -> c implies a -> c;
  typedef NodeVecTy::iterator node_iterator;
  for (node_iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
    NodeTy *Node = I;

    for (NodeTy::iterator SI = Node->succ_begin(), SE = Node->succ_end();
         SI != SE; ++SI) {
      NodeTy *Succ = *SI;

      if (Succ->IsTrivial)
        continue;

      for (NodeTy::iterator SSI = Succ->succ_begin(), SSE = Succ->succ_end();
           SSI != SSE; ++SSI) {
        NodeTy *SuccSucc = *SSI;

        if (SuccSucc->IsTrivial)
          continue;

        if (!Node->countSuccessor(SuccSucc)) {
          DEBUG(dbgs() << "\nNontransitive edge:\n";
          Node->dump();
          Succ->dump();
          SuccSucc->dump());
          NonTransitiveNodes.push_back(Succ);
          ++NumNonTranEdgeBreak;
          break;
        }
      }
    }

    while (!NonTransitiveNodes.empty())
      Node->unlinkSucc(NonTransitiveNodes.pop_back_val());
  }
}

void CompGraphBase::computeCompatibility() {
  Entry.dropAllEdges();
  Exit.dropAllEdges();

  for (iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I)
    I->dropAllEdges();

  for (iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
    NodeTy *Node = I;

    // And insert the node into the graph.
    for (NodeTy::iterator I = Entry.succ_begin(), E = Entry.succ_end(); I != E; ++I) {
      NodeTy *Other = *I;

      // Make edge between compatible nodes.
      if (Node->isCompatibleWith(Other))
        makeEdge(Node, Other);
    }

    // There will always edge from entry to a node and from node to exit.
    makeEdge(&Entry, Node);
    makeEdge(Node, &Exit);
  }
}

void CompGraphBase::initializeCosts() {
  typedef NodeTy::cost_iterator iterator;
  for (iterator I = Entry.cost_begin(), E = Entry.cost_end(); I != E; ++I){
    NodeTy *Src = I->first;
    I->second.InterconnectCost += area_factor * computeReqiredResource(Src);
    I->second.InterconnectCost += mux_factor * computeSingleNodeFaninCost(Src);

    for (iterator SI = Src->cost_begin(), SE = Src->cost_end(); SI != SE; ++SI){
      NodeTy *Dst = SI->first;

      if (Dst->IsTrivial)
        continue;

      float FaninCost = computeFaninCost(Src, Dst, &SI->second);
      SI->second.InterconnectCost += mux_factor * FaninCost;
    }
  }
}

unsigned CompGraphBase::computeSingleNodeFaninCost(CompGraphNode *Node) const {
  unsigned Cost = 0;

  std::set<VASTValue*> FIs;
  for (unsigned i = 0, e = Node->size(); i != e; ++i) {
    VASTSelector *Sel = Node->getSelector(i);

    FIs.clear();
    // Try to find is there any node share the same fanout, there will be some
    // benefit if we bind nodes that share common fanout.
    typedef VASTSelector::iterator iterator;
    for (iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
      const VASTLatch &L = *I;
      VASTValPtr CurFI = L;
      VASTValPtr GurGuard = L.getGuard();

      if (Sel->isTrivialFannin(L))
        continue;

      FIs.insert(CurFI.get());
      FIs.insert(GurGuard.get());

      typedef std::set<VASTValue*>::iterator fanin_iterator;
      for (fanin_iterator FI = FIs.begin(), FE = FIs.end(); FI != FE; ++FI) {
        VASTValue *OtherFI = *FI;
        if (OtherFI != CurFI.get())
          computeFaninDelta(CurFI.get(), OtherFI, 0);

        if (OtherFI != GurGuard.get())
          computeFaninDelta(GurGuard.get(), OtherFI, 0);
      }

      Cost += FIs.size() * Sel->getBitWidth();
    }
  }

  return Cost;
}

int CompGraphBase::computeFaninCost(CompGraphNode *Src, CompGraphNode *Dst,
                                    CostTy *Cost) const {
  int TotalFIs = 0;
  assert(Src->size() == Dst->size() && "Fanin size not matched!");
  for (unsigned i = 0, e = Src->size(); i != e; ++i)
    TotalFIs += computeFaninCost(Src->getSelector(i), Dst->getSelector(i), Cost);
  return TotalFIs;
}

int CompGraphBase::computeFaninCost(VASTSelector *Src, VASTSelector *Dst,
                                    CostTy *Cost) const {
  // Ignore the invert flag, we assume the invert has zero cost.
  std::set<VASTValue*> SrcFIs, DstFIs;
  int Intersected = 0;
  typedef VASTSelector::iterator iterator;
  for (iterator SI = Src->begin(), SE = Src->end(); SI != SE; ++SI) {
    const VASTLatch &SL = *SI;

    if (Src->isTrivialFannin(SL) || Dst->isTrivialFannin(SL))
      continue;

    VASTValPtr SrcFI = SL;

    if (!SrcFIs.insert(SrcFI.get()).second)
      continue;

    for (iterator DI = Dst->begin(), DE = Dst->end(); DI != DE; ++DI) {
      const VASTLatch &DL = *DI;

      if (Dst->isTrivialFannin(DL) || Src->isTrivialFannin(DL))
        continue;

      VASTValPtr DstFI = DL;

      if (!DstFIs.insert(DstFI.get()).second)
        continue;

      if (computeFaninDelta(SrcFI.get(), DstFI.get(), Cost)) {
        ++Intersected;
        continue;
      }
    }
  }

  unsigned Bitwidth = Dst->getBitWidth();
  int IncreasedNumPorts = int(DstFIs.size()) - Intersected;
  return IncreasedNumPorts * int(Bitwidth);
}

bool CompGraphBase::computeFaninDelta(VASTValue *SrcFI, VASTValue *DstFI,
                                      CostTy *Cost) const {
  if (SrcFI == DstFI)
    return true;

  if (SrcFI->getASTType() != DstFI->getASTType())
    return false;

  unsigned Bitwidth = std::max(SrcFI->getBitWidth(), DstFI->getBitWidth());

  if (VASTSeqValue *SrcV = dyn_cast<VASTSeqValue>(SrcFI)) {
    VASTSeqValue *DstV = cast<VASTSeqValue>(DstFI);
    return computeFaninDelta(SrcV->getSelector(), DstV->getSelector(),
                             Bitwidth, Cost);
  }

  if (VASTExpr *SrcExpr = dyn_cast<VASTExpr>(SrcFI)) {
    VASTExpr *DstExpr = cast<VASTExpr>(DstFI);
    typedef CombPatternTable::DeltaResult Delta;
    const Delta D = CPT.getLeavesDelta(SrcExpr, DstExpr);
    if (D.IsAlwaysDifferent)
      return false;

    if (D.IsAlwaysIdentical)
      return true;

    bool IsIdentical = true;

    assert(D.Deltas.size() && "Unexpected empty delta!");
    float Weight = float(Bitwidth) / float(D.Deltas.size());

    typedef Delta::iterator iterator;
    for (iterator I = D.begin(), E = D.end(); I != E; ++I)
      IsIdentical &= computeFaninDelta(I->first, I->second, Weight, Cost);
    
    return IsIdentical;
  }

  return false;
}

bool CompGraphBase::computeFaninDelta(VASTSelector *Src, VASTSelector *Dst,
                                      float Weight, CostTy *Cost) const {
  CompGraphNode *SrcNode = lookupNode(Src);
  CompGraphNode *DstNode = lookupNode(Dst);

  if (!SrcNode || !DstNode)
    return false;

  if (SrcNode == DstNode)
    return true;

  if (!SrcNode->countSuccessor(DstNode)) {
    if (!DstNode->countSuccessor(SrcNode))
      return false;

    std::swap(SrcNode, DstNode);
  }

  if (Cost) {
    // Add a edge delta, if source node and dst node are bound to the same
    // physical unit, there is benefit to bind the current units too.
    Cost->accumulateDelta(SrcNode, DstNode, Weight);
    // Also give some benefit to bind SrcNode and DstNode to the same physical
    // unit, since doing so help reduce the cost.
    Weight *= 0.2f;
  }
 
  // Otherwise Src and Dst are fanin to the same selector, there is benefit
  // to bind SrcNode to Dst node as they redece the interconnect of the fanout
  // selector.
  SrcNode->getCostToInternal(DstNode).InterconnectCost -= mux_factor * Weight;

  return false;
}

void CompGraphBase::addBoundNode(VASTSeqOp *SeqOp) {
  for (unsigned i = 0; i < SeqOp->num_srcs(); ++i) {
    VASTSelector *Sel = SeqOp->getSrc(i).getSelector();
    CompGraphNode *&BoundNode = SelectorMap[Sel];
    if (BoundNode)
      continue;

    // Just prevent the node from being bound.
    BoundNode = createNode(VFUs::Mux, 0, Nodes.size(), DataflowInst(0, 0), Sel);
    Nodes.push_back(BoundNode);
  }

  if (VASTSeqInst *SeqInst = dyn_cast<VASTSeqInst>(SeqOp)) {
    if (isa<LoadInst>(SeqInst->getValue())) {
      VASTSelector *Addr = SeqOp->getSrc(0).getSelector();
      VASTMemoryBus *Bus = cast<VASTMemoryBus>(Addr->getParent());
      unsigned PortIdx = 0;
      if (Bus->isDualPort() && Bus->getAddr(1) == Addr)
        PortIdx = 1;
      // Pretend the Memory output port share the same register with the address
      // so the latch operation that reads the output port can get a fanin.
      SelectorMap[Bus->getFanout(PortIdx)] = SelectorMap[Addr];
    }
  }
}

CompGraphNode *CompGraphBase::addNewNode(VASTSeqInst *SeqInst) {
  assert(SeqInst && "Unexpected null pointer pass to GetOrCreateNode!");
  DataflowInst Inst(SeqInst);

  CompGraphNode *&Node = InstMap[Inst];
  if (Node)
    return Node;

  SmallVector<VASTSelector*, 4> Sels;
  assert(SeqInst->getNumDefs() == SeqInst->num_srcs()
         && "Expected all assignments are definitions!");
  for (unsigned i = 0; i < SeqInst->num_srcs(); ++i)
    Sels.push_back(SeqInst->getSrc(i).getSelector());

  VFUs::FUTypes FUType = SeqInst->getFUType();
  unsigned FUCost = SeqInst->getFUCost();

  // Create the node if it not exists yet.
  Node = createNode(FUType, FUCost, Nodes.size() + 1, Inst, Sels);
  Nodes.push_back(Node);

  // Build the node mapping.
  for (unsigned i = 0, e = Sels.size(); i != e; ++i)
    SelectorMap[Sels[i]] = Node;

  return Node;
}

void CompGraphBase::decomposeTrivialNodes() {
  typedef NodeVecTy::iterator node_iterator;
  for (node_iterator I = Nodes.begin(), E = Nodes.end(); I != E; /*++I*/) {
    NodeTy *Node = static_cast<NodeTy*>((CompGraphNode*)(I++));
    if (Node->size() != 1 && Node->FUType == VFUs::Trivial) {
      typedef NodeTy::sel_iterator sel_iterator;
      for (sel_iterator I = Node->begin(), E = Node->end(); I != E; ++I) {
        VASTSelector *Sel = *I;
        NodeTy *SubNode
          = new NodeTy(VFUs::Trivial, 0, Nodes.size(), Node->Inst, Sel);
        // Copy the live-interval from the parent node.
        SubNode->getDefs() = Node->getDefs();
        SubNode->getReachables() = Node->getReachables();
        Nodes.push_back(SubNode);
        // Also update the node mapping.
        SelectorMap[Sel] = SubNode;
        ++NumDecomposed;
      }

      InstMap.erase(Node->Inst);
      Nodes.erase(Node);
    }
  }
}

unsigned CompGraphBase::computeReqiredResource(const CompGraphNode *Node) const {
  float Cost = 0.0f;
  // 1. Calculate the number of registers we can reduce through this edge.
  typedef NodeTy::const_sel_iterator sel_iterator;
  // for (sel_iterator I = Node->begin(), E = Node->end(); I != E; ++I)
  //  Cost += (*I)->getBitWidth();

  // 2. Calculate the functional unit resource reduction through this edge.
  Cost += Node->FUCost;

  return Cost;
}

namespace llvm {
template<> struct DOTGraphTraits<CompGraphBase*> : public DefaultDOTGraphTraits{
  typedef CompGraphBase GraphTy;
  typedef GraphTy::NodeTy NodeTy;
  typedef NodeTy::iterator NodeIterator;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getEdgeSourceLabel(const NodeTy *Node, NodeIterator I){
    SmallString<8> S;
    {
      raw_svector_ostream SS(S);
      SS << format("%.2f", Node->getCostTo(*I).InterconnectCost);
    }

    return S.str();
  }

  static std::string getEdgeAttributes(const NodeTy *Node, NodeIterator I,
                                       GraphTy *G) {
    return "style=dashed";
  }


  static void addCustomGraphFeatures(const CompGraphBase *G,
                                     GraphWriter<CompGraphBase*> &GW) {
    if (!G->hasbinding()) return;

    unsigned ClusterNum = 0;

    raw_ostream &O = GW.getOStream();
    typedef CompGraphBase::cluster_iterator cluster_iterator;
    for (cluster_iterator I = G->cluster_begin(), E = G->cluster_end();
         I != E; ++I) {
      ArrayRef<NodeTy*> Nodes(*I);

      if (Nodes.size() == 1)
        continue;

      O.indent(2) << "subgraph cluster_" << ClusterNum << " {\n";
      O.indent(4) << "label = \"" << ClusterNum << "\";\n";
      ++ClusterNum;

      O.indent(4) << "style = solid;\n";
      O.indent(4) << "Node" << static_cast<const void*>(Nodes.front())
                  << ";\n";

      for (unsigned i = 1; i < Nodes.size(); ++i)
        O.indent(4) << "Node" << static_cast<const void*>(Nodes[i])
                    << ";\n";

      O.indent(2) << "}\n";
    }
  }

  std::string getNodeLabel(const NodeTy *Node, const GraphTy *Graph) {
    SmallString<32> S;
    {
      raw_svector_ostream SS(S);
      SS << Node->Idx << ' ';
      if (Node->Inst.IsLauch()) {
        SS << Node->Inst->getOpcodeName();
      }
    }

    return S.str();
  }

  static std::string getNodeAttributes(const NodeTy *Node,
                                       const GraphTy *Graph) {
    return "shape=Mrecord";
  }
};
}

void CompGraphBase::viewGraph() {
  ViewGraph(this, "CompatibilityGraph");
}

#include "lpsolve/lp_lib.h"

namespace llvm {
class MinCostFlowSolver {
public:
  typedef std::pair<const CompGraphNode*, const CompGraphNode*> EdgeType;

private:
  CompGraphBase &G;
  lprec *lp;
  VFUs::FUTypes Type;

  // Map the edge to column number in LP.
  typedef std::map<EdgeType, std::pair<unsigned, unsigned> > Edge2IdxMapTy;
  Edge2IdxMapTy Edge2IdxMap;

  bool lookupEdgeConsistency(EdgeType Edge) const {
    Edge2IdxMapTy::const_iterator I = Edge2IdxMap.find(Edge);
    return I == Edge2IdxMap.end() ? 0 : I->second.second;
  }

  unsigned lookUpEdgeIdx(EdgeType Edge) const {
    Edge2IdxMapTy::const_iterator I = Edge2IdxMap.find(Edge);
    return I == Edge2IdxMap.end() ? 0 : I->second.first;
  }

  unsigned createEdgeVariables(const CompGraphNode *N, unsigned Col);
public:
  MinCostFlowSolver(CompGraphBase &G, VFUs::FUTypes Type)
    : G(G), lp(0), Type(Type) {}
  ~MinCostFlowSolver() {
    delete_lp(lp);
  }

  unsigned createLPAndVariables();
  unsigned createBlanceConstraints();
  void setCost(unsigned Iteration);
  void applySolveSettings();
  bool solveMinCostFlow();

  typedef CompGraphBase::ClusterType ClusterType;
  typedef CompGraphBase::ClusterVectors ClusterVectors;
  unsigned buildBinging(ClusterVectors &Clusters);
  unsigned buildBinging(CompGraphNode *Src, ClusterType &Cluster);

  bool hasFlow(EdgeType E, unsigned TotalRows) const {
    Edge2IdxMapTy::const_iterator I = Edge2IdxMap.find(E);
    assert(I != Edge2IdxMap.end() && "Edge ID does not existed!");
    REAL flow = get_var_primalresult(lp, TotalRows + I->second.first);
    return flow != 0.0;
  }

  unsigned getNumRows() const {
    return get_Nrows(lp);
  }
};
}

unsigned MinCostFlowSolver::createEdgeVariables(const CompGraphNode *N,
                                                unsigned Col) {
  typedef CompGraphNode::iterator iterator;
  for (iterator I = N->succ_begin(), E = N->succ_end(); I != E; ++I) {
    CompGraphNode *Succ = *I;
    if (Succ->FUType != Type && !Succ->IsTrivial)
      continue;

    bool inserted
      = Edge2IdxMap.insert(std::make_pair(EdgeType(N, Succ),
                                          std::make_pair(Col, false))).second;
    assert(inserted && "Edge had already exisited?");

    add_columnex(lp, 0, 0,0);
    DEBUG(SmallString<8> S;
    {
      raw_svector_ostream SS(S);
      SS << 'E' << Col;
    }
    set_col_name(lp, Col, const_cast<char*>(S.c_str())););
    set_int(lp, Col, TRUE);
    // Set the maximum capacity (upper bound) of each edge to 1.
    set_upbo(lp, Col, 1.0);
    Col += 1;
  }

  return Col;
}

// Create the lp model and variables, return the number of variable created.
unsigned MinCostFlowSolver::createLPAndVariables() {
  lp = make_lp(0, 0);

  set_add_rowmode(lp, FALSE);

  // Column number of LP in lpsolve starts from 1
  unsigned Col =  createEdgeVariables(G.getEntry(), 1);

  for (CompGraphBase::iterator I = G.begin(), E = G.end(); I != E; ++I) {
    CompGraphNode *N = I;
    if (N->FUType != Type)
      continue;

    Col = createEdgeVariables(N, Col);
  }

  set_add_rowmode(lp, FALSE);
  return Col - 1;
}

unsigned MinCostFlowSolver::createBlanceConstraints() {
  set_add_rowmode(lp, TRUE);

  // Allocate rows for source and sink node.
  std::vector<int> Col;
  std::vector<REAL> Coeff;

  // Count the number of nodes, use it as the outflow of the entry and the
  // inflow of the exit.
  unsigned MaxFlow = 0;
  const CompGraphNode *S = G.getEntry();
  typedef CompGraphNode::iterator iterator;
  for (iterator SI = S->succ_begin(), SE = S->succ_end(); SI != SE; ++SI) {
    CompGraphNode *Succ = *SI;
    if (Succ->FUType != Type)
      continue;

    ++MaxFlow;
    unsigned EdgeIdx = lookUpEdgeIdx(EdgeType(S, Succ));
    assert(EdgeIdx && "Edge column number not available!");
    Col.push_back(EdgeIdx);
    Coeff.push_back(1.0);
  }

  // TODO: Set a tighter limit if the number of FU is constranted (E.g. mult).

  // The outflow from source must be smaller than supply.
  if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), LE, MaxFlow))
    report_fatal_error("Cannot add flow constraint!");

  Col.clear();
  Coeff.clear();
  const CompGraphNode *T = G.getExit();
  for (iterator SI = T->pred_begin(), SE = T->pred_end(); SI != SE; ++SI) {
    CompGraphNode *Pred = *SI;

    if (Pred->FUType != Type)
      continue;

    unsigned EdgeIdx = lookUpEdgeIdx(EdgeType(Pred, T));
    assert(EdgeIdx && "Edge column number not available!");
    Col.push_back(EdgeIdx);
    Coeff.push_back(1.0);
  }

  // The inflow to sink must be smaller than supply.
  if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), LE, MaxFlow))
    report_fatal_error("Cannot add flow constraint!");

  Col.clear();
  Coeff.clear();

  for (CompGraphBase::iterator I = G.begin(), E = G.end(); I != E; ++I) {
    CompGraphNode *N = I;

    if (N->FUType != Type)
      continue;

    Col.clear();
    Coeff.clear();

    // For each node, build constraint: Outflow = 1, since the splited edge
    // implies a unit flow incoming to each node.
    typedef CompGraphNode::iterator iterator;
    for (iterator SI = N->succ_begin(), SE = N->succ_end(); SI != SE; ++SI) {
      CompGraphNode *Succ = *SI;

      unsigned EdgeIdx = lookUpEdgeIdx(EdgeType(N, Succ));
      assert(EdgeIdx && "Edge column number not available!");
      Col.push_back(EdgeIdx);
      Coeff.push_back(1.0);
    }

    if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), EQ, 1.0))
      report_fatal_error("Cannot add flow constraint!");

    Col.clear();
    Coeff.clear();

    // For each node, build constraint: Inflow = 1, since the splited edge
    // implies a unit flow incoming to each node.
    typedef CompGraphNode::iterator iterator;
    for (iterator PI = N->pred_begin(), PE = N->pred_end(); PI != PE; ++PI) {
      CompGraphNode *Pred = *PI;
      unsigned EdgeIdx = lookUpEdgeIdx(EdgeType(Pred, N));
      assert(EdgeIdx && "Edge column number not available!");
      Col.push_back(EdgeIdx);
      Coeff.push_back(1.0);
    }

    if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), EQ, 1.0))
      report_fatal_error("Cannot add flow constraint!");
  }

  set_add_rowmode(lp, FALSE);

  return get_Nrows(lp);
}

void MinCostFlowSolver::setCost(unsigned Iteration) {
  std::vector<int> Indices;
  std::vector<REAL> Coefficients;
  typedef Edge2IdxMapTy::iterator iterator;
  float Scale = (float(Iteration) / 10.0f) * (float(Iteration) / 10.0f);
  Scale = std::max<float>(1.0f, Scale);

  for (iterator I = Edge2IdxMap.begin(), E = Edge2IdxMap.end(); I != E; ++I) {
    EdgeType Edge = I->first;
    const CompGraphNode *Src = Edge.first, *Dst = Edge.second;
    if (Dst->IsTrivial)
      continue;


    float EdgeCost = G.computeCost(Src, Dst);
    if (Iteration == 0) {
      I->second.second = I->second.second > 0 ? 1 : 0;
    } else if (Src->getBindingIdx() == Dst->getBindingIdx()) {
      unsigned Consistency = (I->second.second += 2);
      EdgeCost -= 1.0f * Consistency * Scale;;
    } else if (I->second.second)
      --I->second.second;

    Indices.push_back(I->second.first);
    Coefficients.push_back(EdgeCost);
  }

  set_obj_fnex(lp, Indices.size(), Coefficients.data(), Indices.data());
  set_minim(lp);
}

void MinCostFlowSolver::applySolveSettings() {
  set_verbose(lp, CRITICAL);
  DEBUG(set_verbose(lp, FULL));

  set_presolve(lp, PRESOLVE_NONE,
               get_presolveloops(lp));

  unsigned TotalRows = get_Nrows(lp), NumVars = get_Ncolumns(lp);
  DEBUG(dbgs() << "The model has " << NumVars << "x" << TotalRows << '\n');
  // Set timeout to 1 minute.
  set_timeout(lp, 20 * 60);
  DEBUG(dbgs() << "Timeout is set to " << get_timeout(lp) << "secs.\n");
}

bool MinCostFlowSolver::solveMinCostFlow() {
  DEBUG(write_lp(lp, "log.lp"));

  int result = solve(lp);

  DEBUG(dbgs() << "ILP result is: "<< get_statustext(lp, result) << "\n");
  DEBUG(dbgs() << "Time elapsed: " << time_elapsed(lp) << "\n");

  switch (result) {
  case INFEASIBLE:
    return false;
  case NOTRUN:
    return true;
  case SUBOPTIMAL:
    DEBUG(dbgs() << "Note: suboptimal schedule found!\n");
  case OPTIMAL:
  case PRESOLVED:
    break;
  case TIMEOUT:
    return false;
  default:
    report_fatal_error(Twine("Min cost flow fail: ")
                       + Twine(get_statustext(lp, result)));
  }

  float Cost = get_var_primalresult(lp, 0);
  DEBUG(dbgs() << "Object: " << Cost << ' ' << Type << '\n');

  return true;
}

unsigned MinCostFlowSolver::buildBinging(ClusterVectors &Clusters) {
  unsigned TotalRows = getNumRows();
  unsigned BindingIdx = 0;
  unsigned Changed = 0;

  CompGraphNode *Entry = G.getEntry();

  typedef CompGraphNode::iterator iterator;
  for (iterator I = Entry->succ_begin(), E = Entry->succ_end(); I != E; ++I) {
    CompGraphNode *Succ = *I;

    if (Succ->FUType != Type)
      continue;

    if (!hasFlow(EdgeType(Entry, Succ), TotalRows))
      continue;

    Clusters.push_back(ClusterType());
    unsigned HeadChanged = Succ->setBindingIdx(++BindingIdx);
    unsigned PathChanged = buildBinging(Succ, Clusters.back());

    Changed += HeadChanged + PathChanged;
    // Remove the trivial clusters.
    if (Clusters.back().empty())
      Clusters.pop_back();
  }

  DEBUG(dbgs() << "Changes: " << Changed << '\n');

  return Changed;
}

unsigned MinCostFlowSolver::buildBinging(CompGraphNode *Src, ClusterType &Cluster) {
  unsigned TotalRows = getNumRows();
  unsigned Changed = 0;

  typedef CompGraphNode::iterator iterator;
  while (!Src->IsTrivial) {
    for (iterator I = Src->succ_begin(), E = Src->succ_end(); I != E; ++I) {
      CompGraphNode *Dst = *I;

      if (!hasFlow(EdgeType(Src, Dst), TotalRows))
        continue;

      if (Dst->IsTrivial) {
        if (!Cluster.empty())
          Cluster.push_back(Src);

        return Changed;
      }

      // Propagate the binding index;
      Changed += Dst->setBindingIdx(Src->getBindingIdx());
      Cluster.push_back(Src);
      Src = Dst;
      break;
    }
  }

  llvm_unreachable("Should return before reach here!");
  return Changed;
}

unsigned CompGraphBase::performBinding() {

  for (unsigned i = 0; i < 5; ++i) {
    if (MCF[i] == 0) {
      MCF[i] = new MinCostFlowSolver(*this, VFUs::FUTypes(i));

      MCF[i]->createLPAndVariables();
      MCF[i]->createBlanceConstraints();
      MCF[i]->applySolveSettings();
    }
  }

  unsigned Iterations = 0;

  unsigned Changes = 1;
  while (Changes) {
    Changes = 0;
    Clusters.clear();

    for (unsigned i = 0; i < 5; ++i) {
      MCF[i]->setCost(Iterations);
      if (!MCF[i]->solveMinCostFlow())
        return 0;

      Changes += MCF[i]->buildBinging(Clusters);
    }
  
    ++Iterations;
    ++NumMinCostFlowIteration;
    //unsigned MinFlow = 1;

    //while (MaxFlow >= MinFlow) {
    //  unsigned MidFlow = MinFlow + (MaxFlow - MinFlow) / 2;

    //  MCF.setFUAllocationConstraints(MidFlow);

    //  if (MCF.solveMinCostFlow())
    //    // MidCount is ok, try a smaller one.
    //    MaxFlow = MidFlow - 1;
    //  else
    //    // Otherwise we should use a bigger supply.
    //    MinFlow = MidFlow + 1;
    //}
  }

  return Clusters.size();
}

CompGraphBase::~CompGraphBase() {
  for (unsigned i = 0; i < 5; ++i)
    if (MCF[i])
      delete MCF[i];
}
