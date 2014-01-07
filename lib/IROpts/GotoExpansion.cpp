//===--------- GotoExpansion.cpp ---Perform Goto Expansion in HLS ---------===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// Do not duplicate
// translate the problem to
// Form single-entry single-exit region
// introduce false pass
//===----------------------------------------------------------------------===//

#include "vast/FUInfo.h"
#include "vast/Passes.h"

#include "llvm/Pass.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#define DEBUG_TYPE "shang-goto-expansion"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumFnExapnded, "Number of Functions goto-expanded");
STATISTIC(NumCallsDeleted, "Number of call sites deleted, not goto-expanded");
STATISTIC(NumCrossBBValue,
          "Number of value demote to stack again after Call BB is splited");

static cl::opt<bool> InitStacks(
"shang-goto-expansion-initialize-stack",
cl::desc("Initialize the newly create stack variable by 0"),
cl::init(false));
namespace {
struct ExpandedFunctionInfo {
  /// The expanded function will become a single-entry single-exit function.
  BasicBlock *Entry, *Exit;
  /// The token to represent the return address and the returned values.
  AllocaInst *RetAddr, *RetVal;
  /// Map the argument of the inlined function to the PHINode.s
  SmallVector<AllocaInst*, 8> Args;
  /// Remember the number of CallSite expanded.
  unsigned NumCallSite;

  ExpandedFunctionInfo()
    : Entry(0), Exit(0), RetAddr(0), RetVal(0), NumCallSite(0) {}

  ExpandedFunctionInfo(const ExpandedFunctionInfo&) LLVM_DELETED_FUNCTION;
  void operator=(ExpandedFunctionInfo&) LLVM_DELETED_FUNCTION;
};

struct GotoExpansion : public ModulePass {
  static char ID;
  DenseMap<Function*, ExpandedFunctionInfo*> ExpandedFunctions;
  LLVMContext *Context;
  IntegerType *RetAddrTy;

  GotoExpansion() : ModulePass(ID), Context(0), RetAddrTy(0) {
    initializeGotoExpansionPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M);
  void releaseMemory() {
    RetAddrTy = 0;
  }

  bool inlineAllCallSites(Function *F);
  void inlineCallSite(CallInst *CI, Function *Caller);
  /// Create the entry block and exit block for the inline candidate.
  ExpandedFunctionInfo *cloneCallee(Function *Caller, Function *Callee);

  void reloadForEscapedUses(BasicBlock *CallBB, BasicBlock *AfterCallBB);
};
}

char GotoExpansion::ID = 0;
INITIALIZE_PASS_BEGIN(GotoExpansion, "shang-goto-expansion",
                "Goto Expansion in HLS", false, false)

INITIALIZE_PASS_END(GotoExpansion, "shang-goto-expansion",
                "Goto Expansion in HLS", false, false)

Pass *vast::createGotoExpansionPass() {
  return new GotoExpansion();
}

bool GotoExpansion::runOnModule(Module &M) {
  bool changed = false;
  Context = &M.getContext();

  std::vector<Function*> Worklist;

  // Find the top level function which is not called by any other functions,
  // we will inline all other functions into it.
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
    Function *F = I;

    if (!F->use_empty() || F->isDeclaration())  continue;

    // Ignore the dead functions.
    if (!F->hasExternalLinkage()) continue;

    Worklist.push_back(F);
  }

  assert(Worklist.size() == 1 && "Cannot handle multiple top-level functions!");
  Function *F = Worklist.back();

  while (inlineAllCallSites(F))
    changed = true;

  // Release the memory and erase the dead functions.
  DeleteContainerSeconds(ExpandedFunctions);
  ExpandedFunctions.clear();

  DEBUG(dbgs() << "After goto expansion:\n"; F->dump(););

  return changed;
}

bool GotoExpansion::inlineAllCallSites(Function *F) {
  std::vector<CallInst*> Worklist;

  // Collect all call site and inline them.
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I)
    if (CallInst *CI = dyn_cast<CallInst>(&*I)) {
      // Do not try to inline the declaration.
      if (CI->getCalledFunction()->isDeclaration()) continue;

      Worklist.push_back(CI);
    }

  if (Worklist.empty()) return false;

  while (!Worklist.empty()) {
    CallInst *CI = Worklist.back();
    Worklist.pop_back();

    // If this call site is dead and it is to a readonly function, we should
    // just delete the call instead of trying to inline it, regardless of
    // size.  This happens because IPSCCP propagates the result out of the
    // call and then we're left with the dead call.
    if (isInstructionTriviallyDead(CI)) {
      DEBUG(dbgs() << "    -> Deleting dead call: " << *CI << "\n");

      CI->eraseFromParent();
      ++NumCallsDeleted;
    }

    inlineCallSite(CI, F);
  }

  return true;
}

void GotoExpansion::inlineCallSite(CallInst *CI, Function *Caller) {
  Function *Callee = CI->getCalledFunction();
  ExpandedFunctionInfo *&Info = ExpandedFunctions[Callee];

  // Clone the callee if we had not do so yet.
  if (Info == 0) Info = cloneCallee(Caller, Callee);

  BasicBlock *CallingBB = CI->getParent();

  BasicBlock *AfterCallBB
    = CallingBB->splitBasicBlock(CI, Callee->getName() +".ret");

  // Change the branch that used to go to AfterCallBB to branch to the first
  // basic block of the inlined function.
  //
  TerminatorInst *Br = CallingBB->getTerminator();
  assert(Br && Br->getOpcode() == Instruction::Br &&
    "splitBasicBlock broken!");
  Br->setOperand(0, Info->Entry);

  // Setup the incoming arguments.
  CallSite CS(CI);
  for (unsigned i = 0, e = CS.arg_size(); i != e; ++i)
    new StoreInst(CS.getArgument(i), Info->Args[i], CallingBB->getTerminator());

  // Also setup the return address value.
  ConstantInt *CurRetAddr = ConstantInt::get(RetAddrTy, Info->NumCallSite);
  new StoreInst(CurRetAddr, Info->RetAddr, CallingBB->getTerminator());
  // Return to the corresponding block according to the CurAddr.
  SwitchInst *SW = cast<SwitchInst>(Info->Exit->getTerminator());
  SW->addCase(CurRetAddr, AfterCallBB);

  // Replace returned value.
  if (Value *V = Info->RetVal) {
    Twine RetName = CI->getName() + ".gerv";
    Value *NewRetVal = new LoadInst(V, RetName, CI);
    CI->replaceAllUsesWith(NewRetVal);
  }
  // And the remove the call site since we had already inlined the call.
  CI->eraseFromParent();

  ++Info->NumCallSite;

  // Avoid the virtual register live across the CallInst, otherwise we may break
  // the SSA form after we change the CFG.
  reloadForEscapedUses(CallingBB, AfterCallBB);
}

void
GotoExpansion::reloadForEscapedUses(BasicBlock *CallBB, BasicBlock *AfterCallBB) {
  typedef BasicBlock::iterator iterator;
  typedef Value::use_iterator use_iterator;
  iterator EntryInsertPoint = CallBB->getParent()->getEntryBlock().begin();
  SmallVector<Instruction*, 4> Worklist;

  for (iterator I = CallBB->begin(), E = CallBB->end(); I != E; ++I) {
    // Ignore the Allocas.
    if (isa<AllocaInst>(I)) continue;

    bool AnyEscapedUse = false;
    for (use_iterator UI = I->use_begin(), UE = I->use_end(); UI != UE; ++UI) {
      Instruction *UseInst = dyn_cast<Instruction>(*UI);
      if (UseInst == 0 || UseInst->getParent() == CallBB) continue;

      // Cross BB use detected, demote the value to stack again.
      assert(UseInst->getParent() == AfterCallBB && "reg2mem pass not run?");
      AnyEscapedUse = true;
    }

    if (AnyEscapedUse) Worklist.push_back(I);
  }

  while (!Worklist.empty()) {
    Instruction *Inst = Worklist.pop_back_val();
    ++NumCrossBBValue;

    AllocaInst *AI = DemoteRegToStack(*Inst, false, EntryInsertPoint);
    if (InitStacks)
      new StoreInst(Constant::getNullValue(Inst->getType()), AI, EntryInsertPoint);
  }
}

ExpandedFunctionInfo *
GotoExpansion::cloneCallee(Function *Caller, Function *Callee) {
  ExpandedFunctionInfo *Info = new ExpandedFunctionInfo();
  StringRef CalleeName = Callee->getName();
  BasicBlock *CallerEntry = &Caller->getEntryBlock();
  typedef BasicBlock::iterator iterator;
  iterator EntryInsertPoint = CallerEntry->begin();

  // Construct the entry and exit blocks.
  BasicBlock *Entry = BasicBlock::Create(*Context, CalleeName + ".entry", Caller);
  BasicBlock *Exit = BasicBlock::Create(*Context, CalleeName + ".exit", Caller);
  Info->Entry = Entry;
  Info->Exit = Exit;

  // Create the RetAddrTy lazily.
  if (RetAddrTy == 0) RetAddrTy= IntegerType::get(*Context, 8);

  // Build the return address.
  Info->RetAddr
    = new AllocaInst(RetAddrTy, CalleeName + ".gera", EntryInsertPoint);
  if (InitStacks)
    new StoreInst(Constant::getNullValue(RetAddrTy), Info->RetAddr,
                  EntryInsertPoint);
  // Create the default unreachable block for the exit. So we can get the warning
  // when the something wrong with the retaddr and we branch to the unreachable
  // BB.
  BasicBlock *UnreachableBB
    = BasicBlock::Create(*Context, CalleeName + ".unreachable", Caller);
  new UnreachableInst(*Context, UnreachableBB);

  Twine SwitchValName = CalleeName + ".gerw";
  Value *SwitchVal = new LoadInst(Info->RetAddr, SwitchValName, Exit);
  SwitchInst::Create(SwitchVal, UnreachableBB, 0, Exit);

  // Create the value for the arguments.
  // Duplicate the argument map, because the VMap will be changed by the clone
  // function.
  ValueToValueMapTy CloneVMap;
  SmallVector<LoadInst*, 8> ArgLoads;
  typedef Function::arg_iterator arg_iterator;
  for (arg_iterator I = Callee->arg_begin(), E = Callee->arg_end(); I != E; ++I){
    Argument *Arg = I;

    Twine ArgName = CalleeName + "." + Arg->getName() + ".geia";
    AllocaInst *ArgAlloca
      = new AllocaInst(Arg->getType(), ArgName, EntryInsertPoint);
    if (InitStacks)
      new StoreInst(Constant::getNullValue(Arg->getType()), ArgAlloca,
                    EntryInsertPoint);
    Info->Args.push_back(ArgAlloca);
    Twine ArgLoadName = ArgName + ".geal";
    LoadInst *L = new LoadInst(ArgAlloca, ArgLoadName, Entry);
    CloneVMap[Arg] = L;
    ArgLoads.push_back(L);
  }

  AttributeSet OldSet = Caller->getAttributes();

  // Clone the caller into the callee.
  // TODO: Fix the line numbers.
  SmallVector<ReturnInst*, 16> Rets;
  CloneFunctionInto(Caller, Callee, CloneVMap, true, Rets, ".ge");

  // There is a bug in llvm CloneFunctionInto which will break the attribute set.
  Caller->setAttributes(OldSet);

  BasicBlock *CalleeEntry = cast<BasicBlock>(CloneVMap[&Callee->getEntryBlock()]);
  // Connect to the newly cloned function body.
  BranchInst::Create(CalleeEntry, Entry);

  // Forward the return value of the cloned function if there is any.
  Type *RetTy = Callee->getReturnType();
  AllocaInst *RetVal = 0;
  if (!RetTy->isVoidTy()) {
    Info->RetVal
      = (RetVal = new AllocaInst(RetTy, CalleeName + ".gerv",
                                 EntryInsertPoint));
    if (InitStacks)
      new StoreInst(Constant::getNullValue(RetTy), RetVal, EntryInsertPoint);
  }

  while (!Rets.empty()) {
    ReturnInst *Ret = Rets.pop_back_val();
    BasicBlock *RetBB = Ret->getParent();
    if (RetVal)
      new StoreInst(Ret->getReturnValue(), RetVal, RetBB->getTerminator());
    // Replace the ret instruction by the branch to the exit bb.
    Ret->eraseFromParent();
    BranchInst::Create(Exit, RetBB);
  }

  // Copy from InlineFunction.
  // StaticAllocas - InlineFunction fills this in with all static allocas that
  // get copied into the caller.
  SmallVector<AllocaInst*, 4> StaticAllocas;
  // If there are any alloca instructions in the block that used to be the entry
  // block for the callee, move them to the entry block of the caller.  First
  // calculate which instruction they should be inserted before.  We insert the
  // instructions at the end of the current alloca list.
  for (iterator I = CalleeEntry->begin(), E = CalleeEntry->end(); I != E; ) {
    AllocaInst *AI = dyn_cast<AllocaInst>(I++);
    if (AI == 0) continue;

    if (!isa<Constant>(AI->getArraySize()))
      continue;

    // Keep track of the static allocas that we inline into the caller.
    StaticAllocas.push_back(AI);

    // Scan for the block of allocas that we can move over, and move them
    // all at once.
    while (isa<AllocaInst>(I) &&
           isa<Constant>(cast<AllocaInst>(I)->getArraySize())) {
      StaticAllocas.push_back(cast<AllocaInst>(I));
      ++I;
    }

    // Transfer all of the allocas over in a block.  Using splice means
    // that the instructions aren't removed from the symbol table, then
    // reinserted.
    CallerEntry->getInstList().splice(EntryInsertPoint, CalleeEntry->getInstList(),
                                      AI, I);
  }

  IRBuilder<> builder(Entry->begin());
  while (!StaticAllocas.empty()) {
    AllocaInst *AI = StaticAllocas.pop_back_val();
    // If the alloca is now dead, remove it.  This often occurs due to code
    // specialization.
    if (AI->use_empty()) {
      AI->eraseFromParent();
      continue;
    }
    // TODO: Add lifetime marker.
  }

  // Also load that replaced the function argument to memory.
  while (!ArgLoads.empty()) {
    LoadInst *L = ArgLoads.pop_back_val();
    AllocaInst *Addr = cast<AllocaInst>(L->getPointerOperand());

    while (!L->use_empty()) {
      Instruction *U = cast<Instruction>(L->use_back());
      assert(!isa<PHINode>(U) && "reg2mem not run before goto-expansion!");
      // If this is a normal instruction, just insert a load.
      Value *V = new LoadInst(Addr, L->getName(), false, U);
      U->replaceUsesOfWith(L, V);
    }
  }

  ++NumFnExapnded;
  return Info;
}
