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

#include "llvm/Support/DOTGraphTraits.h"
#include "llvm/Support/GraphWriter.h"
#define DEBUG_TYPE "shang-compatibility-graph"
#include "llvm/Support/Debug.h"

using namespace llvm;

void CompGraphNode::print(raw_ostream &OS) const {
  ::dump(Reachables, OS);
}

void CompGraphNode::dump() const {
  print(dbgs());
}

void CompGraphNode::merge(const CompGraphNode *RHS, DominatorTree *DT) {
  Defs        |= RHS->Defs;
  Reachables  |= RHS->Reachables;
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

  // Alives should not intersects.
  if (intersects(Reachables, RHS->Reachables))
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

void CompGraphBase::verifyTransitive() {
  // Check a -> b and b -> c implies a -> c;
  typedef NodeVecTy::iterator node_iterator;
  for (node_iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I) {
    NodeTy *Node = I;

    for (NodeTy::iterator SI = Node->succ_begin(), SE = Node->succ_end();
         SI != SE; ++SI) {
      NodeTy *Succ = *SI;

      if (Succ->isTrivial())
        continue;

      for (NodeTy::iterator SSI = Succ->succ_begin(), SSE = Succ->succ_end();
           SSI != SSE; ++SSI) {
        NodeTy *SuccSucc = *SI;

        if (SuccSucc->isTrivial())
          continue;

        if (!Node->isCompatibleWith(SuccSucc))
          llvm_unreachable("Compatible graph is not transitive!");
      }
    }
  }
}

void CompGraphBase::recomputeCompatibility() {
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
        NodeTy::MakeEdge(Node, Other, DT);
    }

    // There will always edge from entry to a node and from node to exit.
    NodeTy::MakeEdge(&Entry, Node, 0);
    NodeTy::MakeEdge(Node, &Exit, 0);
  }
}

namespace llvm {
template<> struct DOTGraphTraits<CompGraphBase*> : public DefaultDOTGraphTraits{
  typedef CompGraphBase GraphTy;
  typedef GraphTy::NodeTy NodeTy;
  typedef NodeTy::iterator NodeIterator;

  DOTGraphTraits(bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getEdgeSourceLabel(const NodeTy *Node,NodeIterator I){
    return "0";
  }

  std::string getNodeLabel(const NodeTy *Node, const GraphTy *Graph) {
    return Node->isTrivial() ? "<null>" : "node";
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

//private:
  CompGraphBase &G;
  lprec *lp;

  // Map the edge to column number in LP.
  typedef std::map<EdgeType, unsigned> Edge2IdxMapTy;
  Edge2IdxMapTy Edge2IdxMap;

  // We need to split the nodes according to cong's paper.
  // Simultaneous FU and Register Binding Based on Network Flow Method
  typedef std::map<const CompGraphNode*, unsigned> SplitEdge2IdxMapTy;
  SplitEdge2IdxMapTy SplitEdge2IdxMap;

  unsigned createEdgeVariables(const CompGraphNode *N, unsigned Col,
                               bool SplitNode);
public:
  explicit MinCostFlowSolver(CompGraphBase &G) : G(G), lp(0) {}
  ~MinCostFlowSolver() {
    delete_lp(lp);
  }

  unsigned createLPAndVariables();
  void createBlanceConstraints();
  void setFUAllocationConstraints(unsigned Supply);
  void setCost();
  void applySolveSettings();
  bool solveMinCostFlow();
};
}

unsigned MinCostFlowSolver::createEdgeVariables(const CompGraphNode *N,
                                                unsigned Col, bool SplitNode) {
  if (SplitNode) {
    SplitEdge2IdxMap[N] = Col;
    SmallString<8> S;
    {
      raw_svector_ostream SS(S);
      SS << 'S' << Col;
    }
    set_col_name(lp, Col, const_cast<char*>(S.c_str()));
    set_int(lp, Col, TRUE);
    // Set the upper bould and lower bound to 1 to ensure a unit flow.
    set_lowbo(lp, Col, 1.0);
    set_upbo(lp, Col, 1.0);
    Col += 1;
  }

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

// Create the lp model and variables, return the number of variable created.
unsigned MinCostFlowSolver::createLPAndVariables() {
  lp = make_lp(0, 0);

  // Create the supply variable
  set_col_name(lp, 1, "supply");
  set_int(lp, 1, TRUE);

  // Column number of LP in lpsolve starts from 1, the other variables start
  // from 2 because we had allocated the supply variable.
  unsigned Col =  createEdgeVariables(G.getEntry(), 2, false);

  for (CompGraphBase::iterator I = G.begin(), E = G.end(); I != E; ++I) {
    CompGraphNode *N = I;
    Col = createEdgeVariables(N, Col, true);
  }

  return Col - 1;
}

void MinCostFlowSolver::createBlanceConstraints() {
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

    unsigned SplitEdgeIdx = SplitEdge2IdxMap[N];
    assert(SplitEdgeIdx && "Split edge column number not available!");
    Col.push_back(SplitEdgeIdx);
    Coeff.push_back(-1.0);

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

    if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), EQ, 0.0))
      report_fatal_error("Cannot add flow constraint!");

    Col.clear();
    Coeff.clear();
    Col.push_back(SplitEdgeIdx);
    Coeff.push_back(-1.0);

    // For each node, build constraint: Outflow = 1, since the splited edge
    // implies a unit flow incoming to each node.
    typedef CompGraphNode::iterator iterator;
    for (iterator PI = N->pred_begin(), PE = N->pred_end(); PI != PE; ++PI) {
      CompGraphNode *Pred = *PI;
      unsigned EdgeIdx = Edge2IdxMap[EdgeType(Pred, N)];
      assert(EdgeIdx && "Edge column number not available!");
      Col.push_back(EdgeIdx);
      Coeff.push_back(1.0);
    }

    if(!add_constraintex(lp, Col.size(), Coeff.data(), Col.data(), EQ, 0.0))
      report_fatal_error("Cannot add flow constraint!");
  }

  set_add_rowmode(lp, FALSE);
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

    if (Edge.first->isTrivial() || Edge.second->isTrivial())
      continue;

    Indices.push_back(I->second);
    // Temporary set the cost of each edge to 1.0
    Coefficients.push_back(1.0);
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
  default:
    report_fatal_error(Twine("Min cost flow fail: ")
                       + Twine(transSolveResult(result)));
  }

  return true;
}

void CompGraphBase::performBinding() {
  MinCostFlowSolver MCF(*this);

  MCF.createLPAndVariables();
  MCF.setCost();
  MCF.createBlanceConstraints();
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
}
