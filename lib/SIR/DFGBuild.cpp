#include "sir/DFGBuild.h"

using namespace llvm;
using namespace vast;

char DFGBuild::ID = 0;
char &llvm::DFGBuildID = DFGBuild::ID;
INITIALIZE_PASS_BEGIN(DFGBuild, "build-DFG",
                      "Build the DFG of design",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(BitMaskAnalysis)
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

  /// Create node for the argument value of design.
  typedef Function::arg_iterator arg_iterator;
  for (arg_iterator AI = F->arg_begin(), AE = F->arg_end(); AI != AE; ++AI) {
    Argument *Arg = AI;
    unsigned ArgBitWidth = getBitWidth(Arg);

    G->createArgumentNode(Arg, ArgBitWidth);
  }

  /// Traverse the whole design and create node for
  /// each instruction in design.  
  typedef Function::iterator bb_iterator;
  for (bb_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    BasicBlock *BB = BI;

    typedef BasicBlock::iterator inst_iterator;
    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;
      unsigned InstBitWidth = getBitWidth(Inst);

      // Do not create same node twice.
      if (!SM.isDFGNodeExisted(Inst))
        G->createDataPathNode(Inst, InstBitWidth);

      // Also create node for its operands if it has not been created.
      typedef Instruction::op_iterator op_iterator;
      for (op_iterator OI = Inst->op_begin(), OE = Inst->op_end(); OI != OE; ++OI) {
        Value *Op = *OI;

        // Ignore the function declaration.
        if (isa<Function>(Op))
          continue;

        unsigned OpBitWidth = getBitWidth(Op);

        if (!SM.isDFGNodeExisted(Op)) {
          if (Instruction *OpInst = dyn_cast<Instruction>(Op))
            G->createDataPathNode(OpInst, OpBitWidth);
          else if (ConstantInt *CI = dyn_cast<ConstantInt>(Op))
            G->createConstantIntNode(CI, OpBitWidth);
          else if (GlobalValue *GV = dyn_cast<GlobalValue>(Op))
            G->createGlobalValueNode(GV, OpBitWidth);
          else if (UndefValue *UV = dyn_cast<UndefValue>(Op))
            G->createUndefValueNode(UV, OpBitWidth);
          // To be noted that, there are some argument values created by us which are
          // not included in the function argument list. These values will be handled
          // here.
          else if (Argument *Arg = dyn_cast<Argument>(Op))
            G->createArgumentNode(Arg, OpBitWidth);
          else
            llvm_unreachable("Unexpected value type!");
        }
      }
    }
  }

  /// Build the dependencies between all the DFG nodes.
  typedef DataFlowGraph::node_iterator node_iterator;
  for (node_iterator NI = G->begin(), NE = G->end(); NI != NE; ++NI) {
    DFGNode *Node = NI;

    // The Virtual Entry & Exit node will be handled later.
    if (Node->isEntryOrExit())
      continue;

    Value *Val = Node->getValue();

    // Create dependency based on the nodes represent instructions.
    if (!isa<Instruction>(Val))
      continue;

    G->createDependencies(Node);
  }

  /// Verify the correctness of DFG graph.
  verifyDFGCorrectness();

  return Changed;
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

      // Verify if we have created DFG node for this instruction.
      assert(SM->isDFGNodeExisted(Inst) && "Node not created!");

      DFGNode *Node = SM->getDFGNodeOfVal(Inst);

      typedef Instruction::op_iterator op_iterator;
      for (op_iterator OI = Inst->op_begin(), OE = Inst->op_end(); OI != OE; ++OI) {
        Value *Op = *OI;

        // Ignore the function declaration which is also regarded as operand in llvm
        // intermediate representation.
        if (isa<Function>(Op))
          continue;

        // Verify if we have created DFG node for this instruction.
        assert(SM->isDFGNodeExisted(Op) && "Node not created!");

        // The corresponding DFG node of users.
        DFGNode *OpNode = SM->getDFGNodeOfVal(Op);

        assert(OpNode->hasChildNode(Node) && "Dependencies not created!");
        assert(Node->hasParentNode(OpNode) && "Dependencies not created!");
      }
    }
  }
}