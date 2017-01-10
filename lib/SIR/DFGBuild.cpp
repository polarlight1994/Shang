#include "sir/DFGBuild.h"

using namespace llvm;
using namespace vast;

char DFGBuild::ID = 0;
char &llvm::DFGBuildID = DFGBuild::ID;
INITIALIZE_PASS_BEGIN(DFGBuild, "build-DFG",
                      "Build the DFG of design",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRRegisterSynthesisForCodeGen)
INITIALIZE_PASS_END(DFGBuild, "build-DFG",
                    "Build the DFG of design",
                    false, true)

bool DFGBuild::runOnSIR(SIR &SM) {
  SM.gc();

  bool Changed = false;

  // Get the design.
  this->SM = &SM;
  // Get the data layout of target device.
  this->TD = &getAnalysis<DataLayout>();
  // Initial an empty DFG.
  this->G = new DataFlowGraph(&SM);

  // The IR representation of design.
  Function *F = SM.getFunction();

//   /// Debug code
//   std::string FinalSIR = LuaI::GetString("FinalSIR");
//   std::string ErrorInFinalSIR;
//   raw_fd_ostream OutputForFinalSIR(FinalSIR.c_str(), ErrorInFinalSIR);
//   OutputForFinalSIR << *F;

  /// Traverse the whole design and create node and its dependencies for
  /// each instruction in design.
  typedef Function::iterator bb_iterator;
  for (bb_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    BasicBlock *BB = BI;

    typedef BasicBlock::iterator inst_iterator;
    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;

      unsigned InstBitWidth = getBitWidth(Inst);

      // Do not create same node twice.
      DFGNode *Node = getOrCreateDFGNode(Inst, InstBitWidth);

      // Also create node for its operands if it has not been created.
      typedef Instruction::op_iterator op_iterator;
      for (op_iterator OI = Inst->op_begin(), OE = Inst->op_end(); OI != OE; ++OI) {
        Value *Op = *OI;

        // Ignore the function declaration.
        if (isa<Function>(Op))
          continue;

        unsigned OpBitWidth = getBitWidth(Op);
        DFGNode *OpNode = getOrCreateDFGNode(Op, OpBitWidth);

        G->createDependency(OpNode, Node);
      }
    }
  }

  /// Connect the root nodes to Entry and leaf nodes to Exit.
  DFGNode *Entry = G->getEntry();
  DFGNode *Exit = G->getExit();
  typedef DataFlowGraph::node_iterator node_iterator;
  for (node_iterator NI = G->begin(), NE = G->end(); NI != NE; ++NI) {
    DFGNode *Node = NI;

    // The Virtual Entry & Exit node will be handled later.
    if (Node->isEntryOrExit())
      continue;

    // The root nodes
    if (Node->parent_empty())
      G->createDependency(Entry, Node);

    // The leaf nodes
    if (Node->child_empty())
      G->createDependency(Node, Exit);
  }

  // Sort the nodes in topological order.
  //G->topologicalSortNodes();

  // Verify the correctness of DFG graph.
  verifyDFGCorrectness();

  return Changed;
}

DFGNode *DFGBuild::getOrCreateDFGNode(Value *V, unsigned BitWidth) {
  DFGNode *Node;
  if (Instruction *Inst = dyn_cast<Instruction>(V)) {
    if (SM->isDFGNodeExisted(V))
      Node = SM->getDFGNodeOfVal(V);
    else
      Node = G->createDataPathNode(Inst, BitWidth);
  }
  else if (ConstantInt *CI = dyn_cast<ConstantInt>(V)) {
    Node = G->createConstantIntNode(CI, BitWidth);
  }
  else if (GlobalValue *GV = dyn_cast<GlobalValue>(V)) {
    if (SM->isDFGNodeExisted(V))
      Node = SM->getDFGNodeOfVal(V);
    else
      Node = G->createGlobalValueNode(GV, BitWidth);
  }
  else if (UndefValue *UV = dyn_cast<UndefValue>(V)) {
    Node = G->createUndefValueNode(UV, BitWidth);
  }
  else if (Argument *Arg = dyn_cast<Argument>(V)) {
    if (SM->isDFGNodeExisted(V))
      Node = SM->getDFGNodeOfVal(V);
    else
      Node = G->createArgumentNode(Arg, BitWidth);
  }
  else
    llvm_unreachable("Unexpected value type!");

  return Node;
}

static unsigned LogCeiling(unsigned x, unsigned n) {
  unsigned log2n = Log2_32_Ceil(n);
  return (Log2_32_Ceil(x) + log2n - 1) / log2n;
}

void DFGBuild::verifyDFGCorrectness() const {
  // The IR representation of design.
  Function *F = SM->getFunction();

  /// Traverse the whole design and verify the created node
  /// of each instruction and its dependencies to its operands.
  typedef Function::iterator bb_iterator;
  for (bb_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    BasicBlock *BB = BI;

    typedef BasicBlock::iterator inst_iterator;
    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;

      if (isa<ReturnInst>(Inst))
        continue;

      // Verify if we have created DFG node for this instruction.
      assert(SM->isDFGNodeExisted(Inst) && "Node not created!");

      DFGNode *Node = SM->getDFGNodeOfVal(Inst);

      unsigned OpNum = 0;
      typedef Instruction::op_iterator op_iterator;
      for (op_iterator OI = Inst->op_begin(), OE = Inst->op_end(); OI != OE; ++OI) {
        Value *Op = *OI;

        // Ignore the function declaration which is also regarded as operand in llvm
        // intermediate representation.
        if (isa<Function>(Op))
          continue;

        OpNum++;

        // Ignore the constant integer and undef value.
        if (isa<ConstantInt>(Op) || isa<UndefValue>(Op))
          continue;

        // Verify if we have created DFG node for this instruction.
        assert(SM->isDFGNodeExisted(Op) && "Node not created!");

        // The corresponding DFG node of users.
        DFGNode *OpNode = SM->getDFGNodeOfVal(Op);

        assert(OpNode->hasChildNode(Node) && "Dependencies not created!");
        assert(Node->hasParentNode(OpNode) && "Dependencies not created!");
      }

      assert(OpNum == Node->parent_size() && "Some operands missed?");
    }
  }
}