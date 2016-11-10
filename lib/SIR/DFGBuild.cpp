#include "sir/DFGBuild.h"

using namespace llvm;
using namespace vast;

char DFGBuild::ID = 0;
char &llvm::DFGBuildID = DFGBuild::ID;
INITIALIZE_PASS_BEGIN(DFGBuild, "build-DFG",
                      "Build the DFG of design",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRBitMaskAnalysis)
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
    createArgumentNode(Arg);
  }

  /// Traverse the whole design and create node for
  /// each instruction in design.  
  typedef Function::iterator bb_iterator;
  for (bb_iterator BI = F->begin(), BE = F->end(); BI != BE; ++BI) {
    BasicBlock *BB = BI;

    typedef BasicBlock::iterator inst_iterator;
    for (inst_iterator II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      Instruction *Inst = II;

      // Do not create same node twice.
      if (!SM.isDFGNodeExisted(Inst))
        createDataPathNode(Inst);

      // Also create node for its operands if it has not been created.
      typedef Instruction::op_iterator op_iterator;
      for (op_iterator OI = Inst->op_begin(), OE = Inst->op_end(); OI != OE; ++OI) {
        Value *Op = *OI;

        if (!SM.isDFGNodeExisted(Op)) {
          if (Instruction *OpInst = dyn_cast<Instruction>(Op))
            createDataPathNode(OpInst);
          else if (ConstantInt *CI = dyn_cast<ConstantInt>(Op))
            createConstantIntNode(CI);
          else if (GlobalValue *GV = dyn_cast<GlobalValue>(Op))
            createGlobalValueNode(GV);
          else if (UndefValue *UV = dyn_cast<UndefValue>(Op))
            createUndefValueNode(UV);
          // To be noted that, there are some argument values created by us which are
          // not included in the function argument list. These values will be handled
          // here.
          else if (Argument *Arg = dyn_cast<Argument>(Op))
            createArgumentNode(Arg);
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

    createDependencies(Node);
  }

  /// Verify the correctness of DFG graph.
  verifyDFGCorrectness();

  return Changed;
}

static unsigned LogCeiling(unsigned x, unsigned n) {
  unsigned log2n = Log2_32_Ceil(n);
  return (Log2_32_Ceil(x) + log2n - 1) / log2n;
}

DFGNode *DFGBuild::createDataPathNode(Instruction *Inst) const {
  // Basic information of current value.
  std::string Name = Inst->getName();
  unsigned BitWidth = getBitWidth(Inst);

  // The node we want to create.
  DFGNode *Node;

  // Identify the type of instruction.
  DFGNode::NodeType Ty = DFGNode::InValid;
  if (const IntrinsicInst *InstII = dyn_cast<IntrinsicInst>(Inst)) {
    Intrinsic::ID ID = InstII->getIntrinsicID();

    switch (ID) {
    case llvm::Intrinsic::shang_add:
    case llvm::Intrinsic::shang_addc:
      Ty = DFGNode::Add;
      break;
    case llvm::Intrinsic::shang_and:
      Ty = DFGNode::And;
      break;
    case llvm::Intrinsic::shang_ashr:
      Ty = DFGNode::AShr;
      break;
    case llvm::Intrinsic::shang_bit_cat:
      Ty = DFGNode::BitCat;
      break;
    case llvm::Intrinsic::shang_bit_extract:
      Ty = DFGNode::BitExtract;
      break;
    case llvm::Intrinsic::shang_bit_repeat:
      Ty = DFGNode::BitRepeat;
      break;
    case llvm::Intrinsic::shang_compressor:
      Ty = DFGNode::CompressorTree;
      break;
    case llvm::Intrinsic::shang_eq:
      Ty = DFGNode::EQ;
      break;
    case llvm::Intrinsic::shang_logic_operations:
      Ty = DFGNode::LogicOperationChain;
      break;
    case llvm::Intrinsic::shang_lshr:
      Ty = DFGNode::LShr;
      break;
    case llvm::Intrinsic::shang_mul:
      Ty = DFGNode::Mul;
      break;
    case llvm::Intrinsic::shang_ne:
      Ty = DFGNode::NE;
      break;
    case llvm::Intrinsic::shang_not:
      Ty = DFGNode::Not;
      break;
    case llvm::Intrinsic::shang_or:
      Ty = DFGNode::Or;
      break;
    case llvm::Intrinsic::shang_rand:
      Ty = DFGNode::RAnd;
      break;
    case llvm::Intrinsic::shang_reg_assign:
      Ty = DFGNode::Register;
      break;
    case llvm::Intrinsic::shang_shl:
      Ty = DFGNode::Shl;
      break;
    case llvm::Intrinsic::shang_sdiv:
      Ty = DFGNode::Div;
      break;
    case llvm::Intrinsic::shang_udiv:
      Ty = DFGNode::Div;
      break;
    case llvm::Intrinsic::shang_sgt:
      Ty = DFGNode::GT;
      break;
    case llvm::Intrinsic::shang_ugt:
      Ty = DFGNode::GT;
      break;
    case llvm::Intrinsic::shang_xor:
      Ty = DFGNode::Xor;
      break;
    default:
      llvm_unreachable("Unexpected instruction type!");
      break;
    }

    Node = G->creatDFGNode(Name, Inst, Ty, BitWidth);
  }
  else if (isa<IntToPtrInst>(Inst) || isa<PtrToIntInst>(Inst) || isa<BitCastInst>(Inst)) {
    Node = G->creatDFGNode(Name, Inst, DFGNode::TypeConversion, BitWidth);
  }
  else if (isa<ReturnInst>(Inst)) {
    Node = G->creatDFGNode(Name, Inst, DFGNode::Ret, BitWidth);
  }
  else {
    llvm_unreachable("Unexpected instruction type!");
  }

  return Node;
}

DFGNode *DFGBuild::createConstantIntNode(ConstantInt *CI) const {
  // Basic information of current value.
  std::string Name = CI->getName();
  unsigned BitWidth = getBitWidth(CI);

  DFGNode *Node = G->creatDFGNode(Name, CI, DFGNode::ConstantInt, BitWidth);

  return Node;
}

DFGNode *DFGBuild::createGlobalValueNode(GlobalValue *GV) const {
  // Basic information of current value.
  std::string Name = GV->getName();
  unsigned BitWidth = getBitWidth(GV);

  DFGNode *Node = G->creatDFGNode(Name, GV, DFGNode::GlobalVal, BitWidth);

  return Node;
}

DFGNode *DFGBuild::createUndefValueNode(UndefValue *UV) const {
  // Basic information of current value.
  std::string Name = UV->getName();
  unsigned BitWidth = getBitWidth(UV);

  DFGNode *Node = G->creatDFGNode(Name, UV, DFGNode::UndefVal, BitWidth);

  return Node;
}

DFGNode *DFGBuild::createArgumentNode(Argument *Arg) const {
  // Basic information of current value.
  std::string Name = Arg->getName();
  unsigned BitWidth = getBitWidth(Arg);

  DFGNode *Node = G->creatDFGNode(Name, Arg, DFGNode::Argument, BitWidth);

  return Node;
}

void DFGBuild::createDependencies(DFGNode *Node) const {
  /// Create dependencies according to the dependencies of
  /// the instruction which the node represents for.
  Instruction *Inst = dyn_cast<Instruction>(Node->getValue());

  typedef Instruction::op_iterator op_iterator;
  for (op_iterator OI = Inst->op_begin(), OE = Inst->op_end(); OI != OE; ++OI) {
    Value *Op = *OI;

    // Ignore the function declaration which is also regarded as operand in llvm
    // intermediate representation.
    if (isa<Function>(Op))
      continue;

    // The corresponding DFG node of users.
    DFGNode *OpNode = SM->getDFGNodeOfVal(Op);

    createDependency(OpNode, Node);
  }
}

void DFGBuild::createDependency(DFGNode *From, DFGNode *To) const {
  From->addChildNode(To);

  assert(From->hasChildNode(To) && "Fail to create dependency!");
  assert(To->hasParentNode(From) && "Fail to create dependency!");
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