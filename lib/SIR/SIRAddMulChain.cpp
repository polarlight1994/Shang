#include "sir/SIR.h"
#include "sir/SIRBuild.h"
#include "sir/SIRPass.h"
#include "sir/Passes.h"

#include "vast/LuaI.h"

#include "llvm/ADT/Statistic.h"

#include <sstream>

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

  std::vector<Value *> eliminateIdenticalOperands(std::vector<Value *> Operands, Value *ChainRoot, unsigned BitWidth);
  std::vector<Value *> OptimizeOperands(std::vector<Value *> Operands, Value *ChainRoot, unsigned BitWidth);

  void AnalysisPossiblePatterns(unsigned OperandSize, std::vector<std::vector<unsigned> > &Patterns, std::vector<unsigned> &Pattern);
  std::vector<std::vector<Value *> > PartitionOperands(std::vector<Value *> Operands);
  void PartitionOperands(std::vector<Value *> Operands, unsigned PartitionNum);

  void generateDotMatrix();
  void generateDotmatrixForChain(IntrinsicInst *ChainRoot, raw_fd_ostream &Output, raw_fd_ostream &DSOutput);
  void replaceWithCompressor();

  void printAllChain();

  void getAnalysisUsage(AnalysisUsage &AU) const {
    SIRPass::getAnalysisUsage(AU);
    AU.addRequired<DataLayout>();
    AU.addRequiredID(SIRBitMaskAnalysisID);
    AU.addRequiredID(SIRFindCriticalPathID);
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
  INITIALIZE_PASS_DEPENDENCY(SIRFindCriticalPath)
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

std::vector<Value *> SIRAddMulChain::eliminateIdenticalOperands(std::vector<Value *> Operands, Value *ChainRoot, unsigned BitWidth) {
  std::vector<Value *> FinalOperands;

  // Eliminate the same operands.
  std::map<Value *, unsigned> Op2Nums;
  for (unsigned i = 0; i < Operands.size(); ++i) {
    Value *Op = Operands[i];

    if (isa<ConstantInt>(Op)) {
      SM->indexKeepVal(Op);
      FinalOperands.push_back(Op);
      continue;
    }

    if (!Op2Nums.count(Op))
      Op2Nums.insert(std::make_pair(Op, 1));
    else
      Op2Nums[Op]++;
  }

  SIRDatapathBuilder Builder(SM, *TD);

  typedef std::map<Value *, unsigned>::iterator iterator;
  for (iterator I = Op2Nums.begin(), E = Op2Nums.end(); I != E; ++I) {
    Value *Op = I->first;
    unsigned OpBitWidth = Builder.getBitWidth(Op);
    unsigned Num = I->second;

    if (Num == 1) {
      SM->indexKeepVal(Op);
      FinalOperands.push_back(Op);
      continue;
    }

    if (Num == 2) {
      Value *ExtractResult = Builder.createSBitExtractInst(Op, OpBitWidth - 1, 0, Builder.createIntegerType(OpBitWidth - 1), ChainRoot, true);
      Value *ShiftResult = Builder.createSBitCatInst(ExtractResult, Builder.createIntegerValue(1, 0), Op->getType(), ChainRoot, true);

      SIRBitMask OpMask = SM->getBitMask(Op);
      OpMask = OpMask.shl(1);

      SM->IndexVal2BitMask(ShiftResult, OpMask);
      SM->indexKeepVal(ShiftResult);
      SM->indexValidTime(ShiftResult, SM->getValidTime(Op));
      FinalOperands.push_back(ShiftResult);
    } else {
      llvm_unreachable("Not handled yet!");
    }
  }

  return FinalOperands;
}

bool MyCompare(std::pair<Value *, float> A, std::pair<Value *, float> B) {
  return A.second < B.second;
}

std::vector<Value *> SIRAddMulChain::OptimizeOperands(std::vector<Value *> Operands, Value *ChainRoot, unsigned BitWidth) {
  // Eliminate the identical operands in add chain.
  std::vector<Value *> OptOperands = eliminateIdenticalOperands(Operands, ChainRoot, BitWidth);

  return OptOperands;
}

void SIRAddMulChain::AnalysisPossiblePatterns(unsigned OperandSize, std::vector<std::vector<unsigned> > &Patterns, std::vector<unsigned> &Pattern) {
  for (unsigned i = OperandSize; i >= 1; --i) {
    if (Pattern.size() != 0)
      if (i > Pattern.back())
        continue;

    std::vector<unsigned> LocalPattern = Pattern;

    LocalPattern.push_back(i);

    if (i == OperandSize)
      Patterns.push_back(LocalPattern);

    AnalysisPossiblePatterns(OperandSize - i, Patterns, LocalPattern);
  }
}

void SIRAddMulChain::PartitionOperands(std::vector<Value *> Operands, unsigned PartitionNum) {
  std::vector<std::vector<unsigned> > PossiblePatterns;
  std::vector<unsigned> InitPattern;

  AnalysisPossiblePatterns(Operands.size(), PossiblePatterns, InitPattern);

  for (unsigned i = 0; i < PossiblePatterns.size(); ++i) {
    std::vector<unsigned> Pattern = PossiblePatterns[i];

    for (unsigned j = 0; j < Pattern.size(); ++j) {
      errs() << "\n" << Pattern[j] << "-";
    }

    errs() << "\n";
  }


}

void SIRAddMulChain::generateDotMatrix() {
  // Print the Dot Matrix
  std::string DotMatrixOutputPath = LuaI::GetString("DotMatrix");
  std::string Error;
  raw_fd_ostream DotMatrixOutput(DotMatrixOutputPath.c_str(), Error);

  std::string DSDotMatrixOutputPath = LuaI::GetString("DSDotMatrix");
  std::string DSError;
  raw_fd_ostream DSDotMatrixOutput(DSDotMatrixOutputPath.c_str(), DSError);

  typedef std::map<IntrinsicInst *, std::set<IntrinsicInst *> >::iterator iterator;
  for (iterator I = ChainMap.begin(), E = ChainMap.end(); I != E; ++I) {
    generateDotmatrixForChain(I->first, DotMatrixOutput, DSDotMatrixOutput);
  }

  std::string CompressorName = LuaI::GetString("CompressorName");
  std::string CompressorPath = LuaI::GetString("CompressorPath");

  std::string CompressorInfoPath = LuaI::GetString("CompressorInfo");
  raw_fd_ostream CompressorInfo(CompressorInfoPath.c_str(), Error);
  CompressorInfo << CompressorName << "," << CompressorPath << ",";
}

void SIRAddMulChain::generateDotmatrixForChain(IntrinsicInst *ChainRoot, raw_fd_ostream &Output, raw_fd_ostream &DSOutput) {
  assert(ChainMap.count(ChainRoot) && "Not a chain rooted on ChainRoot!");
  std::set<IntrinsicInst *> &Chain = ChainMap[ChainRoot];

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
      }

      Operands.push_back(Operand);
    }
  }

  // Optimize operands if there are known same sign bits in two or more operands.
  std::vector<Value *> OptOperands = Operands;
  OptOperands = OptimizeOperands(Operands, ChainRoot, TD->getTypeSizeInBits(ChainRoot->getType()));

  IntrinsicInst *Compressor = ChainRoot2Compressor[ChainRoot];
  SM->IndexOps2AdderChain(Compressor, OptOperands);

  // Generate all elements in Dot Matrix.
  unsigned MatrixRowNum = OptOperands.size();
  unsigned MatrixColNum = TD->getTypeSizeInBits(ChainRoot->getType());

  std::vector<std::vector<std::string> > Matrix;
  std::vector<std::vector<std::string> > DSMatrix;
  for (unsigned i = 0; i < MatrixRowNum; ++i) {
    std::vector<std::string> Row;
    std::vector<std::string> DSRow;
    for (unsigned j = 0; j < MatrixColNum; ++j) {
      Row.push_back("1\'b0");
      DSRow.push_back("NULL");
    }      

    Matrix.push_back(Row);
    DSMatrix.push_back(DSRow);
  }

  for (unsigned i = 0; i < MatrixRowNum; ++i) {
    Value *RowVal = OptOperands[i];

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

        DSMatrix[i][MatrixColNum - 1 - j] = "0.0-0";
      }
    } else {
      float delay;
      if (SM->isArgReg(RowVal))
        delay = 0.0f;
      else
        delay = SM->getValidTime(RowVal);

      std::string LeftBracket = "[", RightBracket = "]";

      RowValName = "operand_" + utostr_32(i);

      std::string SameBit;
      for (unsigned j = 0; j < MatrixColNum; ++j) {
        std::stringstream ss;
        ss << delay;

        std::string delay_string;
        ss >> delay_string;
        ss.clear();
        DSMatrix[i][j] = delay_string + "-0";

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
            } else if (Mask.isSameKnownAt(j)) {
              if (SameBit.size() != 0)
                Matrix[i][j] = SameBit;
              else {
                SameBit = Mangle(RowValName) + LeftBracket + string_j + RightBracket;
                Matrix[i][j] = SameBit;
              }

              continue;
            }
          }

          Matrix[i][j] = Mangle(RowValName) + LeftBracket + string_j + RightBracket;
        }
        else {
          Matrix[i][j] = "1\'b0";
        }
      }
    }    
  }

  Output << "compressor_" + Mangle(Compressor->getName()) << "-" << MatrixRowNum << "-" << MatrixColNum << "\n";
  for (unsigned i = 0; i < Matrix.size(); ++i) {
    for (unsigned j = 0; j < MatrixColNum; ++j) {
      Output << Matrix[i][MatrixColNum - 1 - j] << ",";
    }

    Output << "\n";
  }

  DSOutput << "compressor_" + Mangle(Compressor->getName()) << "-" << MatrixRowNum << "-" << MatrixColNum << "\n";

  for (unsigned i = 0; i < DSMatrix.size(); ++i) {
    for (unsigned j = 0; j < MatrixColNum; ++j) {
      std::string element = DSMatrix[i][MatrixColNum - 1 - j];
      DSOutput << DSMatrix[i][MatrixColNum - 1 - j] << ",";
    }

    DSOutput << "\n";
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