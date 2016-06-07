#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/PostOrderIterator.h"

using namespace llvm;
using namespace vast;

namespace llvm {
struct SIRFindCriticalPath : public SIRPass {
  SIR *SM;
  DataLayout *TD;

  std::vector<SMGNode *> SMGNodesVector;
  std::map<Value *, SMGNode *> Val2SMGNodeMap;

  typedef std::vector<SMGNode *> NodesListTy;

  typedef std::map<SMGNode *, float> ValValidTimeTy;
  ValValidTimeTy ValValidTimeMap;

  static char ID;
  SIRFindCriticalPath() : SIRPass(ID) {
    initializeSIRFindCriticalPathPass(*PassRegistry::getPassRegistry());
  }

  float getDistance(SMGNode *Src, SMGNode *Dst);
  bool updateDistance(SMGNode *Dst, float Distance);
  void findCriticalPath(Value *Root);
  void findCriticalPath();

  void topologicalSortSMGNodes();

  void buildSMGNodeEdges(SMGNode *Node);
  void buildSMG();
  bool runOnSIR(SIR &SM);

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(SIRRegisterSynthesisForCodeGenID);
    AU.setPreservesAll();
  }
};
}

char SIRFindCriticalPath::ID = 0;
char &llvm::SIRFindCriticalPathID = SIRFindCriticalPath::ID;
INITIALIZE_PASS_BEGIN(SIRFindCriticalPath, "sir-find-critical-path",
                      "Find the critical path in module",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRRegisterSynthesisForCodeGen)
INITIALIZE_PASS_END(SIRFindCriticalPath, "sir-find-critical-path",
                      "Find the critical path in module",
                      false, true)

static unsigned LogCeiling(unsigned x, unsigned n) {
  unsigned log2n = Log2_32_Ceil(n);
  return (Log2_32_Ceil(x) + log2n - 1) / log2n;
}

bool SIRFindCriticalPath::updateDistance(SMGNode *Dst, float Distance) {
  if (!ValValidTimeMap.count(Dst)) {
    return ValValidTimeMap.insert(std::make_pair(Dst, Distance)).second;
  }

  float &originDistance = ValValidTimeMap[Dst];

  if (Distance > originDistance) {
    originDistance = Distance;

    assert(ValValidTimeMap[Dst] == Distance && "Update failed!");
    return true;
  }

  return false;
}

void SIRFindCriticalPath::findCriticalPath() {
  typedef std::vector<SMGNode *>::iterator node_iterator;
  for (node_iterator NI = SMGNodesVector.begin(), NE = SMGNodesVector.end(); NI != NE; ++NI) {
    SMGNode *Src = *NI;

    if (!SM->isArgReg(Src->getValue()))
      continue;

    typedef SMGNode::child_iterator child_iterator;
    for (child_iterator CI = Src->child_begin(), CE = Src->child_end(); CI != CE; ++CI) {
      SMGNode *Dst = *CI;

      updateDistance(Dst, CI->getDistance());
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;

    typedef ValValidTimeTy::iterator iterator;
    // Use the Floyd-Warshal algorithm to compute the shortest path.
    for (iterator I = ValValidTimeMap.begin(), E = ValValidTimeMap.end(); I != E; ++I) {
      SMGNode *Thu = I->first;

      typedef SMGNode::child_iterator child_iterator;
      for (child_iterator CI = Thu->child_begin(), CE = Thu->child_end(); CI != CE; ++CI) {
        SMGNode *Dst = *CI;

        float Thu2DstDistance = CI->getDistance();
        float ThuValidTime = ValValidTimeMap[Thu];

        changed |= updateDistance(Dst, ThuValidTime + Thu2DstDistance);
      }
    }
  }
}

void SIRFindCriticalPath::buildSMGNodeEdges(SMGNode *Node) {
  Instruction *Inst = dyn_cast<Instruction>(Node->getValue());
  assert(Inst && "Unexpected SMGNode!");

  // Add all edges from current Node to its children.
  typedef Value::use_iterator use_iterator;
  for (use_iterator UI = Inst->use_begin(), UE = Inst->use_end(); UI != UE; ++UI) {
    Value *UserVal = *UI;

    if (isa<ReturnInst>(UserVal))
      continue;

    /// Get the delay of this node.
    float delay;

    // These instructions have not been transformed into SIR,
    // but clearly they cost no delay.
    if (isa<PtrToIntInst>(UserVal) || isa<IntToPtrInst>(UserVal) || isa<BitCastInst>(UserVal))
      delay = 0.0f;
    else {
      // Otherwise it must be shang intrinsic instructions.
      IntrinsicInst *II = dyn_cast<IntrinsicInst>(UserVal);
      assert(II && "Unexpected non-IntrinsicInst!");

      Intrinsic::ID ID = II->getIntrinsicID();

      switch (ID) {
        // Bit-level operations cost no delay.
      case Intrinsic::shang_bit_cat:
      case Intrinsic::shang_bit_repeat:
      case Intrinsic::shang_bit_extract: {
        delay = 0.0f;
        break;
      }

      case Intrinsic::shang_not: {
        delay = 0.0f;
        break;
      }

      case Intrinsic::shang_and:
      case Intrinsic::shang_or:
      case Intrinsic::shang_xor: {
        // To be noted that, in LLVM IR the return value
        // is counted in Operands, so the real numbers
        // of operands should be minus one.
        unsigned IONums = II->getNumOperands() - 1;
        unsigned LogicLevels = LogCeiling(IONums, VFUs::MaxLutSize);
        delay = LogicLevels * VFUs::LUTDelay;
        break;
      }
      case Intrinsic::shang_rand: {
        unsigned IONums = TD->getTypeSizeInBits(II->getOperand(0)->getType());
        unsigned LogicLevels = LogCeiling(IONums, VFUs::MaxLutSize);
        delay = LogicLevels * VFUs::LUTDelay;
        break;
      }

      case Intrinsic::shang_add:
      case Intrinsic::shang_addc: {
        unsigned BitWidth = TD->getTypeSizeInBits(II->getType());
        delay = LuaI::Get<VFUAddSub>()->lookupLatency(std::min(BitWidth, 64u));
        break;
      }
      case Intrinsic::shang_mul: {
        unsigned BitWidth = TD->getTypeSizeInBits(II->getType());
        delay = LuaI::Get<VFUMult>()->lookupLatency(std::min(BitWidth, 64u));
        break;
      }

      case Intrinsic::shang_sdiv:
      case Intrinsic::shang_udiv: {
        // Hack: Need to add the lookUpDelay function of Div into VFUs.
        delay = 345.607f;
        break;
      }

      case Intrinsic::shang_shl:
      case Intrinsic::shang_ashr:
      case Intrinsic::shang_lshr: {
        unsigned BitWidth = TD->getTypeSizeInBits(II->getType());
        delay = LuaI::Get<VFUShift>()->lookupLatency(std::min(BitWidth, 64u));
        break;
      }

      case Intrinsic::shang_sgt:
      case Intrinsic::shang_ugt: {
        unsigned BitWidth = TD->getTypeSizeInBits(II->getType());
        float Delay = LuaI::Get<VFUICmp>()->lookupLatency(std::min(BitWidth, 64u));
        break;
      }

      case  Intrinsic::shang_reg_assign: {
        // To be noted that, reg_assign instruction is created
        // to represent the SeqVal stored in register, so it
        // will devote 0 delay.
        delay = 0.0f;
        break;
      }

      default:
        llvm_unreachable("Unexpected opcode!");
        break;
      }
    }

    assert(Val2SMGNodeMap.count(UserVal) && "ChildNode not created yet?");
    SMGNode *ChildNode = Val2SMGNodeMap[UserVal];

    Node->addChildNode(ChildNode, delay);
  }
}

void SIRFindCriticalPath::buildSMG() {
  Function *F = SM->getFunction();

  // Create the SMGNodes.
  typedef Function::iterator bb_iterator;
  typedef BasicBlock::iterator inst_iterator;
  for (bb_iterator BBI = F->begin(), BBE = F->end(); BBI != BBE; ++BBI) {
    BasicBlock *BB = BBI;

    for (inst_iterator InstI = BB->begin(), InstE = BB->end(); InstI != InstE; ++InstI) {
      Instruction *Inst = InstI;

      if (isa<ReturnInst>(Inst))
        continue;

      SMGNode *Node = new SMGNode(Inst);
      SMGNodesVector.push_back(Node);
      Val2SMGNodeMap.insert(std::make_pair(Inst, Node));
    }
  }

  // Create the edges between the SMGNodes.
  typedef std::vector<SMGNode *>::iterator node_iterator;
  for (node_iterator NI = SMGNodesVector.begin(), NE = SMGNodesVector.end(); NI != NE; ++NI) {
    SMGNode *Node = *NI;

    buildSMGNodeEdges(Node);
  }
}

bool SIRFindCriticalPath::runOnSIR(SIR &SM) {
  this->TD = &getAnalysis<DataLayout>();
  this->SM = &SM;

  SM.gc();
  buildSMG();
  findCriticalPath();

  std::string ValValidTime = LuaI::GetString("ValValidTime");
  std::string Error;
  raw_fd_ostream Out(ValValidTime.c_str(), Error);

  typedef ValValidTimeTy::iterator iterator;
  for (iterator I = ValValidTimeMap.begin(), E = ValValidTimeMap.end(); I != E; ++I) {
    SMGNode *Dst = I->first;

    std::string DstName = Dst->getValue()->getName();
    if (SIRRegister *Reg = SM.lookupSIRReg(Dst->getValue()))
      DstName = Reg->getName();

    //Out << Mangle(DstName) << " valid time is " << I->second << "\n";

    SM.indexValidTime(Dst->getValue(), I->second);
  }

  return false;
}