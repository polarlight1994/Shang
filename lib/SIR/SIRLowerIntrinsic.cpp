// #include "sir/Passes.h"
// #include "sir/SIR.h"
// #include "sir/SIRPass.h"
// #include "sir/SIRBuild.h"
// 
// #include "vast/LuaI.h"
// 
// #include "llvm/Pass.h"
// #include "llvm/Linker.h"
// #include "llvm/Analysis/Verifier.h"
// #include "llvm/Bitcode/ReaderWriter.h"
// #include "llvm/IR/LLVMContext.h"
// #include "llvm/IR/Instruction.h"
// #include "llvm/IR/Instructions.h"
// #include "llvm/IR/Module.h"
// #include "llvm/IR/User.h"
// #include "llvm/IR/IntrinsicInst.h"
// #include "llvm/IR/DataLayout.h"
// #include "llvm/IRReader/IRReader.h"
// #include "llvm/Support/CommandLine.h"
// #include "llvm/Support/ManagedStatic.h"
// #include "llvm/Support/Path.h"
// #include "llvm/Support/PrettyStackTrace.h"
// #include "llvm/Support/Signals.h"
// #include "llvm/Support/SourceMgr.h"
// #include "llvm/Support/SystemUtils.h"
// #include "llvm/Support/ToolOutputFile.h"
// #include <memory>
// 
// using namespace llvm;
// using namespace vast;
// 
// static int NumInstructionLowered = 0;
// static int NumMemIntrinsicsLowered = 0;
// static int NumVectorInstLowered = 0;
// static int NumShuffleVectorInstLowered = 0;
// 
// namespace llvm {
// struct SIRLowerIntrinsic : public SIRPass {
//   static char ID;
// 
//   SIRLowerIntrinsic() : SIRPass(ID) {
//     initializeSIRLowerIntrinsicPass(*PassRegistry::getPassRegistry());
//   }
// 
//   bool runOnSIR(SIR &SM);
// 
//   void getAnalysisUsage(AnalysisUsage &AU) const;
// 
//   void visitAndLowerInst(SIR &SM, DataLayout *TD);
// 
//   bool lowerMemIntrinsic(Module *M, DataLayout *TD, MemIntrinsic *MI);
//   bool lowerIntrinsic(Module *M, DataLayout *TD, IntrinsicInst *II);
// 
//   bool lowerShuffleVectorInst(SIRDatapathBuilder &Builder, ShuffleVectorInst *SVI);
//   bool lowerVectorInst(SIRDatapathBuilder &Builder, Instruction *VI);
// };
// }
// 
// INITIALIZE_PASS_BEGIN(SIRLowerIntrinsic,
//                       "SIR-Low-Intrinsic",
//                       "Lower the llvm.mem intrinsics",
//                       false, true)
//   INITIALIZE_PASS_DEPENDENCY(DataLayout)
// INITIALIZE_PASS_END(SIRLowerIntrinsic,
//                     "SIR-Low-Intrinsic",
//                     "Lower the llvm.mem intrinsics",
//                     false, true)
// 
// char SIRLowerIntrinsic::ID = 0;
// 
// Pass *llvm::createSIRLowerIntrinsicPass() {
//   return new SIRLowerIntrinsic();
// }
// 
// void SIRLowerIntrinsic::getAnalysisUsage(AnalysisUsage &AU) const {
//   SIRPass::getAnalysisUsage(AU);
//   AU.addRequired<DataLayout>();
//   AU.setPreservesAll();
// }
// 
// bool SIRLowerIntrinsic::lowerShuffleVectorInst(SIRDatapathBuilder &Builder,
//                                                ShuffleVectorInst *SVI) {
//   Value *VectorOp1 = SVI->getOperand(0);
//   Value *VectorOp2 = SVI->getOperand(1);
//   Value *MaskOp = SVI->getOperand(2);
// 
//   VectorType *VectorOp1Ty = dyn_cast<VectorType>(VectorOp1->getType());
//   VectorType *VectorOp2Ty = dyn_cast<VectorType>(VectorOp2->getType());
//   VectorType *MaskOpTy = dyn_cast<VectorType>(MaskOp->getType());
// 
//   assert(VectorOp1Ty == VectorOp2Ty && "Unexpected Type!");
//   assert(VectorOp1Ty && VectorOp2Ty && MaskOpTy && "Unexpected Type!");
// 
//   unsigned ElemBitWidth = Builder.getBitWidth(VectorOp1Ty->getElementType());
//   unsigned VectorOp1ElemNum = VectorOp1Ty->getNumElements();
//   unsigned VectorOp2ElemNum = VectorOp2Ty->getNumElements();
//   unsigned MaskOpElemNum = MaskOpTy->getNumElements();
// 
//   ConstantVector *MaskCV = dyn_cast<ConstantVector>(MaskOp);
//   assert(MaskCV || isa<ConstantAggregateZero>(MaskOp) && "Unexpected MaskOp Type!");
// 
//   Value *ZeroInit = Builder.createIntegerValue(32, 0);
// 
//   SmallVector<Value *, 8> Masks;
//   for (int i = 0; i < MaskOpElemNum; i++) {
//     Value *Mask = MaskCV ? MaskCV->getOperand(i) : ZeroInit;
//     Masks.push_back(Mask);
//   }
// 
//   SmallVector<Value *, 8> ResultElems;
// 
//   for (unsigned i = 0; i < MaskOpElemNum; i++) {
//     Value *Idx = Builder.createIntegerValue(32, i);
//     Value *Mask = Masks[i];
// 
//     Value *ResultElem;
//     if (ConstantInt *CI = dyn_cast<ConstantInt>(Mask)) {
//       int MaskVal = getConstantIntValue(CI);
// 
//       if (MaskVal < VectorOp1ElemNum) {
//         ResultElem = ExtractElementInst::Create(VectorOp1, Mask, "", SVI);
//       } else {
//         Value *AlignedMask = Builder.createIntegerValue(32, MaskVal - VectorOp1ElemNum);
// 
//         ResultElem = ExtractElementInst::Create(VectorOp2, AlignedMask, "", SVI);
//       }
//     } else if (UndefValue *UV = dyn_cast<UndefValue>(Mask)) {
//       // Hack: Not sure if the result is UndefValue when the mask is UndefValue.
//       ResultElem = UndefValue::get(VectorOp1Ty->getElementType());
//     }
// 
//     ResultElems.push_back(ResultElem);
//   }
// 
//   unsigned BitCatBitWidth = MaskOpElemNum * ElemBitWidth;
//   Value *BitCatResult
//     = Builder.createSBitCatInst(ResultElems,
//                                 Builder.createIntegerType(BitCatBitWidth),
//                                 SVI, true);
// 
//   Value *Result = Builder.createBitCastInst(BitCatResult, SVI->getType(), SVI, false);
// 
//   return (Result != NULL);
// }
// 
// bool SIRLowerIntrinsic::lowerVectorInst(SIRDatapathBuilder &Builder, Instruction *VI) {
//   assert(VI->getType()->isVectorTy() && "Unexpected Non-Vector instruction!");
// 
//   unsigned BitWidth = Builder.getBitWidth(VI);
//   unsigned Nums = VI->getType()->getVectorNumElements();
//   Type *ElemTy = VI->getType()->getVectorElementType();
// 
//   unsigned NumOperands = VI->getNumOperands();
// 
//   SmallVector<Value *, 4> PartInsts;
// 
//   for (int i = Nums - 1; i >= 0; i--) {
//     SmallVector<Value *, 4> PartOps;
// 
//     for (int j = 0; j < NumOperands; j++) {
//       Value *Operand_j = VI->getOperand(j);
//       assert(Operand_j->getType()->isVectorTy() && "Unexpected operand Type!");
// 
//       Value *Idx = Builder.createIntegerValue(32, i);
//       Value *Operand_j_i_Elem = ExtractElementInst::Create(Operand_j, Idx, "", VI);
//       PartOps.push_back(Operand_j_i_Elem);
//     }
// 
//     switch (VI->getOpcode()) {
//     default:
//       llvm_unreachable("Unexpected Opcode!");
//     case Instruction::ICmp: {
//       ICmpInst *ICI = dyn_cast<ICmpInst>(VI);
//       assert(ICI && "Unexpected NULL ICI!");
//       assert(PartOps.size() == 2 && "Unexpected operand size!");
// 
//       Value *PartInst
//         = new ICmpInst(VI, ICI->getPredicate(), PartOps[0], PartOps[1], "");
//       PartInsts.push_back(PartInst);
// 
//       break;
//                             }
//     case Instruction::ZExt: {
//       ZExtInst *ZI = dyn_cast<ZExtInst>(VI);
//       assert(ZI && "Unexpected NULL ZI!");
//       assert(PartOps.size() == 1 && "Unexpected operand size!");
// 
//       Value *PartInst = new ZExtInst(PartOps[0], ElemTy, "", VI);
//       PartInsts.push_back(PartInst);
// 
//       break;
//                             }
//     case Instruction::Add:
//     case Instruction::Sub:
//     case Instruction::Mul:
//     case Instruction::Shl:
//     case Instruction::AShr:
//     case Instruction::LShr:
//     case Instruction::UDiv:
//     case Instruction::SDiv:
//     case Instruction::And:
//     case Instruction::Or:
//     case Instruction::Xor:
//       BinaryOperator *BO = dyn_cast<BinaryOperator>(VI);
//       assert(BO && "Unexpected NULL BO!");
//       assert(PartOps.size() == 2 && "Unexpected operand size!");
// 
//       Value *PartInst
//         = BinaryOperator::Create(BinaryOperator::BinaryOps(VI->getOpcode()),
//                                  PartOps[0], PartOps[1], "", VI);
//       PartInsts.push_back(PartInst);
// 
//       break;
// 
//     }
//   }
// 
//   Value *BitCatResult
//     = Builder.createSBitCatInst(PartInsts,
//                                 Builder.createIntegerType(BitWidth),
//                                 VI, true);
// 
//   Value *Result = Builder.createBitCastInst(BitCatResult, VI->getType(), VI, false);
// 
//   return (Result != NULL);
// }
// 
// bool SIRLowerIntrinsic::lowerMemIntrinsic(Module *M, DataLayout *TD, MemIntrinsic *MI) {
//   // Link the SIR-Mem-Intrinsic-BC file at the first time.
//   if (!NumMemIntrinsicsLowered) {
//     SMDiagnostic Err;
//     std::string ErrorMessage;
//     std::string InputFilename = LuaI::GetString("SIRMemIntrinsic");
//     LLVMContext &Context = getGlobalContext();
// 
//     // Load the SIRMemIntrinsic BC file to get the function.
//     Module *SIRMemIntrinsic = ParseIRFile(InputFilename, Err, Context);
//     if (SIRMemIntrinsic == 0)
//       llvm_unreachable("Error in loading file");
// 
//     // Link to the origin module.
//     Linker L(M);
//     if (L.linkInModule(SIRMemIntrinsic, &ErrorMessage))
//       llvm_unreachable("Error in linking file");
// 
//     // Verify if we link module successfully.
//     if (verifyModule(*M))
//       llvm_unreachable("Linked module is broken");
//   }
// 
//   Value *Dst = MI->getArgOperand(0);
//   Value *Src = MI->getArgOperand(1);
//   Value *Length = MI->getArgOperand(2);
// 
//   unsigned SizeInBytes = TD->getTypeStoreSize(Dst->getType());
//   Value *Ops[] = { Dst, Src, Length };
// 
//   std::vector<Type *> FuncTy;
//   // The operand type
//   FuncTy.push_back(Dst->getType());
//   FuncTy.push_back(Src->getType());
//   FuncTy.push_back(Length->getType());
// 
//   std::string FN;
//   Intrinsic::ID ID;
//   switch(MI->getIntrinsicID()) {
//   case Intrinsic::memcpy:
//     FN = "sir_memcpy_" + utostr_32(SizeInBytes);
//     break;
//   case Intrinsic::memset:
//     FN = "sir_memset_" + utostr_32(SizeInBytes);
//     break;
//   case Intrinsic::memmove:
//     FN = "sir_memmove_" + utostr_32(SizeInBytes);
//     break;
//   default:
//     llvm_unreachable("Unexpected MemIntrinsic");
//   }
// 
//   Function *MIFunc = M->getFunction(FN);
//   Instruction *SIRMemInstrinsic = CallInst::Create(MIFunc, Ops,
//     MI->getName(), MI);
//   assert(SIRMemInstrinsic && "Unexpected NULL instruction!");
// 
//   MI->replaceAllUsesWith(SIRMemInstrinsic);
// 
//   return true;
// }
// 
// bool SIRLowerIntrinsic::lowerIntrinsic(Module *M, DataLayout *TD, IntrinsicInst *II) {
//   bool Changed = false;
// 
//   switch (II->getIntrinsicID()) {
//   default:
//     break;
//   case Intrinsic::memcpy:
//   case Intrinsic::memset:
//   case Intrinsic::memmove:
//     MemIntrinsic *MI = dyn_cast<MemIntrinsic>(II);
//     if (lowerMemIntrinsic(M, TD, MI)) {
//       Changed = true;
//       NumMemIntrinsicsLowered++;
//     }
//     break;
//   }
// 
//   // Set all the unused SIRMemIntrinsic functions into LocalLinkage,
//   // so that we can delete the unused functions after linking.
//   typedef Module::iterator iterator;
//   for (iterator I = M->begin(), E = M->end();	I != E; I++) {
//     Function *F = I;
// 
//     if (F->getName().startswith("sir_mem"))
//       F->setLinkage(GlobalValue::InternalLinkage);
//   }
// 
//   return Changed;
// }
// 
// void SIRLowerIntrinsic::visitAndLowerInst(SIR &SM, DataLayout *TD) {
//   SIRDatapathBuilder Builder(&SM, *TD);
// 
//   Module *M = SM.getModule();
// 
//   SmallVector<Instruction *, 8> InstLowered;
// 
//   // Visit all instruction in module and lower instruction.
//   typedef Module::iterator f_iterator;
//   for (f_iterator I = M->begin(), E = M->end(); I != E; I++) {
//     Function *F = I;
// 
//     typedef Function::iterator bb_iterator;
//     for (bb_iterator I = F->begin(), E = F->end(); I != E; I++) {
//       BasicBlock *BB = I;
// 
//       typedef BasicBlock::iterator inst_iterator;
//       for (inst_iterator I = BB->begin(), E = BB->end(); I != E; I++) {
//         Instruction *Inst = I;
// 
//         if (isa<ShuffleVectorInst>(Inst))
//           int I = 0;
// 
//         if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst)) {
//           if (lowerIntrinsic(M, TD, II)) {
//             NumInstructionLowered++;
//             InstLowered.push_back(Inst);
//             break;
//           }
//         }
// 
//         if (Inst->getType()->isVectorTy()) {
//           if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst) ||
//               isa<InsertElementInst>(Inst) ||
//               isa<ExtractElementInst>(Inst) ||
//               isa<PHINode>(Inst) || isa<BitCastInst>(Inst))
//             continue;
// 
//           if (ShuffleVectorInst *SVI = dyn_cast<ShuffleVectorInst>(Inst)) {
//             if (lowerShuffleVectorInst(Builder, SVI)) {
//               NumShuffleVectorInstLowered++;
//               InstLowered.push_back(Inst);
//               break;
//             }
//           }
// 
//           if (lowerVectorInst(Builder, Inst)) {
//             NumVectorInstLowered++;
//             InstLowered.push_back(Inst);
//             break;
//           }
//         }
//       }
//     }
//   }
// 
//   // After lowering, we should remove the LoweredInst since it is useless.
//   typedef SmallVector<Instruction *, 8>::iterator iterator;
//   for (iterator I = InstLowered.begin(), E = InstLowered.end(); I != E; I++) {
//     Instruction *LowerInst = *I;
// 
//     assert(LowerInst->use_empty() && "Unexpected Use still exist!");
// 
//     LowerInst->eraseFromParent();
//   }
// }
// 
// bool SIRLowerIntrinsic::runOnSIR(SIR &SM) {
//   DataLayout *TD = &getAnalysis<DataLayout>();
// 
//   // Visit and lower the instruction.
//   visitAndLowerInst(SM, TD);
// 
//   return (NumInstructionLowered != 0);
// }
