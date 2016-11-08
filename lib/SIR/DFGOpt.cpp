#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/DFGBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"
#include "sir/LangSteam.h"

#include "vast/FUInfo.h"
#include "vast/LuaI.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <sstream>
#include "math.h"

using namespace llvm;
using namespace vast;

namespace {
struct DFGOpt : public SIRPass {
  static char ID;
  DataLayout *TD;
  SIR *SM;
  DataFlowGraph *DFG;

  // The Operations and Operands of logic operation chain
  typedef std::pair<std::vector<DFGNode *>,
                    std::vector<DFGNode *> > OperationsAndOperandsTy;
  // The map between Root and Operation & Operand of logic operation chain
  typedef std::map<DFGNode *, OperationsAndOperandsTy> LOCType;  
  // The logic operation chains
  LOCType LOC;
  // The map between Root and the LOC DFG node we creat
  std::map<DFGNode *, DFGNode *> LOCRoot2DFGNode;

  DFGOpt() : SIRPass(ID) {
    initializeDFGOptPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSIR(SIR &SM);

  void mergeLogicOperations();
  std::vector<DFGNode *> collectMergeRootNodes();
  void traverseFromRootNode(DFGNode *RootNode);
  DFGNode *LOCBuild(DFGNode *RootNode);
  std::vector<DFGNode *> getOperationsOfLOC(DFGNode *RootNode) {
    assert(LOC.count(RootNode) && "LOC not existed!");

    return LOC[RootNode].first;
  }
  std::vector<DFGNode *> getOperandsOfLOC(DFGNode *RootNode) {
    assert(LOC.count(RootNode) && "LOC not existed!");

    return LOC[RootNode].second;
  }
  void createDependency(DFGNode *From, DFGNode *To);
  void removeDependency(DFGNode *From, DFGNode *To);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(DFGBuildID);
    AU.setPreservesAll();
  }
};
}

char DFGOpt::ID = 0;
char &llvm::DFGOptID = DFGOpt::ID;
INITIALIZE_PASS_BEGIN(DFGOpt, "DFG-optimization",
                      "Perform the DFG optimization",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(DFGBuild)
INITIALIZE_PASS_END(DFGOpt, "DFG-optimization",
                    "Perform the DFG optimization",
                    false, true)

bool DFGOpt::runOnSIR(SIR &SM) {
  this->TD = &getAnalysis<DataLayout>();
  this->SM = &SM;

  // Get the DFG.
  DFGBuild &DB = getAnalysis<DFGBuild>();
  this->DFG = DB.getDFG();

  mergeLogicOperations();

  return false;
}

void DFGOpt::mergeLogicOperations() {
  // Collect the potential root of logic operations to be merged.
  std::vector<DFGNode *> RootNodes = collectMergeRootNodes();

  /// Debug code
  std::string DebugFile = LuaI::GetString("DebugFile");
  std::string ErrorInDebugFile;
  raw_fd_ostream OutputForDebugFile(DebugFile.c_str(), ErrorInDebugFile);

  // Traverse from root and extract the logic operations chain.
  typedef std::vector<DFGNode *>::iterator node_iterator;
  for (node_iterator NI = RootNodes.begin(), NE = RootNodes.end(); NI != NE; ++NI)
    traverseFromRootNode(*NI);

  // Merge the logic operations chain as a single special datapath DFG node.
  typedef LOCType::iterator loc_iterator;
  for (loc_iterator LI = LOC.begin(), LE = LOC.end(); LI != LE; ++LI) {
    DFGNode *RootNode = LI->first;
    LOCBuild(RootNode);

    OutputForDebugFile << "Root is [" << RootNode->getName() << "]\n";
    OutputForDebugFile << "\tuse operands {";
    DFGNode *LOCNode = LOCRoot2DFGNode[RootNode];
    typedef DFGNode::iterator iterator;
    for (iterator I = LOCNode->parent_begin(), E = LOCNode->parent_end(); I != E; ++I) {
      DFGNode *ParentNode = *I;

      OutputForDebugFile << ParentNode->getName() << ", ";
    }
    OutputForDebugFile << "}, used by {";
    for (iterator I = LOCNode->child_begin(), E = LOCNode->child_end(); I != E; ++I) {
      DFGNode *ChildNode = *I;

      OutputForDebugFile << ChildNode->getName() << ", ";
    }
    OutputForDebugFile << "}\n";
  } 
}

std::vector<DFGNode *> DFGOpt::collectMergeRootNodes() {
  // Vector for the roots
  std::vector<DFGNode *> RootNodesVector;

  /// Traverse the whole design, and collect the nodes as merge root, which meet
  /// the following conditions:
  /// 1) the node type is Not, And, Or, Xor
  /// 2) the node is used by other nodes whose type is not listed in 1

  typedef DataFlowGraph::node_iterator node_iterator;
  for (node_iterator NI = DFG->begin(), NE = DFG->end(); NI != NE; ++NI) {
    DFGNode *Node = NI;
    DFGNode::NodeType Ty = Node->getType();

    if (Ty == DFGNode::Not || Ty == DFGNode::And || Ty == DFGNode::Or ||
        Ty == DFGNode::Xor) {
      typedef DFGNode::iterator child_iterator;
      for (child_iterator CI = Node->child_begin(), CE = Node->child_end();
           CI != CE; ++CI) {
        DFGNode *ChildNode = *CI;
        DFGNode::NodeType ChildTy = ChildNode->getType();

        if (ChildTy != DFGNode::Not && ChildTy != DFGNode::And &&
            ChildTy != DFGNode::Or && ChildTy != DFGNode::Xor) {
          RootNodesVector.push_back(Node);

          break;
        }
      }
    }
  }

  return RootNodesVector;
}

void DFGOpt::traverseFromRootNode(DFGNode *RootNode) {
  // The operations in current logic operation chain.
  std::vector<DFGNode *> Operations;
  // The operands of current logic operation chain.
  std::vector<DFGNode *> Operands;

  std::set<DFGNode *> Visited;

  typedef DFGNode::iterator iterator;
  std::vector<std::pair<DFGNode *, iterator> > VisitStack;

  VisitStack.push_back(std::make_pair(RootNode, RootNode->parent_begin()));

  while (!VisitStack.empty()) {
    DFGNode *CurNode = VisitStack.back().first;
    iterator &It = VisitStack.back().second;

    // All parents of current instruction have been visited.
    if (It == CurNode->parent_end()) {
      VisitStack.pop_back();
      continue;
    }

    DFGNode *ParentNode = *It;
    ++It;

    // Avoid visit same value twice.
    if (Visited.count(ParentNode))
      continue;
    Visited.insert(ParentNode);

    DFGNode::NodeType Ty = ParentNode->getType();

    // Still part of the logic operations chain.
    if (Ty == DFGNode::Not || Ty == DFGNode::And || Ty == DFGNode::Or ||
        Ty == DFGNode::Xor) {
      VisitStack.push_back(std::make_pair(ParentNode, ParentNode->parent_begin()));

      // The operation in current logic operation chain.
      Operations.push_back(ParentNode);

      continue;
    }

    // Reach the leaf, that is, the operand of current logic operation chain.
    Operands.push_back(ParentNode);
  }

  // Index the logic operation chain.
  LOC.insert(std::make_pair(RootNode, std::make_pair(Operations, Operands)));
}

DFGNode *DFGOpt::LOCBuild(DFGNode *RootNode) {
  // Get the operations and operands of LOC.
  std::vector<DFGNode *> Operations = getOperationsOfLOC(RootNode);
  std::vector<DFGNode *> Operands = getOperandsOfLOC(RootNode);

  // Create a DFG node represent the LOC.
  std::string LOCName = "LOC_" + RootNode->getName();
  unsigned LOCBitWidth = RootNode->getBitWidth();
  DFGNode *LOCNode
    = DFG->creatDFGNode(LOCName, NULL, DFGNode::LogicOperationChain, LOCBitWidth);

  // Index the root and the LOC node.
  LOCRoot2DFGNode.insert(std::make_pair(RootNode, LOCNode));

  /// Create dependencies for LOC DFG node.
  // The parents of LOC DFG node
  for (unsigned i = 0; i < Operands.size(); ++i) {
    DFGNode *OperandNode = Operands[i];

    createDependency(OperandNode, LOCNode);
  }
  // The children of LOC DFG node
  typedef DFGNode::iterator child_iterator;
  for (child_iterator CI = RootNode->child_begin(), CE = RootNode->child_end();
       CI != CE; ++CI) {
    DFGNode *ChildNode = *CI;

    createDependency(LOCNode, ChildNode);
  }

  /// Remove useless dependencies for origin LOC operation nodes.
  for (unsigned i = 0; i < Operands.size(); ++i) {
    DFGNode *OperandNode = Operands[i];

    for (unsigned j = 0; j < Operations.size(); ++j) {
      DFGNode *OperationNode = Operations[j];

      if (OperandNode->hasChildNode(OperationNode)) {
        removeDependency(OperandNode, OperationNode);
      }
    }
  }

  return LOCNode;
}

void DFGOpt::createDependency(DFGNode *From, DFGNode *To) {
  From->addChildNode(To);

  assert(From->hasChildNode(To) && "Fail to create dependency!");
  assert(To->hasParentNode(From) && "Fail to create dependency!");
}

void DFGOpt::removeDependency(DFGNode *From, DFGNode *To) {
  From->removeChildNode(To);

  assert(!From->hasChildNode(To) && "Fail to remove dependency!");
  assert(!To->hasParentNode(From) && "Fail to remove dependency!");
}
