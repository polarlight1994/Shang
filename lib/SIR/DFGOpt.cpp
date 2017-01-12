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

  DFGOpt() : SIRPass(ID) {
    initializeDFGOptPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSIR(SIR &SM);

  //void shrinkOperatorStrength();

  void mergeLogicOperations();
  std::vector<DFGNode *> collectMergeRootNodes();
  void traverseFromRootNode(DFGNode *RootNode);
  DFGNode *LOCBuild(DFGNode *RootNode);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(DFGBuildID);
    AU.addRequiredID(BitMaskAnalysisID);
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
  INITIALIZE_PASS_DEPENDENCY(BitMaskAnalysis)
INITIALIZE_PASS_END(DFGOpt, "DFG-optimization",
                    "Perform the DFG optimization",
                    false, true)

bool DFGOpt::runOnSIR(SIR &SM) {
  this->TD = &getAnalysis<DataLayout>();
  this->SM = &SM;

  // Get the DFG.
  DFGBuild &DB = getAnalysis<DFGBuild>();
  this->DFG = DB.getDFG();

  //shrinkOperatorStrength();
  mergeLogicOperations();

  return false;
}

// void DFGOpt::shrinkOperatorStrength() {
//   /// Eliminate the unnecessary nodes according to BitMask.
//   typedef DataFlowGraph::node_iterator node_iterator;
//   for (node_iterator NI = DFG->begin(), NE = DFG->end(); NI != NE; ++NI) {
//     DFGNode *Node = NI;
// 
//     // Ignore the virtual Entry/Exit node.
//     if (Node->isEntryOrExit() || Node->getType() == DFGNode::Ret)
//       continue;
// 
//     DFGNode::NodeType Ty = Node->getType();
//     unsigned BitWidth = Node->getBitWidth();
//     Value *Val = Node->getValue();
//     BitMask Mask;
// 
//     if (SM->hasBitMask(Node))
//       Mask = SM->getBitMask(Node);
//     else if (Node->getType() == DFGNode::Argument ||
//              Node->getType() == DFGNode::UndefVal)
//       Mask = BitMask(BitWidth);
//     else
//       Mask = SM->getBitMask(Val);
// 
//     // Get the mask of its parent nodes.
//     std::vector<BitMask> ParentMasks;
//     typedef DFGNode::iterator parent_iterator;
//     for (parent_iterator PI = Node->parent_begin(), PE = Node->parent_end();
//          PI != PE; ++PI) {
//       DFGNode *ParentNode = *PI;
// 
//       if (ParentNode->isEntryOrExit())
//         continue;
// 
//       BitMask ParentMask = SM->getBitMask(ParentNode);
//       ParentMasks.push_back(ParentMask);
//     }
// 
//     // If the value of current node is already known as a constant, then
//     // we can create a ConstantInt DFG node to replace it.
//     if (Mask.isAllBitKnown() && Node->getType() != DFGNode::ConstantInt) {
//       APInt CI = Mask.getKnownOnes();
// 
//       DFGNode *CINode = DFG->createConstantIntNode(CI.getZExtValue(), BitWidth);
//       Node->replaceAllUseWith(CINode);
//     }    
// 
//     // If the node type is And or Or, then this node can be eliminated
//     // when each bit can be computed without any logic levels.
//     if (Ty == DFGNode::And || Ty == DFGNode::Or) {
//       bool CanBeEliminated = true;
// 
//       for (unsigned i = 0; i < BitWidth; ++i) {
//         unsigned UnKnown = 0;
//         for (unsigned j = 0; j < ParentMasks.size(); ++j) {
//           BitMask ParentMask = ParentMasks[j];
// 
//           if (!ParentMask.isBitKnownAt(i))
//             ++UnKnown;
//         }
// 
//         if (UnKnown > 1)
//           CanBeEliminated = false;
//       }
// 
//       if (CanBeEliminated) {
//         std::string BMNodeName = "BM_" + Node->getName();
//         DFGNode *BMNode = DFG->createDFGNode(BMNodeName, NULL, Node->getParentBB(),
//                                              DFGNode::BitManipulate, BitWidth);
//         SM->IndexNode2BitMask(BMNode, Mask);
// 
//         Node->replaceAllWith(BMNode);
//         DFG->indexReplaceNode(Node, BMNode);
//       }
//     }
//   }
// }

void DFGOpt::mergeLogicOperations() {
  // Collect the potential root of logic operations to be merged.
  std::vector<DFGNode *> RootNodes = collectMergeRootNodes();

//   /// Debug code
//   std::string DebugFile = LuaI::GetString("DebugFile");
//   std::string ErrorInDebugFile;
//   raw_fd_ostream OutputForDebugFile(DebugFile.c_str(), ErrorInDebugFile);

  // Traverse from root and extract the logic operations chain.
  typedef std::vector<DFGNode *>::iterator node_iterator;
  for (node_iterator NI = RootNodes.begin(), NE = RootNodes.end(); NI != NE; ++NI)
    traverseFromRootNode(*NI);

  // Merge the logic operations chain as a single special datapath DFG node.
  typedef DataFlowGraph::loc_iterator loc_iterator;
  for (loc_iterator LI = DFG->loc_begin(), LE = DFG->loc_end(); LI != LE; ++LI) {
    DFGNode *RootNode = LI->first;
    DFGNode *LOCNode = LOCBuild(RootNode);

    // Also index the mask for the new created LOCNode.
    BitMask Mask = SM->getBitMask(RootNode);
    SM->IndexNode2BitMask(LOCNode, Mask);

//     std::vector<DFGNode *> Operations = DFG->getOperationsOfLOC(RootNode);
//     std::vector<DFGNode *> Operands = DFG->getOperandsOfLOC(RootNode);
// 
//     for (unsigned i = 0; i < Operations.size(); ++i) {
//       DFGNode *Operation = Operations[i];
// 
//       Operation->getValue()->print(OutputForDebugFile);
//       OutputForDebugFile << "\n";
//     }
//     
//     OutputForDebugFile << "\nOperands include: ";
//     for (unsigned i = 0; i < Operands.size(); ++i) {
//       DFGNode *Operand = Operands[i];
// 
//       OutputForDebugFile << Operand->getName() << ", ";
//     }
//     OutputForDebugFile << "\n";
// 
//     OutputForDebugFile << "\nReal operands include: ";
//     typedef DFGNode::iterator iterator;
//     for (iterator I = LOCNode->parent_begin(), E = LOCNode->parent_end(); I != E; ++I) {
//       DFGNode *ParentNode = *I;
// 
//       OutputForDebugFile << ParentNode->getName() << ", ";
//     }
//     OutputForDebugFile << "\n";
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

  typedef DFGNode::iterator iterator;
  std::vector<std::pair<DFGNode *, iterator> > VisitStack;

  VisitStack.push_back(std::make_pair(RootNode, RootNode->parent_begin()));
  Operations.push_back(RootNode);

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

    DFGNode::NodeType Ty = ParentNode->getType();

    // Still part of the logic operations chain.
    if (ParentNode->child_size() == 1) {
      if (Ty == DFGNode::Not || Ty == DFGNode::And ||
          Ty == DFGNode::Or || Ty == DFGNode::Xor) {
        VisitStack.push_back(std::make_pair(ParentNode, ParentNode->parent_begin()));

        // The operation in current logic operation chain.
        Operations.push_back(ParentNode);

        continue;
      }
    }

    // Reach the leaf, that is, the operand of current logic operation chain.
    Operands.push_back(ParentNode);
  }

  // Ignore the single logic operation.
  if (Operations.size() == 1)
    return;

  // Index the logic operation chain.
  DFG->indexLOC(RootNode, Operations, Operands);
}

DFGNode *DFGOpt::LOCBuild(DFGNode *RootNode) {
  // Get the operations and operands of LOC.
  std::vector<DFGNode *> Operations = DFG->getOperationsOfLOC(RootNode);
  std::vector<DFGNode *> Operands = DFG->getOperandsOfLOC(RootNode);

  // Create a DFG node represent the LOC.
  std::string LOCName = "LOC_" + RootNode->getName();
  unsigned LOCBitWidth = RootNode->getBitWidth();
  DFGNode *LOCNode
    = DFG->createDFGNode(LOCName, NULL, NULL, RootNode->getParentBB(),
                         DFGNode::LogicOperationChain, LOCBitWidth);

  // Index the root and the LOC node.
  DFG->indexRootOfLOC(RootNode, LOCNode);

  /// Create dependencies for LOC DFG node.
  // The parents of LOC DFG node
  for (unsigned i = 0; i < Operands.size(); ++i) {
    DFGNode *OperandNode = Operands[i];

    DFG->createDependency(OperandNode, LOCNode);

    for (unsigned j = 0; j < Operations.size(); ++j) {
      DFGNode *OperationNode = Operations[j];

      if (OperationNode->hasParentNode(OperandNode)) {
        DFG->removeDependency(OperandNode, OperationNode);
      }
    }
  }
  // The children of LOC DFG node
  std::vector<DFGNode *> Childs;
  typedef DFGNode::iterator child_iterator;
  for (child_iterator CI = RootNode->child_begin(), CE = RootNode->child_end();
       CI != CE; ++CI) {
    DFGNode *ChildNode = *CI;
    Childs.push_back(ChildNode);
  }

  for (unsigned i = 0; i < Childs.size(); ++i) {
    DFGNode *ChildNode = Childs[i];
    DFG->replaceDepSrc(ChildNode, RootNode, LOCNode);
  }

  return LOCNode;
}
