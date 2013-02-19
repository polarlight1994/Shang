//===--------- GotoExpansion.cpp ---Perform Goto Expansion in HLS ---------===//
//
//                      The Shang HLS frameowrk                               //
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

#include "shang/DesignMetrics.h"
#include "shang/FUInfo.h"
#include "shang/Passes.h"

#include "llvm/Pass.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/raw_ostream.h"
#define DEBUG_TYPE "shang-inliner"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumFnExapnded, "Number of Functions inlined");
STATISTIC(NumCallsDeleted, "Number of call sites deleted, not inlined");

namespace {
struct ExpandedFunctionInfo {
  /// The expanded function will become a single-entry single-exit function.
  BasicBlock *Entry, *Exit;
  /// The token to represent the return address and the returned values.
  PHINode *RetAddr, *RetVal;
  /// Map the argument of the inlined function to the PHINode.s
  SmallVector<PHINode*, 8> Args;
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

  GotoExpansion() : ModulePass(ID), Context(0), RetAddrTy(0) {}

  bool runOnModule(Module &M);
  void releaseMemory() {
    DeleteContainerSeconds(ExpandedFunctions);
    RetAddrTy = 0;
  }

  bool inlineAllCallSites(Function *F);
  void inlineCallSite(CallInst *CI, Function *Caller);
  /// Create the entry block and exit block for the inline candidate.
  ExpandedFunctionInfo *cloneCallee(Function *Caller, Function *Callee);
};
}

char GotoExpansion::ID = 0;
INITIALIZE_PASS_BEGIN(GotoExpansion, "hls-goto-expansion",
                "Goto Expansion in HLS", false, false)

INITIALIZE_PASS_END(GotoExpansion, "hls-goto-expansion",
                "Goto Expansion in HLS", false, false)

Pass *llvm::createGotoExpansionPass() {
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
    Info->Args[i]->addIncoming(CS.getArgument(i), CallingBB);  

  // Also setup the return address value.
  ConstantInt *CurRetAddr = ConstantInt::get(RetAddrTy, Info->NumCallSite);
  Info->RetAddr->addIncoming(CurRetAddr, CallingBB);
  // Return to the corresponding block according to the CurAddr.
  SwitchInst *SW = cast<SwitchInst>(Info->Exit->getTerminator());
  SW->addCase(CurRetAddr, AfterCallBB);

  // Replace returned value.
  if (Value *V = Info->RetVal) CI->replaceAllUsesWith(V);

  // And the remove the call site since we had already inlined the call.
  CI->eraseFromParent();

  ++Info->NumCallSite;
}

ExpandedFunctionInfo *GotoExpansion::cloneCallee(Function *Caller, Function *Callee)\
{
  ExpandedFunctionInfo *Info = new ExpandedFunctionInfo();
  StringRef CalleeName = Callee->getName();

  // Construct the entry and exit blocks.
  BasicBlock *Entry = BasicBlock::Create(*Context, CalleeName + ".entry", Caller);
  BasicBlock *Exit = BasicBlock::Create(*Context, CalleeName + ".exit", Caller);
  Info->Entry = Entry;
  Info->Exit = Exit;

  // Create the RetAddrTy lazily.
  if (RetAddrTy == 0) RetAddrTy= IntegerType::get(*Context, 8);

  // Build the return address.
  Info->RetAddr = PHINode::Create(RetAddrTy, 0, CalleeName + ".retaddr", Entry);

  // Create the default unreachable block for the exit. So we can get the warning
  // when the something wrong with the retaddr and we branch to the unreachable
  // BB.
  BasicBlock *UnreachableBB
    = BasicBlock::Create(*Context, CalleeName + ".unreachable", Caller);
  new UnreachableInst(*Context, UnreachableBB);

  SwitchInst::Create(Info->RetAddr, UnreachableBB, 0, Exit);

  // Create the value for the arguments.
  // Duplicate the argument map, because the VMap will be changed by the clone
  // function.
  ValueToValueMapTy CloneVMap;
  typedef Function::arg_iterator arg_iterator;
  for (arg_iterator I = Callee->arg_begin(), E = Callee->arg_end(); I != E; ++I){
    Argument *Arg = I;

    Twine ArgName = CalleeName + "." + Arg->getName() + ".inline.arg";
    PHINode *ArgPHI = PHINode::Create(Arg->getType(), 0, ArgName, Entry);
    Info->Args.push_back(ArgPHI);
    CloneVMap[Arg] = ArgPHI;
  }

  // Clone the caller into the callee.
  // TODO: Fix the line numbers.
  SmallVector<ReturnInst*, 16> Rets;
  CloneFunctionInto(Caller, Callee, CloneVMap, true, Rets, ".goto_expansion");

  BasicBlock *CalleeEntry = cast<BasicBlock>(CloneVMap[&Callee->getEntryBlock()]);
  // Connect to the newly cloned function body.
  BranchInst::Create(CalleeEntry, Entry);

  // Forward the return value of the cloned function if there is any.
  Type *RetTy = Callee->getReturnType();
  PHINode *RetVal = 0;
  if (!RetTy->isVoidTy())
    Info->RetVal
      = (RetVal = PHINode::Create(RetTy, Rets.size(), CalleeName + ".retval",
                                  Exit->getTerminator()));

  while (!Rets.empty()) {
    ReturnInst *Ret = Rets.pop_back_val();
    BasicBlock *RetBB = Ret->getParent();
    if (RetVal) RetVal->addIncoming(Ret->getReturnValue(), RetBB);
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
  typedef BasicBlock::iterator iterator;
  BasicBlock *CallerEntry = &Caller->getEntryBlock();
  iterator InsertPoint = CallerEntry->begin();
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
    CallerEntry->getInstList().splice(InsertPoint, CalleeEntry->getInstList(),
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

  ++NumFnExapnded;
  return Info;
}
