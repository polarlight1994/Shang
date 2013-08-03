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

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "shang-compatibility-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumNonTranEdgeBreak, "Number of non-transitive edges are broken");

float CompGraphNodeBase::getCostTo(const CompGraphNodeBase *To) const  {
  CostVecTy::const_iterator I = SuccCosts.find(To);
  assert(I != SuccCosts.end() && "Not a Successor!");
  return I->second;
}

void
CompGraphNodeBase::setCost(const CompGraphNodeBase *To, float Cost) {
  assert(SuccCosts.count(To) && "Not a successor!");
  SuccCosts[To] = Cost;
}

void CompGraphNodeBase::print(raw_ostream &OS) const {
  OS << "LI" << Idx << " order " << Order;
  if (DomBlock)
    OS << ' ' << DomBlock->getName() << ' ';
  OS << '\n';

  OS.indent(2) << "Defs: ";
  ::dump(Defs, OS);
  OS.indent(2) << "Reachables: ";
  ::dump(Reachables, OS);
}

void CompGraphNodeBase::dump() const {
  print(dbgs());
}

void CompGraphNodeBase::merge(const CompGraphNodeBase *RHS, DominatorTree *DT) {
  Defs        |= RHS->Defs;
  Reachables  |= RHS->Reachables;
  DomBlock = DT->findNearestCommonDominator(DomBlock, RHS->DomBlock);
}

bool CompGraphNodeBase::isCompatibleWith(const CompGraphNodeBase *RHS) const {
  if (!isCompatibleWithInternal(RHS))
    return false;

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
  DomTreeNode *Root = DT->getRootNode();
  unsigned Order = 0;
  for (iterator I = df_begin(Root), E = df_end(Root); I != E; ++I)
    DTDFSOrder[(*I)->getBlock()] = ++Order;
}

bool
CompGraphBase::isBefore(CompGraphNodeBase *Src, CompGraphNodeBase *Dst) {
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

        if (!Node->isCompatibleWith(SuccSucc)) {
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

void CompGraphBase::compuateEdgeCosts() {
  for (iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
    NodeTy *Src = I;
    unsigned SrcBinding = getBinding(Src);

    typedef NodeTy::iterator succ_iterator;
    for (succ_iterator I = Src->succ_begin(), E = Src->succ_end(); I != E; ++I) {
      NodeTy *Dst = *I;

      if (Dst->IsTrivial)
        continue;

      Src->setCost(Dst, computeCost(Src, SrcBinding, Dst, getBinding(Dst)));
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
    return utostr_32((*I)->Idx);
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
  typedef std::pair<const CompGraphNodeBase*, const CompGraphNodeBase*> EdgeType;

private:
  CompGraphBase &G;
  lprec *lp;

  // Map the edge to column number in LP.
  typedef std::map<EdgeType, unsigned> Edge2IdxMapTy;
  Edge2IdxMapTy Edge2IdxMap;

  unsigned createEdgeVariables(const CompGraphNodeBase *N, unsigned Col);

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

unsigned MinCostFlowSolver::createEdgeVariables(const CompGraphNodeBase *N,
                                                unsigned Col) {
  typedef CompGraphNodeBase::iterator iterator;
  for (iterator I = N->succ_begin(), E = N->succ_end(); I != E; ++I) {
    CompGraphNodeBase *Succ = *I;
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
    CompGraphNodeBase *N = I;
    Col = createEdgeVariables(N, Col);
  }

  return Col - 1;
}

unsigned MinCostFlowSolver::createBlanceConstraints() {
  set_add_rowmode(lp, TRUE);

  // Allocate rows for source and sink node.
  std::vector<int> Col;
  std::vector<REAL> Coeff;

  const CompGraphNodeBase *S = G.getEntry();
  typedef CompGraphNodeBase::iterator iterator;
  for (iterator SI = S->succ_begin(), SE = S->succ_end(); SI != SE; ++SI) {
    CompGraphNodeBase *Succ = *SI;
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
  const CompGraphNodeBase *T = G.getExit();
  for (iterator SI = T->pred_begin(), SE = T->pred_end(); SI != SE; ++SI) {
    CompGraphNodeBase *Pred = *SI;
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
    CompGraphNodeBase *N = I;

    Col.clear();
    Coeff.clear();

    // For each node, build constraint: Outflow = 1, since the splited edge
    // implies a unit flow incoming to each node.
    typedef CompGraphNodeBase::iterator iterator;
    for (iterator SI = N->succ_begin(), SE = N->succ_end(); SI != SE; ++SI) {
      CompGraphNodeBase *Succ = *SI;
      unsigned EdgeIdx = Edge2IdxMap[EdgeType(N, Succ)];
      assert(EdgeIdx && "Edge column number not available!");
      Col.push_back(EdgeIdx);
      Coeff.push_back(1.0);
    }

    if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), EQ, 1.0))
      report_fatal_error("Cannot add flow constraint!");

    Col.clear();
    Coeff.clear();

    // For each node, build constraint: Outflow = 1, since the splited edge
    // implies a unit flow incoming to each node.
    typedef CompGraphNodeBase::iterator iterator;
    for (iterator PI = N->pred_begin(), PE = N->pred_end(); PI != PE; ++PI) {
      CompGraphNodeBase *Pred = *PI;
      unsigned EdgeIdx = Edge2IdxMap[EdgeType(Pred, N)];
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

void MinCostFlowSolver::setCost() {
  std::vector<int> Indices;
  std::vector<REAL> Coefficients;
  typedef Edge2IdxMapTy::iterator iterator;

  for (iterator I = Edge2IdxMap.begin(), E = Edge2IdxMap.end(); I != E; ++I) {
    EdgeType Edge = I->first;
    const CompGraphNodeBase *Src = Edge.first, *Dst = Edge.second;
    if (Src->IsTrivial || Src->IsTrivial)
      continue;

    Indices.push_back(I->second);

    Coefficients.push_back(Src->getCostTo(Dst));
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
  DEBUG(dbgs() << "The model has " << NumVars
               << "x" << TotalRows << '\n');
  // Set timeout to 1 minute.
  set_timeout(lp, 60);
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

  return true;
}

unsigned
MinCostFlowSolver::buildBinging(unsigned TotalRows, BindingMapTy &BindingMap) {
  BindingMap.clear();

  unsigned FUIdx = 0;
  typedef CompGraphNodeBase::iterator iterator;
  std::vector<std::pair<CompGraphNodeBase*, iterator> > WorkStack;

  CompGraphNodeBase *S = G.getEntry();
  for (iterator I = S->succ_begin(), E = S->succ_end(); I != E; ++I) {
    CompGraphNodeBase *Succ = *I;
    if (!hasFlow(EdgeType(S, Succ), TotalRows))
      continue;

    BindingMap.insert(std::make_pair(Succ, ++FUIdx));
    WorkStack.push_back(std::make_pair(Succ, Succ->succ_begin()));
  }

  while (!WorkStack.empty()) {
    CompGraphNodeBase *Src = WorkStack.back().first;
    iterator ChildIt = WorkStack.back().second++;

    assert(ChildIt != Src->succ_end() &&
           "We should find a flow before we reach the end of successors list!");
    CompGraphNodeBase *Dst = *ChildIt;

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

  unsigned MaxFlow = Nodes.size(), MinFlow = 1;

  while (MaxFlow >= MinFlow) {
    unsigned MidFlow = MinFlow + (MaxFlow - MinFlow) / 2;

    MCF.setFUAllocationConstraints(MidFlow);

    if (MCF.solveMinCostFlow())
      // MidCount is ok, try a smaller one.
      MaxFlow = MidFlow - 1;
    else
      // Otherwise we should use a bigger supply.
      MinFlow = MidFlow + 1;
  }

  dbgs() << "Original supply: " << Nodes.size()
         << " Minimal: " << MaxFlow << '\n';
  return MCF.buildBinging(TotalRows, BindingMap);
}
