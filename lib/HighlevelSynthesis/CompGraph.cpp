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

void CompGraphBase::computeFixedCosts() {
  for (iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
    NodeTy *Src = I;

    typedef NodeTy::iterator succ_iterator;
    for (succ_iterator I = Src->succ_begin(), E = Src->succ_end(); I != E; ++I) {
      NodeTy *Dst = *I;

      if (Dst->IsTrivial)
        continue;

      Src->setCost(Dst, computeFixedCost(Src, Dst));
    }
  }
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

float CompGraphBase::computeSavedFIMux(VASTSelector *Src,
                                        VASTSelector *Dst) const {
  std::set<unsigned> MergedFIs;
  typedef VASTSelector::iterator iterator;

  for (iterator I = Src->begin(), E = Src->end(); I != E; ++I) {
    unsigned SrashID = CST.getOrCreateStrashID(*I);
    MergedFIs.insert(SrashID);
  }

  int IntersectedFIs = 0;
  for (iterator I = Dst->begin(), E = Dst->end(); I != E; ++I) {
    unsigned SrashID = CST.getOrCreateStrashID(*I);
    if (!MergedFIs.insert(SrashID).second)
      ++IntersectedFIs;
  }

  int Bitwidth = std::max(Src->getBitWidth(), Dst->getBitWidth());

  // Remember to multiple the saved mux (1 bit) port by the bitwidth.
  return IntersectedFIs * Bitwidth;
}

float CompGraphBase::computeSavedFIMux(const CompGraphNode *Src,
                                       const CompGraphNode *Dst) const {
  assert(Src->size() == Dst->size() && "Number of operand register not agreed!");
  float Cost = 0.0f;
  for (unsigned i = 0, e = Src->size(); i != e; ++i)
    Cost += computeSavedFIMux(Src->getSelector(i), Dst->getSelector(i));

  return Cost;
}

float CompGraphBase::computeSavedFOMux(const CompGraphNode *Src,
                                        const CompGraphNode *Dst) const {
  float Cost = 0.0f;

  std::set<CompGraphNode*> NumMergedFOs;
  typedef CompGraphNode::iterator iterator;
  for (iterator I = Src->fanout_begin(), E = Src->fanout_end(); I != E; ++I)
    NumMergedFOs.insert(*I);

  unsigned IntersectedFOs = 0;
  for (iterator I = Dst->fanout_begin(), E = Dst->fanout_end(); I != E; ++I)
    if (!NumMergedFOs.insert(*I).second)
      ++IntersectedFOs;

  bool IsICmp = Src->FUType == VFUs::ICmp;
  unsigned Bitwidth = IsICmp ? 1u : Src->getSelector(0)->getBitWidth();

  // For each intersected fanouts, mux is required.
  // Assume each fanins require 1 LE
  return IntersectedFOs * Bitwidth;
}

float CompGraphBase::computeSavedResource(const CompGraphNode *Src,
                                           const CompGraphNode *Dst) const {
  float Cost = 0.0f;
  // 1. Calculate the number of registers we can reduce through this edge.
  typedef NodeTy::const_sel_iterator sel_iterator;
  for (sel_iterator I = Src->begin(), E = Src->end(); I != E; ++I)
    Cost += (*I)->getBitWidth();

  // 2. Calculate the functional unit resource reduction through this edge.
  if (VFUs::isNoTrivialFUCompatible(Src->FUType, Dst->FUType))
    Cost += std::min(Src->FUCost, Dst->FUCost);

  return Cost;
}

float
CompGraphBase::computeInterConnectConsistency(const CompGraphNode *Src,
                                              const CompGraphNode *Dst) const {
  typedef std::map<EdgeType, EdgeVector>::const_iterator iterator;
  iterator I = CompatibleEdges.find(EdgeType(const_cast<CompGraphNode*>(Src),
                                             const_cast<CompGraphNode*>(Dst)));

  if (I == CompatibleEdges.end())
    return 0.0f;

  typedef EdgeVector::const_iterator edge_iterator;
  const EdgeVector &Edges = I->second;

  unsigned NumCompatibles = 0;
  for (edge_iterator EI = Edges.begin(), EE = Edges.end(); EI != EE; ++EI) {
    NodeTy *Src = EI->first, *Dst = EI->second;

    if (Src->getBindingIdx() == Dst->getBindingIdx())
      ++NumCompatibles;
  }

  return float(NumCompatibles) / float(Edges.size());
}

void CompGraphBase::computeInterconnects(CompGraphNode *N) {
  for (unsigned i = 0, e = N->size(); i != e; ++i)
    computeInterconnects(N, i);
}

void CompGraphBase::computeInterconnects(CompGraphNode *N, unsigned SelIdx) {
  if (!N->FaninNodes[SelIdx].empty())
    return;

  extractFaninNodes(N->getSelector(SelIdx), N->FaninNodes[SelIdx]);

  typedef CompGraphNode::iterator iterator;
  for (iterator I = N->fanin_begin(SelIdx), E = N->fanin_end(SelIdx);
       I != E; ++I) {
    CompGraphNode *Fanain = *I;
    Fanain->FanoutNodes.insert(N);
  }
}

void CompGraphBase::computeCompatibleEdges(std::set<NodeTy*> &DstNodes,
                                           std::set<NodeTy*> &SrcNodes,
                                           EdgeVector &CompEdges) {
  typedef std::set<NodeTy*>::iterator iterator;
  for (iterator DI = DstNodes.begin(), DE = DstNodes.end(); DI != DE; ++DI) {
    NodeTy *Dst = *DI;

    for (iterator SI = SrcNodes.begin(), SE = SrcNodes.end(); SI != SE; ++SI) {
      NodeTy *Src = *SI;

      if (Src->countSuccessor(Dst)) {
        CompEdges.push_back(EdgeType(Src, Dst));
        ++NumCompEdges;
        continue;
      }

      if (Dst->countSuccessor(Dst)) {
        CompEdges.push_back(EdgeType(Dst, Src));
        ++NumCompEdges;
        continue;
      }

    }
  }
}

void CompGraphBase::computeCompatibleEdges(NodeTy *Dst, NodeTy *Src,
                                           EdgeVector &CompEdges) {
  assert(Src->size() == Dst->size() && "Number of operand register not agreed!");

  for (unsigned i = 0, e = Src->size(); i != e; ++i)
    computeCompatibleEdges(Dst->FaninNodes[i], Src->FaninNodes[i], CompEdges);

  computeCompatibleEdges(Dst->FanoutNodes, Src->FanoutNodes, CompEdges);
}

void CompGraphBase::computeInterconnects() {
  for (iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
    CompGraphNode *Src = I;
    computeInterconnects(Src);

    typedef NodeTy::iterator succ_iterator;
    for (succ_iterator I = Src->succ_begin(), E = Src->succ_end(); I != E; ++I) {
      NodeTy *Dst = *I;

      if (Dst->IsTrivial)
        continue;

      computeInterconnects(Dst);

      EdgeVector &CompEdges = CompatibleEdges[EdgeType(Src, Dst)];
      computeCompatibleEdges(Dst, Src, CompEdges);
      if (CompEdges.empty())
        CompatibleEdges.erase(EdgeType(Src, Dst));
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

  // Map the edge to column number in LP.
  typedef std::map<EdgeType, unsigned> Edge2IdxMapTy;
  Edge2IdxMapTy Edge2IdxMap;

  unsigned lookUpEdgeIdx(EdgeType Edge) const {
    Edge2IdxMapTy::const_iterator I = Edge2IdxMap.find(Edge);
    return I == Edge2IdxMap.end() ? 0 : I->second;
  }

  unsigned createEdgeVariables(const CompGraphNode *N, unsigned Col);
public:
  MinCostFlowSolver(CompGraphBase &G) : G(G), lp(0) {}
  ~MinCostFlowSolver() {
    delete_lp(lp);
  }

  unsigned createLPAndVariables();
  unsigned createBlanceConstraints();
  void setFUAllocationConstraints(unsigned Supply);
  void setCost(unsigned iteration);
  void applySolveSettings();
  bool solveMinCostFlow();

  typedef CompGraphBase::ClusterType ClusterType;
  typedef CompGraphBase::ClusterVectors ClusterVectors;
  unsigned buildBinging(ClusterVectors &Clusters);
  unsigned buildBinging(CompGraphNode *Src, ClusterType &Cluster);

  bool hasFlow(EdgeType E, unsigned TotalRows) const {
    Edge2IdxMapTy::const_iterator I = Edge2IdxMap.find(E);
    assert(I != Edge2IdxMap.end() && "Edge ID does not existed!");
    REAL flow = get_var_primalresult(lp, TotalRows + I->second);
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
    bool inserted
      = Edge2IdxMap.insert(std::make_pair(EdgeType(N, Succ), Col)).second;
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
  // Create the supply variable
  add_columnex(lp, 0, 0,0);
  DEBUG(set_col_name(lp, 1, "supply"));
  set_int(lp, 1, TRUE);

  // Column number of LP in lpsolve starts from 1, the other variables start
  // from 2 because we had allocated the supply variable.
  unsigned Col =  createEdgeVariables(G.getEntry(), 2);

  for (CompGraphBase::iterator I = G.begin(), E = G.end(); I != E; ++I) {
    CompGraphNode *N = I;
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

  const CompGraphNode *S = G.getEntry();
  typedef CompGraphNode::iterator iterator;
  for (iterator SI = S->succ_begin(), SE = S->succ_end(); SI != SE; ++SI) {
    CompGraphNode *Succ = *SI;
    unsigned EdgeIdx = lookUpEdgeIdx(EdgeType(S, Succ));
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
    unsigned EdgeIdx = lookUpEdgeIdx(EdgeType(Pred, T));
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

void MinCostFlowSolver::setFUAllocationConstraints(unsigned Supply) {
  set_upbo(lp, 1, Supply);
  set_lowbo(lp, 1, Supply);
}

void MinCostFlowSolver::setCost(unsigned iteration) {
  std::vector<int> Indices;
  std::vector<REAL> Coefficients;
  typedef Edge2IdxMapTy::iterator iterator;

  for (iterator I = Edge2IdxMap.begin(), E = Edge2IdxMap.end(); I != E; ++I) {
    EdgeType Edge = I->first;
    const CompGraphNode *Src = Edge.first, *Dst = Edge.second;
    if (Src->IsTrivial || Dst->IsTrivial)
      continue;

    Indices.push_back(I->second);

    float EdgeCost = G.computeCost(Src, Dst, iteration);
    Coefficients.push_back(EdgeCost);
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
  DEBUG(dbgs() << "The model has " << NumVars << "x" << TotalRows << '\n');
  // Set timeout to 1 minute.
  set_timeout(lp, 20 * 60);
  DEBUG(dbgs() << "Timeout is set to " << get_timeout(lp) << "secs.\n");
}

bool MinCostFlowSolver::solveMinCostFlow() {
  DEBUG(write_lp(lp, "log.lp"));

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

unsigned MinCostFlowSolver::buildBinging(ClusterVectors &Clusters) {
  unsigned TotalRows = getNumRows();
  unsigned BindingIdx = 0;
  Clusters.clear();
  unsigned Changed = 0;

  CompGraphNode *Entry = G.getEntry();

  typedef CompGraphNode::iterator iterator;
  for (iterator I = Entry->succ_begin(), E = Entry->succ_end(); I != E; ++I) {
    CompGraphNode *Succ = *I;
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

  dbgs() << "Changes: " << Changed << '\n';

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
  if (MCF == 0) {
    MCF = new MinCostFlowSolver(*this);

    MCF->createLPAndVariables();
    MCF->createBlanceConstraints();
    MCF->applySolveSettings();
  }

  unsigned MaxFlow = Nodes.size();

  unsigned iteration = 0;
  do {
    MCF->setFUAllocationConstraints(MaxFlow);
    MCF->setCost(++iteration);
    if (!MCF->solveMinCostFlow())
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
  } while (MCF->buildBinging(Clusters));

  return Clusters.size();
}

CompGraphBase::~CompGraphBase() {
  if (MCF)
    delete MCF;
}
