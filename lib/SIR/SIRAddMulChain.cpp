#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

#include "llvm/ADT/Statistic.h"

using namespace llvm;
using namespace vast;

namespace {
struct SIRAddMulChain : public SIRPass {
  static char ID;
  DataLayout *TD;
  SIR *SM;

  unsigned ChainNum;

  std::set<IntrinsicInst *> Visited;
  std::set<IntrinsicInst *> Collected;
  std::map<IntrinsicInst *, std::set<IntrinsicInst *> > ChainMap;
  std::map<IntrinsicInst *, IntrinsicInst *> ChainRoot2Compressor;

  std::map<Value *, unsigned> FlattenMul2PPNum;

  SIRAddMulChain() : SIRPass(ID), ChainNum(0) {
    initializeSIRAddMulChainPass(*PassRegistry::getPassRegistry());
  }

  bool runOnSIR(SIR &SM);
  void collectAddMulChain();
  void visit(Value *Root);
  void collect(IntrinsicInst *ChainRoot);
  void collectAddShiftChain(IntrinsicInst *ChainRoot);
  void generateDotMatrix();
  void generateDotmatrixForChain(IntrinsicInst *ChainRoot, raw_fd_ostream &Output);
  void replaceWithCompressor();

  void printAllChain();

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(SIRBitMaskAnalysisID);
    AU.setPreservesAll();
  }
};
}

char SIRAddMulChain::ID = 0;
char &llvm::SIRAddMulChainID = SIRAddMulChain::ID;
INITIALIZE_PASS_BEGIN(SIRAddMulChain, "sir-add-mul-chain",
                      "Perform the add-mul chain optimization",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
  INITIALIZE_PASS_DEPENDENCY(SIRBitMaskAnalysis)
INITIALIZE_PASS_END(SIRAddMulChain, "sir-add-mul-chain",
                    "Perform the add-mul chain optimization",
                    false, true)

bool SIRAddMulChain::runOnSIR(SIR &SM) {
  this->TD = &getAnalysis<DataLayout>();
  this->SM = &SM;

  collectAddMulChain();
  printAllChain();

  replaceWithCompressor();
  generateDotMatrix();

  return false;
}

void SIRAddMulChain::collectAddMulChain() {
  Function *F = SM->getFunction();

  typedef SIR::register_iterator reg_iterator;
  for (reg_iterator RI = SM->registers_begin(), RE = SM->registers_end(); RI != RE; ++RI) {
    SIRRegister *Reg = RI;

    visit(Reg->getLLVMValue());
  }
}

void SIRAddMulChain::visit(Value *Root) {
  IntrinsicInst *RootInst = dyn_cast<IntrinsicInst>(Root);
  assert(RootInst && "Unexpected value type!");

  typedef Instruction::op_iterator op_iterator;
  std::vector<std::pair<IntrinsicInst *, op_iterator> > VisitStack;
  std::vector<IntrinsicInst *> AddInstVector;

  VisitStack.push_back(std::make_pair(RootInst, RootInst->op_begin()));

  while(!VisitStack.empty()) {
    IntrinsicInst *CurNode = VisitStack.back().first;
    op_iterator &I = VisitStack.back().second;

    // All children of current node have been visited.
    if (I == CurNode->op_end()) {
      VisitStack.pop_back();

      if (CurNode->getIntrinsicID() == Intrinsic::shang_add || CurNode->getIntrinsicID() == Intrinsic::shang_addc) {

        unsigned UsedByChainNum = 0;
        unsigned UserNum = 0;
        typedef Value::use_iterator use_iterator;
        for (use_iterator UI = CurNode->use_begin(), UE = CurNode->use_end(); UI != UE; ++UI) {
          Value *UserVal = *UI;

          if (IntrinsicInst *UserInst = dyn_cast<IntrinsicInst>(UserVal)) {
            ++UserNum;

            if (UserInst->getIntrinsicID() == Intrinsic::shang_add || UserInst->getIntrinsicID() == Intrinsic::shang_addc)
              ++UsedByChainNum;
          }
        }

        if (UsedByChainNum == 0 || UserNum >= 2)
          AddInstVector.push_back(CurNode);
      }

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    Value *ChildVal = *I;

    ++I;
    IntrinsicInst *ChildInst = dyn_cast<IntrinsicInst>(ChildVal);

    if (!ChildInst)
      continue;

    if (Visited.count(ChildInst))
      continue;

    if (ChildInst->getIntrinsicID() == Intrinsic::shang_reg_assign)
      continue;

    VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
    Visited.insert(ChildInst);
  }

  for (unsigned i = 0; i < AddInstVector.size(); ++i) {
    IntrinsicInst *AddInst = AddInstVector[i];

    collect(AddInst);
  }
}

void SIRAddMulChain::collect(IntrinsicInst *ChainRoot) {
  assert(ChainRoot->getIntrinsicID() == Intrinsic::shang_add ||
         ChainRoot->getIntrinsicID() == Intrinsic::shang_addc &&
         "Unexpected intrinsic instruction type!");

  typedef Instruction::op_iterator op_iterator;
  std::vector<std::pair<IntrinsicInst *, op_iterator> > VisitStack;

  VisitStack.push_back(std::make_pair(ChainRoot, ChainRoot->op_begin()));

  unsigned Depth = 0;
  std::set<IntrinsicInst *> Chain;
  while(!VisitStack.empty()) {
    IntrinsicInst *CurNode = VisitStack.back().first;
    op_iterator &I = VisitStack.back().second;

    assert(CurNode->getIntrinsicID() == Intrinsic::shang_add ||
           CurNode->getIntrinsicID() == Intrinsic::shang_addc && "Unexpected type!");

    // All children of current node have been visited.
    if (I == CurNode->op_end()) {
      VisitStack.pop_back();

      if (Depth != 0) {
         Chain.insert(CurNode);
         Collected.insert(CurNode);
      }

      continue;
    }

    // Otherwise, remember the node and visit its children first.
    Value *ChildVal = *I;
    ++I;
    IntrinsicInst *ChildInst = dyn_cast<IntrinsicInst>(ChildVal);

    if (!ChildInst)
      continue;

    if (Collected.count(ChildInst))
      continue;

    unsigned UsedByChainNum = 0;
    typedef Value::use_iterator use_iterator;
    for (use_iterator UI = ChildInst->use_begin(), UE = ChildInst->use_end(); UI != UE; ++UI) {
      Value *UserVal = *UI;

      if (IntrinsicInst *UserInst = dyn_cast<IntrinsicInst>(UserVal))
        ++UsedByChainNum;
    }
    if (UsedByChainNum >= 2)
      continue;

    if (ChildInst->getIntrinsicID() == Intrinsic::shang_add || ChildInst->getIntrinsicID() == Intrinsic::shang_addc) {
      VisitStack.push_back(std::make_pair(ChildInst, ChildInst->op_begin()));
      Depth++;
    }
  }

  if (Depth != 0 && Chain.size() > 2) {
    ChainMap.insert(std::make_pair(ChainRoot, Chain));
    Collected.insert(ChainRoot);
  }
}

void SIRAddMulChain::generateDotMatrix() {
  // Print the Dot Matrix
  std::string DotMatrixOutputPath = LuaI::GetString("DotMatrix");
  std::string Error;
  raw_fd_ostream DotMatrixOutput(DotMatrixOutputPath.c_str(), Error);

  typedef std::map<IntrinsicInst *, std::set<IntrinsicInst *> >::iterator iterator;
  for (iterator I = ChainMap.begin(), E = ChainMap.end(); I != E; ++I) {
    generateDotmatrixForChain(I->first, DotMatrixOutput);
  }

  std::string CompressorName = LuaI::GetString("CompressorName");
  std::string CompressorPath = LuaI::GetString("CompressorPath");

  std::string CompressorInfoPath = LuaI::GetString("CompressorInfo");
  raw_fd_ostream CompressorInfo(CompressorInfoPath.c_str(), Error);
  CompressorInfo << CompressorName << "," << CompressorPath << ",";
}

void SIRAddMulChain::generateDotmatrixForChain(IntrinsicInst *ChainRoot, raw_fd_ostream &Output) {
  assert(ChainMap.count(ChainRoot) && "Not a chain rooted on ChainRoot!");
  std::set<IntrinsicInst *> &Chain = ChainMap[ChainRoot];

/*  unsigned TotalPartialProductNum = 0;*/

  // Extract all operands added by the Chain.
  std::vector<Value *> Operands;
  typedef std::set<IntrinsicInst *>::iterator iterator;
  for (iterator I = Chain.begin(), E = Chain.end(); I != E; ++I) {
    IntrinsicInst *ChainInst = *I;

    for (unsigned i = 0; i < ChainInst->getNumOperands() - 1; ++i) {
      Value *Operand = ChainInst->getOperand(i);

      // Ignore the chain instruction itself.
      if (IntrinsicInst *OperandInst = dyn_cast<IntrinsicInst>(Operand)) {
        if (Chain.count(OperandInst))
          continue;

//         if (OperandInst->getIntrinsicID() == Intrinsic::shang_mul) {
//           Value *RHS = OperandInst->getOperand(1);
//           unsigned RHSBitWidth = TD->getTypeSizeInBits(RHS->getType());
//           unsigned PartialProductNum = (RHSBitWidth / 2) + 1;
// 
//           FlattenMul2PPNum.insert(std::make_pair(OperandInst, PartialProductNum));
//           TotalPartialProductNum += PartialProductNum;
//         }

        Intrinsic::ID ID = OperandInst->getIntrinsicID();
        // To check if there are add/mul--bit_extract--add chains exists.
        if (ID == Intrinsic::shang_shl || ID == Intrinsic::shang_lshr) {
          if (isa<ConstantInt>(OperandInst->getOperand(1))) {
            if (IntrinsicInst *Op = dyn_cast<IntrinsicInst>(OperandInst->getOperand(0))) {
              Intrinsic::ID OpID = Op->getIntrinsicID();

              if (OpID == Intrinsic::shang_add || OpID == Intrinsic::shang_addc)
                errs() << "Found one add--bit_extract--add chain\n";
              if (OpID == Intrinsic::shang_mul)
                errs() << "Found one mul--bit_extract--add chain\n";
            }
          }
        }

        if (ID == Intrinsic::shang_bit_extract) {
          if (isa<ConstantInt>(OperandInst->getOperand(1)) && isa<ConstantInt>(OperandInst->getOperand(2))) {
            if (IntrinsicInst *Op = dyn_cast<IntrinsicInst>(OperandInst->getOperand(0))) {
              Intrinsic::ID OpID = Op->getIntrinsicID();

              if (OpID == Intrinsic::shang_add || OpID == Intrinsic::shang_addc)
                errs() << "Found one add--bit_extract--add chain\n";
              if (OpID == Intrinsic::shang_mul)
                errs() << "Found one mul--bit_extract--add chain\n";
            }
          }
        }
      }

      Operands.push_back(Operand);
      SM->indexKeepVal(Operand);
    }
  }

  IntrinsicInst *Compressor = ChainRoot2Compressor[ChainRoot];
  SM->IndexOps2AdderChain(Compressor, Operands);

  // Generate all elements in Dot Matrix.
  unsigned MatrixRowNum = Operands.size();
  unsigned MatrixColNum = TD->getTypeSizeInBits(ChainRoot->getType());

  std::vector<std::vector<std::string> > Matrix;
  for (unsigned i = 0; i < MatrixRowNum; ++i) {
    std::vector<std::string> Row;
    for (unsigned j = 0; j < MatrixColNum; ++j)
      Row.push_back("1\'b0");

    Matrix.push_back(Row);
  }

  for (unsigned i = 0; i < MatrixRowNum; ++i) {
    Value *RowVal = Operands[i];

//     // Ignore the Multiplier value which is to be flattened.
//     if (FlattenMul2PPNum.count(RowVal))
//       continue;

    unsigned RowValBitWidth = TD->getTypeSizeInBits(RowVal->getType());
    std::string RowValName = RowVal->getName();

    if (RowValName.empty()) {
      ConstantInt *CI = dyn_cast<ConstantInt>(RowVal);
      assert(CI && "Unexpected value without a name!");

      unsigned CIVal = CI->getZExtValue();

      for (unsigned j = 0; j < MatrixColNum; ++j) {
        unsigned BaseVal = int(std::pow(double(2.0), double(MatrixColNum - 1 - j)));
        unsigned BitVal = CIVal / (BaseVal);
        CIVal = (CIVal >= BaseVal) ? CIVal - BaseVal : CIVal;

        if (BitVal)
          Matrix[i][MatrixColNum - 1 - j] = "1\'b1";
        else
          Matrix[i][MatrixColNum - 1 - j] = "1\'b0";
      }
    } else {
      std::string LeftBracket = "[", RightBracket = "]";

      RowValName = "operand_" + utostr_32(i);

      for (unsigned j = 0; j < MatrixColNum; ++j) {
        std::string string_j = utostr_32(j);

        if (j < RowValBitWidth) {
          if (SM->hasBitMask(RowVal)) {
            SIRBitMask Mask = SM->getBitMask(RowVal);

            if (Mask.isOneKnownAt(j)) {
              Matrix[i][j] = "1\'b1";
              continue;
            }
            else if (Mask.isZeroKnownAt(j)) {
              Matrix[i][j] = "1\'b0";
              continue;
            }
          }

          Matrix[i][j] = Mangle(RowValName) + LeftBracket + string_j + RightBracket;
        }
        else
          Matrix[i][j] = "1\'b0";
      }
    }    
  }

//   // Append the flatten multiplier operands to the Matrix.
//   typedef std::map<Value *, unsigned>::iterator map_iterator;
//   for (map_iterator I = FlattenMul2PPNum.begin(), E = FlattenMul2PPNum.end(); I != E; ++I) {
//     Value *MulVal = I->first;
//     unsigned MulValBitWidth = TD->getTypeSizeInBits(MulVal->getType());
// 
//     unsigned PPNum = I->second;
//     std::string MulValName = MulVal->getName();    
//     for (unsigned i = 0; i < PPNum; ++i) {
//       std::vector<std::string> PartialProduct;
// 
//       for (unsigned j = 0; j < MulValBitWidth; ++j) {
//         std::string PartialProductBit = MulValName + "_PP_" + utostr_32(i) + "[" + utostr_32(j) +"]";
//         PartialProduct.push_back(PartialProductBit);
//       }
// 
//       assert(PartialProduct.size() == MatrixColNum && "Unexpected Column number!");
//       Matrix.push_back(PartialProduct);
//     }
//   }


  Output << "compressor_" + Mangle(Compressor->getName()) << "-" << MatrixRowNum << "-" << MatrixColNum << "\n";
  for (unsigned i = 0; i < Matrix.size(); ++i) {
    for (unsigned j = 0; j < MatrixColNum; ++j) {
      Output << Matrix[i][MatrixColNum - 1 - j] << ",";
    }

    Output << "\n";
  }

  errs() << "compressor_" + Mangle(Compressor->getName()) << "-" << MatrixRowNum << "-" << MatrixColNum << "\n";
}

void SIRAddMulChain::replaceWithCompressor() {
  SIRDatapathBuilder Builder(SM, *TD);

  typedef std::map<IntrinsicInst *, std::set<IntrinsicInst *> >::iterator iterator;
  for (iterator I = ChainMap.begin(), E = ChainMap.end(); I != E; ++I) {
    Value *CompressorVal = Builder.createCompressorInst(I->first);

    IntrinsicInst *Compressor = dyn_cast<IntrinsicInst>(CompressorVal);
    ChainRoot2Compressor.insert(std::make_pair(I->first, Compressor));
  }
}

void SIRAddMulChain::printAllChain() {
  std::string ChainFile = LuaI::GetString("Chain");
  std::string Error;
  raw_fd_ostream ChainOutput(ChainFile.c_str(), Error);

  unsigned OptNum = 0;
  typedef std::map<IntrinsicInst *, std::set<IntrinsicInst *> >::iterator iterator;
  for (iterator I = ChainMap.begin(), E = ChainMap.end(); I != E; ++I) {
    IntrinsicInst *II = I->first;
    std::set<IntrinsicInst *> Chain = I->second;

    ChainOutput << "Root instruction is " << II->getName();
    ChainOutput << "\n";

    typedef std::set<IntrinsicInst *>::iterator chain_iterator;
    for (chain_iterator I = Chain.begin(), E = Chain.end(); I != E; ++I) {
      IntrinsicInst *ChainInst = *I;

      if (ChainInst->hasOneUse())
        ChainOutput.indent(2) << "(*****)";
      else {
        ChainOutput.indent(2) << "(";
        typedef Value::use_iterator use_iterator;
        for (use_iterator UI = ChainInst->use_begin(), UE = ChainInst->use_end(); UI != UE; ++UI) {
          Value *UserInst = *UI;

          if (!UserInst->use_empty())
            ChainOutput << UserInst->getName() << ", ";
        }
        ChainOutput << ")  ";
      }

      ChainInst->print(ChainOutput.indent(2));
      ChainOutput << "\n";
    }

    ChainOutput << "\n\n";

    OptNum += Chain.size();
  }
}