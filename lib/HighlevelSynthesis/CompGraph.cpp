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
STATISTIC(NumCompEdges,
          "Number of compatible edges in the compatibility graph");
STATISTIC(NumDecomposed,
          "Number of operand register of chained operation decomposed");

float CompGraphNode::getCostTo(const CompGraphNode *To) const  {
  CostVecTy::const_iterator I = SuccCosts.find(To);
  assert(I != SuccCosts.end() && "Not a Successor!");
  return I->second;
}

void
CompGraphNode::setCost(const CompGraphNode *To, float Cost) {
  assert(SuccCosts.count(To) && "Not a successor!");
  SuccCosts[To] = Cost;
}

void CompGraphNode::print(raw_ostream &OS) const {
  OS << "LI" << Idx << " order " << Order;
  if (DomBlock)
    OS << ' ' << DomBlock->getName() << ' ';
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

bool CompGraphNode::isCompatibleWith(const CompGraphNode *RHS) const {
  if (Sels.size() != RHS->Sels.size())
    return false;

  // Bitwidth should be the same.
  for (unsigned i = 0, e = size(); i != e; ++i)
    if (getSelector(i)->getBitWidth() != RHS->getSelector(i)->getBitWidth())
      return false;

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

void CompGraphBase::initalizeDTDFSOrder() {
  typedef df_iterator<DomTreeNode*> iterator;
  DomTreeNode *Root = DT.getRootNode();
  unsigned Order = 0;
  for (iterator I = df_begin(Root), E = df_end(Root); I != E; ++I)
    DTDFSOrder[(*I)->getBlock()] = ++Order;
}

bool
CompGraphBase::isBefore(CompGraphNode *Src, CompGraphNode *Dst) {
  assert(!Src->IsTrivial && !Dst->IsTrivial && "Unexpected trivial node!");
  if (Src->DomBlock != Dst->DomBlock)
    return getDTDFSOrder(Src->DomBlock) < getDTDFSOrder(Dst->DomBlock);

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
      if (isCompatible(Node, Other) && Node->isCompatibleWith(Other))
        makeEdge(Node, Other);
    }

    // There will always edge from entry to a node and from node to exit.
    makeEdge(&Entry, Node);
    makeEdge(Node, &Exit);
  }
}

void CompGraphBase::collectCompatibleEdges(NodeTy *Dst, NodeTy *Src,
                                           std::set<NodeTy*> &SrcFIs) {
  EdgeVector &CompEdges = CompatibleEdges[EdgeType(Src, Dst)];

  std::set<CompGraphNode*> DstFIs;
  extractFaninNodes(Dst, DstFIs);

  typedef std::set<NodeTy*>::iterator iterator;
  for (iterator I = SrcFIs.begin(), E = SrcFIs.end(); I != E; ++I) {
    NodeTy *SrcFI = *I;

    for (iterator J = DstFIs.begin(), E = DstFIs.end(); J != E; ++J) {
      NodeTy *DstFI = *J;

      if (!VFUs::isNoTrivialFUCompatible(SrcFI->FUType, DstFI->FUType))
        continue;

      if (SrcFI->countSuccessor(DstFI)) {
        CompEdges.push_back(EdgeType(SrcFI, DstFI));
        ++NumCompEdges;
      }

      if (DstFI->countSuccessor(SrcFI)) {
        CompEdges.push_back(EdgeType(DstFI, SrcFI));
        ++NumCompEdges;
      }
    }
  }

  if (CompEdges.empty())
    CompatibleEdges.erase(EdgeType(Src, Dst));
}

void CompGraphBase::compuateEdgeCosts() {
  std::set<NodeTy*> SrcFIs;
  for (iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
    NodeTy *Src = I;
    unsigned SrcBinding = getBinding(Src);

    SrcFIs.clear();
    extractFaninNodes(Src, SrcFIs);

    typedef NodeTy::iterator succ_iterator;
    for (succ_iterator I = Src->succ_begin(), E = Src->succ_end(); I != E; ++I) {
      NodeTy *Dst = *I;

      if (Dst->IsTrivial)
        continue;

      unsigned DstBinding = getBinding(Dst);
      Src->setCost(Dst, computeCost(Src, SrcBinding, Dst, DstBinding));

      if (!SrcFIs.empty())
        collectCompatibleEdges(Dst, Src, SrcFIs);
    }
  }
}

CompGraphNode *CompGraphBase::addNewNode(VASTSeqInst *SeqInst) {
  assert(SeqInst && "Unexpected null pointer pass to GetOrCreateNode!");

  SmallVector<VASTSelector*, 4> Sels;
  assert(SeqInst->getNumDefs() == SeqInst->num_srcs()
         && "Expected all assignments are definitions!");
  for (unsigned i = 0; i < SeqInst->num_srcs(); ++i)
    Sels.push_back(SeqInst->getSrc(i).getSelector());

  VFUs::FUTypes FUType = SeqInst->getFUType();
  unsigned FUCost = SeqInst->getFUCost();

  // Create the node if it not exists yet.
  BasicBlock *DomBlock = cast<Instruction>(SeqInst->getValue())->getParent();
  NodeTy *Node = new NodeTy(FUType, FUCost, Nodes.size() + 1, DomBlock, Sels);  
  Nodes.push_back(Node);

  // Build the node mapping.
  for (unsigned i = 0, e = Sels.size(); i != e; ++i)
    NodesMap[Sels[i]] = Node;

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
        NodeTy *SubNode = new NodeTy(VFUs::Trivial, 0, Nodes.size(),
                                     Node->getDomBlock(), Sel);
        // Copy the live-interval from the parent node.
        SubNode->getDefs() = Node->getDefs();
        SubNode->getReachables() = Node->getReachables();
        Nodes.push_back(SubNode);
        // Also update the node mapping.
        NodesMap[Sel] = SubNode;
        ++NumDecomposed;
      }

      Nodes.erase(Node);
    }
  }
}

float CompGraphBase::computeIncreasedMuxPorts(VASTSelector *Src,
                                              VASTSelector *Dst) const {
  std::set<unsigned> SrcFIs, DstFIs, MergedFIs;
  typedef VASTSelector::iterator iterator;

  for (iterator I = Src->begin(), E = Src->end(); I != E; ++I) {
    unsigned SrashID = CST.getOrCreateStrashID(*I);
    SrcFIs.insert(SrashID);
    MergedFIs.insert(SrashID);
  }

  for (iterator I = Dst->begin(), E = Dst->end(); I != E; ++I) {
    unsigned SrashID = CST.getOrCreateStrashID(*I);
    DstFIs.insert(SrashID);
    MergedFIs.insert(SrashID);
  }

  int IncreasePorts = MergedFIs.size() - std::min(DstFIs.size(), SrcFIs.size());

  // Remember to multiple the saved mux (1 bit) port by the bitwidth.
  return IncreasePorts * std::max(Src->getBitWidth(), Dst->getBitWidth());
}

static void ExtractFaninNodes(VASTSelector *Sel,
                              std::set<VASTSeqValue*> &SVSet) {
  for (VASTSelector::iterator I = Sel->begin(), E = Sel->end(); I != E; ++I) {
    const VASTLatch &L = *I;
    L->extractSupportingSeqVal(SVSet);
    L.getGuard()->extractSupportingSeqVal(SVSet);
  }
}

void
CompGraphBase::translateToCompNodes(std::set<VASTSeqValue*> &SVSet,
                                    std::set<CompGraphNode*> &Fanins) const {
  typedef std::set<VASTSeqValue*>::iterator iterator;
  for (iterator I = SVSet.begin(), E = SVSet.end(); I != E; ++I) {
    VASTSeqValue *SV = *I;
    if (CompGraphNode *Src = lookupNode(SV->getSelector()))
      Fanins.insert(Src);
  }
}

void CompGraphBase::extractFaninNodes(VASTSelector *Sel,
                                      std::set<CompGraphNode*> &Fanins) const {
  std::set<VASTSeqValue*> SVSet;

  ExtractFaninNodes(Sel, SVSet);
  translateToCompNodes(SVSet, Fanins);
}

void
CompGraphBase::extractFaninNodes(NodeTy *N, std::set<NodeTy*> &Fanins) const {
  std::set<VASTSeqValue*> SVSet;

  for (NodeTy::sel_iterator I = N->begin(), E = N->end(); I != E; ++I) {
    VASTSelector *Sel = *I;
    ExtractFaninNodes(Sel, SVSet);
  }

  translateToCompNodes(SVSet, Fanins);
}

float CompGraphBase::computeIncreasedMuxPorts(CompGraphNode *Src,
                                              CompGraphNode *Dst) const {
  assert(Src->size() == Dst->size() && "Number of operand register not agreed!");
  float Cost = 0.0f;
  for (unsigned i = 0, e = Src->size(); i != e; ++i)
    Cost += computeIncreasedMuxPorts(Src->getSelector(i), Dst->getSelector(i));

  return Cost;
}

float CompGraphBase::compuateSavedResource(CompGraphNode *Src,
                                           CompGraphNode *Dst) const {
  float Cost = 0.0f;
  // 1. Calculate the number of registers we can reduce through this edge.
  typedef NodeTy::sel_iterator sel_iterator;
  for (sel_iterator I = Src->begin(), E = Src->end(); I != E; ++I)
    Cost += (*I)->getBitWidth();

  // 2. Calculate the functional unit resource reduction through this edge.
  if (VFUs::isNoTrivialFUCompatible(Src->FUType, Dst->FUType))
    Cost += std::min(Src->FUCost, Dst->FUCost);

  return Cost;
}

void CompGraphBase::setCommonFIBenefit() {
  typedef std::set<VASTSelector*>::iterator iterator;
  for (iterator I = BoundSels.begin(), E = BoundSels.end(); I != E; ++I)
    setCommonFIBenefit(*I);

  typedef std::map<VASTSelector*, CompGraphNode*>::iterator map_iterator;
  for (map_iterator I = NodesMap.begin(), E = NodesMap.end(); I != E; ++I)
    setCommonFIBenefit(I->first);
}

void CompGraphBase::setCommonFIBenefit(VASTSelector *Sel) {
  std::set<CompGraphNode*> Fanins;
  extractFaninNodes(Sel, Fanins);
  float Benefit = compuateCommonFIBenefit(Sel);

  if (Benefit <= 0)
    return;

  typedef std::set<CompGraphNode*>::iterator iterator;
  for (iterator I = Fanins.begin(), E = Fanins.end(); I != E; ++I) {
    CompGraphNode *LHS = static_cast<CompGraphNode*>(*I);

    for (iterator J = I; J != E; ++J) {
      CompGraphNode *RHS = static_cast<CompGraphNode*>(*J);

      if (LHS == RHS)
        continue;

      if (LHS->countSuccessor(RHS)) {
        LHS->setCost(RHS, LHS->getCostTo(RHS) - Benefit);
        continue;
      }

      if (RHS->countSuccessor(LHS)) {
        RHS->setCost(LHS, RHS->getCostTo(LHS) - Benefit);
        continue;
      }
    }
  }
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
      SS << format("%.2f", Node->getCostTo(*I));
    }

    return S.str();
  }

  static std::string getEdgeAttributes(const NodeTy *Node, NodeIterator I,
                                       GraphTy *G) {
    if (unsigned FUIdx = G->getBinding(const_cast<NodeTy*>(Node)))
      if (G->getBinding(*I) == FUIdx)
        return "color=blue";

    return "style=dashed";
  }


  static void addCustomGraphFeatures(const CompGraphBase *G,
                                     GraphWriter<CompGraphBase*> &GW) {
    if (!G->hasbinding()) return;

    std::map<unsigned, std::vector<NodeTy*> > Bindings;
    typedef CompGraphBase::binding_iterator binding_iterator;
    for (binding_iterator I = G->binding_begin(), E = G->binding_end();
         I != E; ++I)
      Bindings[I->second].push_back(I->first);

    raw_ostream &O = GW.getOStream();
    typedef std::map<unsigned, std::vector<NodeTy*> >::iterator cluster_iterator;
    for (cluster_iterator I = Bindings.begin(), E = Bindings.end(); I != E; ++I)
    {
      ArrayRef<NodeTy*> Nodes(I->second);

      if (Nodes.size() == 1)
        continue;

      O.indent(2) << "subgraph cluster_" << I->first << " {\n";
      O.indent(4) << "label = \"" << I->first << "\";\n";

      O.indent(4) << "style = solid;\n";

      for (unsigned i = 0; i < Nodes.size(); ++i)
        O.indent(4) << "Node" << static_cast<const void*>(Nodes[i]) << ";\n";

      O.indent(2) << "}\n";
    }
  }

  std::string getNodeLabel(const NodeTy *Node, const GraphTy *Graph) {
    return utostr_32(Node->Idx);
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

namespace {
class MinCostFlowSolver {
public:
  typedef std::pair<const CompGraphNode*, const CompGraphNode*> EdgeType;

private:
  CompGraphBase &G;
  lprec *lp;

  // Map the edge to column number in LP.
  typedef std::map<EdgeType, unsigned> Edge2IdxMapTy;
  Edge2IdxMapTy Edge2IdxMap;
  
  struct EdgeConsistencyBenefit {
    unsigned SrcEdgeIdx, DstEdgeIdx;
    unsigned SrcDstConsistentVarIdx, DstSrcConsistentVarIdx;
    float ConsistentBenefit;
  };
  std::vector<EdgeConsistencyBenefit> EdgeConsistencies;

  unsigned createEdgeVariables(const CompGraphNode *N, unsigned Col);
  unsigned createConsistencyVariables(EdgeType SrcEdge, EdgeType DstEdge,
                                      float Benefit, unsigned Col);
public:
  explicit MinCostFlowSolver(CompGraphBase &G) : G(G), lp(0) {}
  ~MinCostFlowSolver() {
    delete_lp(lp);
  }

  unsigned createLPAndVariables();
  unsigned createBlanceConstraints();
  void setFUAllocationConstraints(unsigned Supply);
  void setCost();
  void applySolveSettings();
  bool solveMinCostFlow();
  typedef CompGraphBase::BindingMapTy BindingMapTy;
  unsigned buildBinging(unsigned TotalRows, BindingMapTy &BindingMap);
  bool hasFlow(EdgeType E, unsigned TotalRows) const {
    Edge2IdxMapTy::const_iterator I = Edge2IdxMap.find(E);
    assert(I != Edge2IdxMap.end() && "Edge ID does not existed!");
    REAL flow = get_var_primalresult(lp, TotalRows + I->second);
    return flow != 0.0;
  }
};
}

unsigned MinCostFlowSolver::createEdgeVariables(const CompGraphNode *N,
                                                unsigned Col) {
  typedef CompGraphNode::iterator iterator;
  for (iterator I = N->succ_begin(), E = N->succ_end(); I != E; ++I) {
    CompGraphNode *Succ = *I;
    Edge2IdxMap[EdgeType(N, Succ)] = Col;

    SmallString<8> S;
    {
      raw_svector_ostream SS(S);
      SS << 'E' << Col;
    }
    set_col_name(lp, Col, const_cast<char*>(S.c_str()));
    set_int(lp, Col, TRUE);
    // Set the maximum capacity (upper bound) of each edge to 1.
    set_upbo(lp, Col, 1.0);
    Col += 1;
  }

  return Col;
}

unsigned
MinCostFlowSolver::createConsistencyVariables(EdgeType SrcEdge, EdgeType DstEdge,
                                              float Benefit, unsigned Col) {
  unsigned SrcDstConsistentVarIdx = Col;
  SmallString<8> S;
  {
    raw_svector_ostream SS(S);
    SS << 'C' << SrcDstConsistentVarIdx;
  }
  set_col_name(lp, SrcDstConsistentVarIdx, const_cast<char*>(S.c_str()));
  set_int(lp, SrcDstConsistentVarIdx, TRUE);
  // The variable will be 1 when the edges are not consistent.
  set_upbo(lp, SrcDstConsistentVarIdx, 1.0);

  unsigned DstSrcConsistentVarIdx = Col + 1;
  S.clear();
  {
    raw_svector_ostream SS(S);
    SS << 'C' << DstSrcConsistentVarIdx;
  }
  set_col_name(lp, DstSrcConsistentVarIdx, const_cast<char*>(S.c_str()));
  set_int(lp, DstSrcConsistentVarIdx, TRUE);
  set_upbo(lp, DstSrcConsistentVarIdx, 1.0);

  unsigned SrcEdgeIdx = Edge2IdxMap[SrcEdge], DstEdgeIdx = Edge2IdxMap[DstEdge];
  assert(SrcEdgeIdx && DstEdgeIdx && "Edge does not exist?");
  EdgeConsistencyBenefit ECB = { SrcEdgeIdx, DstEdgeIdx,
                                 SrcDstConsistentVarIdx, DstSrcConsistentVarIdx,
                                 Benefit };
  EdgeConsistencies.push_back(ECB);

  return Col + 2;
}

// Create the lp model and variables, return the number of variable created.
unsigned MinCostFlowSolver::createLPAndVariables() {
  lp = make_lp(0, 0);

  // Create the supply variable
  set_col_name(lp, 1, "supply");
  set_int(lp, 1, TRUE);

  // Column number of LP in lpsolve starts from 1, the other variables start
  // from 2 because we had allocated the supply variable.
  unsigned Col =  createEdgeVariables(G.getEntry(), 2);

  for (CompGraphBase::iterator I = G.begin(), E = G.end(); I != E; ++I) {
    CompGraphNode *N = I;
    Col = createEdgeVariables(N, Col);
  }

  // Create the variable for compatible edges, i.e., given edge (n0, n1),
  // (n2, n3), sometimes is is profit to bind n0 and n2 to the same physical
  // unit if n1 and n3 are bound to the same physical unit.
  typedef CompGraphBase::comp_edge_iterator comp_edge_iterator;
  for (comp_edge_iterator I = G.comp_edge_begin(), E = G.comp_edge_end();
       I != E; ++I) {
    CompGraphBase::EdgeType Edge = I->first;
    const CompGraphBase::EdgeVector &SrcEdges = I->second;
    typedef CompGraphBase::EdgeVector::const_iterator edge_iterator;
    for (edge_iterator I = SrcEdges.begin(), E = SrcEdges.end(); I != E; ++I) {
      CompGraphBase::EdgeType FIEdge = *I;
      float benefit = G.getEdgeConsistencyBenefit(Edge, FIEdge);
      if (benefit > 0)
        Col = createConsistencyVariables(Edge, FIEdge, benefit, Col);
    }
  }

  return Col - 1;
}

unsigned MinCostFlowSolver::createBlanceConstraints() {
  set_add_rowmode(lp, TRUE);

  // Allocate rows for source and sink node.
  std::vector<int> Col;
  std::vector<REAL> Coeff;

  const CompGraphNode *S = G.getEntry();
  typedef CompGraphNode::iterator iterator;
  for (iterator SI = S->succ_begin(), SE = S->succ_end(); SI != SE; ++SI) {
    CompGraphNode *Succ = *SI;
    unsigned EdgeIdx = Edge2IdxMap[EdgeType(S, Succ)];
    assert(EdgeIdx && "Edge column number not available!");
    Col.push_back(EdgeIdx);
    Coeff.push_back(1.0);
  }

  // Add the supply variable.
  Col.push_back(1);
  Coeff.push_back(-1.0);

  // The outflow from source must be smaller than supply.
  if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), LE, 0.0))
    report_fatal_error("Cannot add flow constraint!");

  Col.clear();
  Coeff.clear();
  const CompGraphNode *T = G.getExit();
  for (iterator SI = T->pred_begin(), SE = T->pred_end(); SI != SE; ++SI) {
    CompGraphNode *Pred = *SI;
    unsigned EdgeIdx = Edge2IdxMap[EdgeType(Pred, T)];
    assert(EdgeIdx && "Edge column number not available!");
    Col.push_back(EdgeIdx);
    Coeff.push_back(1.0);
  }

  // Add the supply variable.
  Col.push_back(1);
  Coeff.push_back(-1.0);

  // The inflow to sink must be smaller than supply.
  if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), LE, 0.0))
    report_fatal_error("Cannot add flow constraint!");

  Col.clear();
  Coeff.clear();

  for (CompGraphBase::iterator I = G.begin(), E = G.end(); I != E; ++I) {
    CompGraphNode *N = I;

    Col.clear();
    Coeff.clear();

    // For each node, build constraint: Outflow = 1, since the splited edge
    // implies a unit flow incoming to each node.
    typedef CompGraphNode::iterator iterator;
    for (iterator SI = N->succ_begin(), SE = N->succ_end(); SI != SE; ++SI) {
      CompGraphNode *Succ = *SI;
      unsigned EdgeIdx = Edge2IdxMap[EdgeType(N, Succ)];
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
      unsigned EdgeIdx = Edge2IdxMap[EdgeType(Pred, N)];
      assert(EdgeIdx && "Edge column number not available!");
      Col.push_back(EdgeIdx);
      Coeff.push_back(1.0);
    }

    if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), EQ, 1.0))
      report_fatal_error("Cannot add flow constraint!");
  }
  

  typedef std::vector<EdgeConsistencyBenefit>::iterator consistency_iterator;
  for (consistency_iterator I = EdgeConsistencies.begin(),
       E = EdgeConsistencies.end(); I != E; ++I) {
    // Apply a penalty on inconsistency
    const EdgeConsistencyBenefit &ECB = *I;

    // Build the consistency "soft" constraints
    // SrcEdgeFlow - DstEdgeFlow - Inconsistency >= 0
    // DstEdgeFlow - SrcEdgeFlow - Inconsistency' >= 0
    // When SrcEdgeFlow and DstEdgeFlow are both 1 (or 0), Inconsistency and
    // Inconsistency' will be 0. Otherwise, either Inconsistency or
    // Inconsistency' will be 1.
    Col.clear();
    Coeff.clear();

    Col.push_back(ECB.SrcEdgeIdx);
    Coeff.push_back(1.0);

    Col.push_back(ECB.DstEdgeIdx);
    Coeff.push_back(-1.0);

    if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), GE, 0.0))
      report_fatal_error("Cannot add consistency constraint!");

    Col.clear();
    Coeff.clear();

    Col.push_back(ECB.DstEdgeIdx);
    Coeff.push_back(1.0);

    Col.push_back(ECB.SrcEdgeIdx);
    Coeff.push_back(-1.0);

    if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), GE, 0.0))
      report_fatal_error("Cannot add consistency constraint!");
  }

  set_add_rowmode(lp, FALSE);

  return get_Nrows(lp);
}

void MinCostFlowSolver::setFUAllocationConstraints(unsigned Supply) {
  set_upbo(lp, 1, Supply);
  set_lowbo(lp, 1, Supply);
}

void MinCostFlowSolver::setCost() {
  std::vector<int> Indices;
  std::vector<REAL> Coefficients;
  typedef Edge2IdxMapTy::iterator iterator;

  for (iterator I = Edge2IdxMap.begin(), E = Edge2IdxMap.end(); I != E; ++I) {
    EdgeType Edge = I->first;
    const CompGraphNode *Src = Edge.first, *Dst = Edge.second;
    if (Src->IsTrivial || Dst->IsTrivial)
      continue;

    Indices.push_back(I->second);

    float EdgeCost = Src->getCostTo(Dst);
    Coefficients.push_back(EdgeCost);
  }

  typedef std::vector<EdgeConsistencyBenefit>::iterator consistency_iterator;
  for (consistency_iterator I = EdgeConsistencies.begin(),
       E = EdgeConsistencies.end(); I != E; ++I) {
    // Apply a penalty on inconsistency
    const EdgeConsistencyBenefit &ECB = *I;
    Indices.push_back(ECB.SrcDstConsistentVarIdx);
    Coefficients.push_back(ECB.ConsistentBenefit);

    Indices.push_back(ECB.DstSrcConsistentVarIdx);
    Coefficients.push_back(ECB.ConsistentBenefit);
  }
  

  set_obj_fnex(lp, Indices.size(), Coefficients.data(), Indices.data());
  set_minim(lp);
}

// Helper function
static const char *transSolveResult(int result) {
  if (result == -2) return "NOMEMORY";
  else if (result > 13) return "Unknown result!";

  static const char *ResultTable[] = {
    "OPTIMAL",
    "SUBOPTIMAL",
    "INFEASIBLE",
    "UNBOUNDED",
    "DEGENERATE",
    "NUMFAILURE",
    "USERABORT",
    "TIMEOUT",
    "PRESOLVED",
    "PROCFAIL",
    "PROCBREAK",
    "FEASFOUND",
    "NOFEASFOUND"
  };

  return ResultTable[result];
}

void MinCostFlowSolver::applySolveSettings() {
  set_verbose(lp, CRITICAL);
  DEBUG(set_verbose(lp, FULL));

  set_presolve(lp, PRESOLVE_NONE,
               get_presolveloops(lp));

  unsigned TotalRows = get_Nrows(lp), NumVars = get_Ncolumns(lp);
  dbgs() << "The model has " << NumVars << "x" << TotalRows << '\n';
  // Set timeout to 1 minute.
  set_timeout(lp, 20 * 60);
  DEBUG(dbgs() << "Timeout is set to " << get_timeout(lp) << "secs.\n");
}

bool MinCostFlowSolver::solveMinCostFlow() {
  write_lp(lp, "log.lp");

  int result = solve(lp);

  DEBUG(dbgs() << "ILP result is: "<< transSolveResult(result) << "\n");
  dbgs() << "Time elapsed: " << time_elapsed(lp) << "\n";

  switch (result) {
  case INFEASIBLE:
    return false;
  case SUBOPTIMAL:
    DEBUG(dbgs() << "Note: suboptimal schedule found!\n");
  case OPTIMAL:
  case PRESOLVED:
    break;
  case TIMEOUT:
    return false;
  default:
    report_fatal_error(Twine("Min cost flow fail: ")
                       + Twine(transSolveResult(result)));
  }

  float Cost = get_var_primalresult(lp, 0);
  dbgs() << "Object: " << Cost << ", Supply: "
         << get_var_primalresult(lp, 1) << '\n';

  return true;
}

unsigned
MinCostFlowSolver::buildBinging(unsigned TotalRows, BindingMapTy &BindingMap) {
  BindingMap.clear();

  unsigned FUIdx = 0;
  typedef CompGraphNode::iterator iterator;
  std::vector<std::pair<CompGraphNode*, iterator> > WorkStack;

  CompGraphNode *S = G.getEntry();
  for (iterator I = S->succ_begin(), E = S->succ_end(); I != E; ++I) {
    CompGraphNode *Succ = *I;
    if (!hasFlow(EdgeType(S, Succ), TotalRows))
      continue;

    BindingMap.insert(std::make_pair(Succ, ++FUIdx));
    WorkStack.push_back(std::make_pair(Succ, Succ->succ_begin()));
  }

  while (!WorkStack.empty()) {
    CompGraphNode *Src = WorkStack.back().first;
    iterator ChildIt = WorkStack.back().second++;

    assert(ChildIt != Src->succ_end() &&
           "We should find a flow before we reach the end of successors list!");
    CompGraphNode *Dst = *ChildIt;

    if (!hasFlow(EdgeType(Src, Dst), TotalRows))
      continue;

    // Else we find a flow.
    WorkStack.pop_back();

    // No need to go any further if we reach the exit.
    if (Dst->IsTrivial)
      continue;


    BindingMapTy::iterator I = BindingMap.find(Src);
    assert(I != BindingMap.end() && "FUIdx of source node not found!");

    BindingMap.insert(std::make_pair(Dst, I->second));
    WorkStack.push_back(std::make_pair(Dst, Dst->succ_begin()));
  }

  return FUIdx;
}

unsigned CompGraphBase::performBinding() {
  MinCostFlowSolver MCF(*this);

  MCF.createLPAndVariables();
  MCF.setCost();
  unsigned TotalRows = MCF.createBlanceConstraints();
  MCF.applySolveSettings();

  unsigned MaxFlow = Nodes.size();

  MCF.setFUAllocationConstraints(MaxFlow);
  if (!MCF.solveMinCostFlow())
    return 0;
  
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

  unsigned ActualFlow = MCF.buildBinging(TotalRows, BindingMap);
  dbgs() << "Original supply: " << Nodes.size()
         << " Minimal: " << ActualFlow << '\n';
  return ActualFlow;
}
