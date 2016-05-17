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
  typedef std::pair<float, NodesListTy> PathTy;
  typedef std::map<SMGNode *, PathTy> SrcPathTy;
  typedef std::map<SMGNode *, SrcPathTy> Src2EndPathTy;

  Src2EndPathTy Src2EndPathMap;

  static char ID;
  SIRFindCriticalPath() : SIRPass(ID) {
    initializeSIRFindCriticalPathPass(*PassRegistry::getPassRegistry());
  }

  float getDistance(SMGNode *Src, SMGNode *Dst);
  bool updateDistance(float Distance, NodesListTy NodeList, SMGNode *Src, SMGNode *Dst);
  void findCriticalPath(Value *Root);
  void findCriticalPath();

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

float SIRFindCriticalPath::getDistance(SMGNode *Src, SMGNode *Dst) {
  Src2EndPathTy::iterator dst_at = Src2EndPathMap.find(Dst);
  if (dst_at == Src2EndPathMap.end())
    return 0.0f;

  SrcPathTy::iterator src_at = dst_at->second.find(Src);
  if (src_at == dst_at->second.end())
    return 0.0f;

  return src_at->second.first;
}

bool SIRFindCriticalPath::updateDistance(float Distance, NodesListTy NodeList, SMGNode *Src, SMGNode *Dst) {
  float Src2DstDistance = getDistance(Src, Dst);
  // Update if the Distance is smaller than origin Distance.
  if (Distance < Src2DstDistance) {
    Src2EndPathMap[Dst][Src] = std::make_pair(Distance, NodeList);
    return true;
  }

  return false;
}

void SIRFindCriticalPath::findCriticalPath() {
  typedef std::vector<SMGNode *>::iterator node_iterator;
  for (node_iterator NI = SMGNodesVector.begin(), NE = SMGNodesVector.end(); NI != NE; ++NI) {
    SMGNode *Src = *NI;

    typedef SMGNode::child_iterator child_iterator;
    for (child_iterator CI = Src->child_begin(), CE = Src->child_end(); CI != CE; ++CI) {
      SMGNode *Dst = *CI;

      std::vector<SMGNode *> Paths;
      Paths.push_back(Dst);
      Paths.push_back(Src);

      Src2EndPathMap[Dst][Src] = std::make_pair(CI->getDistance(), Paths);
    }
  }

  ReversePostOrderTraversal<SMGNode *, GraphTraits<SMGNode *> > RPO(*SMGNodesVector.begin());
  typedef ReversePostOrderTraversal<SMGNode *, GraphTraits<SMGNode *> >::rpo_iterator iterator;

  bool changed = true;
  while (changed) {
    changed = false;

    // Use the Floyd-Warshal algorithm to compute the shortest path.
    for (iterator I = RPO.begin(), E = RPO.end(); I != E; ++I) {
      SMGNode *Dst = *I;

      typedef SMGNode::parent_iterator parent_iterator;
      for (parent_iterator PI = Dst->parent_begin(), PE = Dst->parent_end(); PI != PE; ++PI) {
        SMGNode *Thu = *PI;
        NodesListTy Thu2DstPath = Src2EndPathMap[Dst][Thu].second;

        float Thu2DstDistance = PI->getDistance();

        SrcPathTy &Srcs = Src2EndPathMap[Thu];

        typedef SrcPathTy::iterator src_iterator;
        for (src_iterator SI = Srcs.begin(), SE = Srcs.end(); SI != SE; ++SI) {
          SMGNode *Src = SI->first;
          NodesListTy Path = Thu2DstPath;

          float Src2ThuDistance = SI->second.first;
          float Src2DstDistance = Src2ThuDistance + Thu2DstDistance;

          NodesListTy Src2ThuPath = SI->second.second;
          for (unsigned i = 1; i < Src2ThuPath.size(); ++i) {
            Path.push_back(Src2ThuPath[i]);
          }

          changed |= updateDistance(Src2DstDistance, Path, Src, Dst);
        }
      }
    }
  }
}

void SIRFindCriticalPath::buildSMGNodeEdges(SMGNode *Node) {
  Instruction *Inst = dyn_cast<Instruction>(Node->getValue());
  assert(Inst && "Unexpected SMGNode!");

  /// Get the delay of this node.
  float delay;

  // These instructions have not been transformed into SIR,
  // but clearly they cost no delay.
  if (isa<PtrToIntInst>(Inst) || isa<IntToPtrInst>(Inst) || isa<BitCastInst>(Inst))
    delay = 0.0f;

  // Otherwise it must be shang intrinsic instructions.
  IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst);
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
    unsigned IONums = Inst->getNumOperands() - 1;
    unsigned LogicLevels = LogCeiling(IONums, VFUs::MaxLutSize);
    delay = LogicLevels * VFUs::LUTDelay;
    break;
  }
  case Intrinsic::shang_rand: {
    unsigned IONums = TD->getTypeSizeInBits(Inst->getOperand(0)->getType());
    unsigned LogicLevels = LogCeiling(IONums, VFUs::MaxLutSize);
    delay = LogicLevels * VFUs::LUTDelay;
    break;
  }

  case Intrinsic::shang_add:
  case Intrinsic::shang_addc: {
    unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());
    delay = LuaI::Get<VFUAddSub>()->lookupLatency(std::min(BitWidth, 64u));
    break;
  }
  case Intrinsic::shang_mul: {
    unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());
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
    unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());
    delay = LuaI::Get<VFUShift>()->lookupLatency(std::min(BitWidth, 64u));
    break;
  }

  case Intrinsic::shang_sgt:
  case Intrinsic::shang_ugt: {
    unsigned BitWidth = TD->getTypeSizeInBits(Inst->getType());
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

  // Add all edges from current Node to its children.
  typedef Value::use_iterator use_iterator;
  for (use_iterator UI = Inst->use_begin(), UE = Inst->use_end(); UI != UE; ++UI) {
    Value *UserVal = *UI;

    if (isa<ReturnInst>(UserVal))
      continue;

    assert(Val2SMGNodeMap.count(UserVal) && "ChildNode not created yet?");
    SMGNode *ChildNode = Val2SMGNodeMap[UserVal];

    Node->addChildNode(ChildNode, 0.0f - delay);
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

  float critical_path_delay = 0.0f;
  SmallVector<SMGNode *, 4> critical_path_srcs;
  SmallVector<SMGNode *, 4> critical_path_dsts;
  //SMGNode *critical_path_src;
  //SMGNode *critical_path_dst;

  typedef Src2EndPathTy::iterator src2endpath_iterator;
  typedef SrcPathTy::iterator srcpath_iterator;
  for (src2endpath_iterator SEI = Src2EndPathMap.begin(), SEE = Src2EndPathMap.end(); SEI != SEE; ++SEI) {
    SMGNode *Dst = SEI->first;

    SrcPathTy SrcPaths = SEI->second;
    for (srcpath_iterator SI = SrcPaths.begin(), SE = SrcPaths.end(); SI != SE; ++SI) {
      SMGNode *Src = SI->first;

      float delay = SI->second.first;

      if (delay < critical_path_delay) {
        critical_path_srcs.clear();
        critical_path_dsts.clear();

        critical_path_srcs.push_back(Src);
        critical_path_dsts.push_back(Dst);
        //critical_path_src = Src;
        //critical_path_dst = Dst;
      } else if (delay == critical_path_delay) {
        critical_path_srcs.push_back(Src);
        critical_path_dsts.push_back(Dst);
      }

      critical_path_delay = std::min(critical_path_delay, delay);
    }
  }

  for (unsigned i = 0; i < critical_path_srcs.size(); ++i) {
    SMGNode *critical_path_src = critical_path_srcs[i];
    SMGNode *critical_path_dst = critical_path_dsts[i];

    NodesListTy Path = Src2EndPathMap[critical_path_dst][critical_path_src].second;

    std::string SrcName = critical_path_src->getValue()->getName();
    std::string DstName = critical_path_dst->getValue()->getName();

    if (SIRRegister *Reg = SM.lookupSIRReg(critical_path_src->getValue()))
      SrcName = Reg->getName();
    if (SIRRegister *Reg = SM.lookupSIRReg(critical_path_dst->getValue()))
      DstName = Reg->getName();

    errs() << "critical path from " << SrcName << " to " << DstName << " in delay of " << critical_path_delay << "\n";
    
    std::vector<Value *> CriticalPath;
    for (unsigned j = 0; j < Path.size(); ++j) {
      SMGNode *Node = Path[j];
      std::string PathNodeName = Node->getValue()->getName();

      if (SIRRegister *Reg = SM.lookupSIRReg(Node->getValue()))
        PathNodeName = Reg->getName();

      errs() << PathNodeName << "--";

      CriticalPath.push_back(Node->getValue());
    }

    SM.indexCriticalPath(CriticalPath);
  }  

  return false;
}