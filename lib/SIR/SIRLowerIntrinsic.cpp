#include "sir/Passes.h"
#include "sir/SIR.h"
#include "sir/SIRPass.h"
#include "sir/SIRBuild.h"

#include "vast/LuaI.h"

#include "llvm/Pass.h"
#include "llvm/Linker.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/User.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/ToolOutputFile.h"
#include <memory>

using namespace llvm;
using namespace vast;

static int NumInstructionLowered = 0;
static int NumMemIntrinsicsLowered = 0;
static int NumVectorInstLowered = 0;
static int NumShuffleVectorInstLowered = 0;

namespace llvm {
struct SIRLowerIntrinsic : public ModulePass {
  static char ID;

  SIRLowerIntrinsic() : ModulePass(ID) {
    initializeSIRLowerIntrinsicPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M);

  void getAnalysisUsage(AnalysisUsage &AU) const;

  void visitAndLowerInst(Module *M, DataLayout *TD);

  bool lowerMemIntrinsic(Module *M, DataLayout *TD, MemIntrinsic *MI);
  bool lowerIntrinsic(Module *M, DataLayout *TD, IntrinsicInst *II);

  bool lowerShuffleVectorInst(SIRDatapathBuilder &Builder, ShuffleVectorInst *SVI);
  bool lowerVectorInst(SIRDatapathBuilder &Builder, Instruction *VI);
};
}

INITIALIZE_PASS_BEGIN(SIRLowerIntrinsic,
                      "SIR-Low-Intrinsic",
                      "Lower the llvm.mem intrinsics",
                      false, true)
  INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(SIRLowerIntrinsic,
                    "SIR-Low-Intrinsic",
                    "Lower the llvm.mem intrinsics",
                    false, true)

char SIRLowerIntrinsic::ID = 0;

Pass *llvm::createSIRLowerIntrinsicPass() {
  return new SIRLowerIntrinsic();
}

void SIRLowerIntrinsic::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DataLayout>();
  AU.setPreservesAll();
}

bool SIRLowerIntrinsic::lowerMemIntrinsic(Module *M, DataLayout *TD, MemIntrinsic *MI) {
  // Link the SIR-Mem-Intrinsic-BC file at the first time.
  if (!NumMemIntrinsicsLowered) {
    SMDiagnostic Err;
    std::string ErrorMessage;
    std::string InputFilename = LuaI::GetString("SIRMemIntrinsic");
    LLVMContext &Context = getGlobalContext();

    // Load the SIRMemIntrinsic BC file to get the function.
    Module *SIRMemIntrinsic = ParseIRFile(InputFilename, Err, Context);
    if (SIRMemIntrinsic == 0)
      llvm_unreachable("Error in loading file");

    // Link to the origin module.
    Linker L(M);
    if (L.linkInModule(SIRMemIntrinsic, &ErrorMessage))
      llvm_unreachable("Error in linking file");

    // Verify if we link module successfully.
    if (verifyModule(*M))
      llvm_unreachable("Linked module is broken");
  }

  Value *Dst = MI->getArgOperand(0);
  Value *Src = MI->getArgOperand(1);
  Value *Length = MI->getArgOperand(2);

  unsigned SizeInBytes = TD->getTypeStoreSize(Dst->getType());
  Value *Ops[] = { Dst, Src, Length };

  std::vector<Type *> FuncTy;
  // The operand type
  FuncTy.push_back(Dst->getType());
  FuncTy.push_back(Src->getType());
  FuncTy.push_back(Length->getType());

  std::string FN;
  Intrinsic::ID ID;
  switch(MI->getIntrinsicID()) {
  case Intrinsic::memcpy:
    FN = "sir_memcpy_" + utostr_32(SizeInBytes);
    break;
  case Intrinsic::memset:
    FN = "sir_memset_" + utostr_32(SizeInBytes);
    break;
  case Intrinsic::memmove:
    FN = "sir_memmove_" + utostr_32(SizeInBytes);
    break;
  default:
    llvm_unreachable("Unexpected MemIntrinsic");
  }

  Function *MIFunc = M->getFunction(FN);
  Instruction *SIRMemInstrinsic = CallInst::Create(MIFunc, Ops,
                                                   MI->getName(), MI);
  assert(SIRMemInstrinsic && "Unexpected NULL instruction!");

  MI->replaceAllUsesWith(SIRMemInstrinsic);

  return true;
}

bool SIRLowerIntrinsic::lowerIntrinsic(Module *M, DataLayout *TD, IntrinsicInst *II) {
  bool Changed = false;

  switch (II->getIntrinsicID()) {
  default:
    break;
  case Intrinsic::memcpy:
  case Intrinsic::memset:
  case Intrinsic::memmove:
    MemIntrinsic *MI = dyn_cast<MemIntrinsic>(II);
    if (lowerMemIntrinsic(M, TD, MI)) {
      Changed = true;
      NumMemIntrinsicsLowered++;
    }
    break;
  }

  // Set all the unused SIRMemIntrinsic functions into LocalLinkage,
  // so that we can delete the unused functions after linking.
  typedef Module::iterator iterator;
  for (iterator I = M->begin(), E = M->end();	I != E; I++) {
    Function *F = I;

    if (F->getName().startswith("sir_mem"))
      F->setLinkage(GlobalValue::InternalLinkage);
  }

  return Changed;
}

void SIRLowerIntrinsic::visitAndLowerInst(Module *M, DataLayout *TD) {
  SmallVector<Instruction *, 8> InstLowered;

  // Visit all instruction in module and lower instruction.
  typedef Module::iterator f_iterator;
  for (f_iterator I = M->begin(), E = M->end(); I != E; I++) {
    Function *F = I;

    typedef Function::iterator bb_iterator;
    for (bb_iterator I = F->begin(), E = F->end(); I != E; I++) {
      BasicBlock *BB = I;

      typedef BasicBlock::iterator inst_iterator;
      for (inst_iterator I = BB->begin(), E = BB->end(); I != E; I++) {
        Instruction *Inst = I;

        if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst)) {
          if (lowerIntrinsic(M, TD, II)) {
            NumInstructionLowered++;
            InstLowered.push_back(Inst);
            break;
          }
        }
      }
    }
  }

  // After lowering, we should remove the LoweredInst since it is useless.
  typedef SmallVector<Instruction *, 8>::iterator iterator;
  for (iterator I = InstLowered.begin(), E = InstLowered.end(); I != E; I++) {
    Instruction *LowerInst = *I;

    assert(LowerInst->use_empty() && "Unexpected Use still exist!");

    LowerInst->eraseFromParent();
  }
}

bool SIRLowerIntrinsic::runOnModule(Module &M) {
  DataLayout *TD = &getAnalysis<DataLayout>();

  // Visit and lower the instruction.
  visitAndLowerInst(&M, TD);

  return (NumInstructionLowered != 0);
}
