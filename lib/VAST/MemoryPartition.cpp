//===-------------- MemoryPartition.cpp - MemoryPartition ---------*-C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The MemoryPartition try to allocate the memory ports.
//
//===----------------------------------------------------------------------===//

#include "Allocation.h"

#include "shang/Utilities.h"
#include "shang/Passes.h"
#include "shang/VASTMemoryPort.h"
#include "shang/VASTModule.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/ADT/ValueMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/Statistic.h"
#define DEBUG_TYPE "shang-memory-partition"
#include "llvm/Support/Debug.h"

using namespace llvm;
STATISTIC(NumMemoryAccess, "Number of memory accesses");
STATISTIC(NumLoad, "Number of Load");
STATISTIC(NumStore, "Number of Store");
STATISTIC(NumMemBanks, "Number of Local Memory Bank Allocated");
STATISTIC(NumBRam2Reg, "Number of Single Element Block RAM Lowered to Register");
STATISTIC(NUMCombROM, "Number of Combinational ROM Generated");

static cl::opt<bool>
EnalbeDualPortRAM("shang-enable-dual-port-ram",
  cl::desc("Enable dual port ram in the design."),
  cl::init(true));

static cl::opt<unsigned>
MaxComblROMLL("vast-max-combinational-rom-logic-level",
  cl::desc("The maximal allowed logic level of a combinational ROM"),
  cl::init(1));

namespace {
struct MemoryPartition : public ModulePass, public HLSAllocation {
  static char ID;

  ValueMap<const Value*, VASTMemoryBus*>  Binding;

  // Look up the memory port allocation if the pointers are not allocated
  // to the BlockRAM.
  virtual VASTMemoryBus *getMemoryBank(const LoadInst &I) const {
    return Binding.lookup(I.getPointerOperand());
  }

  virtual VASTMemoryBus *getMemoryBank(const StoreInst &I) const {
    return Binding.lookup(I.getPointerOperand());
  }

  virtual VASTMemoryBus *getMemoryBank(const GlobalVariable &GV) const {
    return Binding.lookup(&GV);
  }

  MemoryPartition() : ModulePass(ID) {
    initializeMemoryPartitionPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    HLSAllocation::getAnalysisUsage(AU);
    AU.addRequired<AliasAnalysis>();
    AU.setPreservesAll();
  }

  bool createMemoryBank(AliasSet *AS, unsigned PortNum);

  bool runOnModule(Module &M);
  void runOnFunction(Function &F, AliasSetTracker &AST);

  void releaseMemory() {
    Binding.clear();
  }

  /// getAdjustedAnalysisPointer - This method is used when a pass implements
  /// an analysis interface through multiple inheritance.  If needed, it
  /// should override this to adjust the this pointer as needed for the
  /// specified pass info.
  virtual void *getAdjustedAnalysisPointer(const void *ID) {
    if (ID == &HLSAllocation::ID)
      return (HLSAllocation*)this;
    return this;
  }
};
}
//===----------------------------------------------------------------------===//

INITIALIZE_AG_PASS_BEGIN(MemoryPartition, HLSAllocation,
                         "memory-partition", "Memory Partition",
                         false, true, false)
  INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
INITIALIZE_AG_PASS_END(MemoryPartition, HLSAllocation,
                       "memory-partition", "Memory Partition",
                       false, true, false)

char MemoryPartition::ID = 0;

Pass *llvm::createMemoryPartitionPass() {
  return new MemoryPartition();
}

bool MemoryPartition::runOnModule(Module &M) {
  InitializeHLSAllocation(this);

  AliasSetTracker AST(getAnalysis<AliasAnalysis>());

  typedef Module::global_iterator global_iterator;
  for (global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I) {
    GlobalVariable *GV = I;

    // DIRTYHACK: Make sure the GV is aligned.
    GV->setAlignment(std::max(8u, GV->getAlignment()));

    AST.add(GV, AliasAnalysis::UnknownSize, 0);
  }

  typedef Module::iterator iterator;
  for (iterator I = M.begin(), E = M.end(); I != E; ++I)
    runOnFunction(*I, AST);

  unsigned CurPortNum = 1;

  for (AliasSetTracker::iterator I = AST.begin(), E = AST.end(); I != E; ++I) {
    AliasSet *AS = I;

    // Ignore the set that does not contain any load/store.
    if (AS->isForwardingAliasSet() || !(AS->isMod() || AS->isRef()))
      continue;

    if (createMemoryBank(AS, CurPortNum))
      ++CurPortNum;
  }

  NumMemBanks += (CurPortNum - 1);

  return false;
}

bool MemoryPartition::createMemoryBank(AliasSet *AS, unsigned PortNum) {
  VASTModule &VM = getModule();
  uint64_t MemBusSizeInBytes = getFUDesc<VFUMemBus>()->getDataWidth() / 8;
  bool AllocateNewPort = true;
  SmallVector<Value*, 8> Pointers;
  SmallPtrSet<Type*, 8> AccessedTypes;
  SmallVector<std::pair<GlobalVariable*, unsigned>, 8> Objects;
  unsigned BankSizeInBytes = 0, MaxElementSizeInBytes = 0;

  for (AliasSet::iterator AI = AS->begin(), AE = AS->end(); AI != AE; ++AI) {
    Value *V = AI.getPointer();
    Type *ElemTy = cast<PointerType>(V->getType())->getElementType();
    unsigned ElementSizeInBytes = TD->getTypeStoreSize(ElemTy);

    if (GlobalVariable *GV = dyn_cast<GlobalVariable>(V)) {
      // Do not allocate local memory port if the pointers alias with external
      // global variables.
      AllocateNewPort &= GV->hasInternalLinkage() || GV->hasPrivateLinkage();

      // Calculate the size of the object.
      unsigned NumElem = 1;

      // Try to expand multi-dimension array to single dimension array.
      while (const ArrayType *AT = dyn_cast<ArrayType>(ElemTy)) {
        ElemTy = AT->getElementType();
        NumElem *= AT->getNumElements();
      }

      // GV may be a struct. In this case, we may not load/store the whole
      // struct in a single instruction. This mean the required data port size
      // is not necessary as big as the element size here.
      ElementSizeInBytes = std::min(TD->getTypeStoreSize(ElemTy),
                                    MemBusSizeInBytes);
      unsigned CurArraySize = NumElem * ElementSizeInBytes;

      // Accumulate the element size.
      BankSizeInBytes += CurArraySize;
      Objects.push_back(std::make_pair(GV, CurArraySize));
    } else
      Pointers.push_back(V);

    AccessedTypes.insert(ElemTy);
    // Update the max size of the accessed type.
    MaxElementSizeInBytes = std::max(MaxElementSizeInBytes,
                                      ElementSizeInBytes);
  }

  assert(AllocateNewPort && "Allocating default bus is not implemented yet!");
  assert(MaxElementSizeInBytes <= MemBusSizeInBytes
          && "Unexpected element size!");
  // The memory bank is read only if all load/store instructions do not modify
  // the accessed location.
  bool IsReadOnly = !AS->isMod();
  unsigned ByteEnWidth = Log2_32_Ceil(MaxElementSizeInBytes);
  unsigned AddrWidth = Log2_32_Ceil(BankSizeInBytes);
  // Dirty Hack: Make the word address part not empty.
  AddrWidth = std::max<unsigned>(ByteEnWidth + 1, AddrWidth);
  bool RequireByteEnable = AccessedTypes.size() != 1;
  // The byte address inside a word of the memory bank is not used
  unsigned EffAddrWidth = AddrWidth - Log2_32_Ceil(MaxElementSizeInBytes);
  bool IsCombROM = IsReadOnly &&
                    EffAddrWidth <= pow(float(VFUs::MaxLutSize),
                                        int(MaxComblROMLL));
  // Now create the memory bus.
  VASTMemoryBus *Bus = VM.createMemBus(PortNum, AddrWidth,
                                       MaxElementSizeInBytes * 8,
                                       RequireByteEnable, EnalbeDualPortRAM,
                                       IsCombROM);

  // Remember the global variable binding, and add the global variable to
  // the memory bank.
  while (!Objects.empty()) {
    std::pair<GlobalVariable*, unsigned> Obj = Objects.pop_back_val();
    GlobalVariable *GV = Obj.first;
    DEBUG(dbgs() << "Assign " << *GV << " to Memory #" << PortNum << "\n");

    Bus->addGlobalVariable(GV, Obj.second);
    bool inserted = Binding.insert(std::make_pair(GV, Bus)).second;
    assert(inserted && "Allocation not inserted!");
  }


  // Remember the pointer operand binding
  while (!Pointers.empty()) {
    Value *Ptr = Pointers.pop_back_val();

    bool inserted = Binding.insert(std::make_pair(Ptr, Bus)).second;
    assert(inserted && "Allocation not inserted!");
    (void) inserted;
  }

  return AllocateNewPort;
}

void MemoryPartition::runOnFunction(Function &F, AliasSetTracker &AST) {
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    Instruction *Inst = &*I;

    // TODO: Handle call instructions.
    if (!isLoadStore(Inst)) continue;

    ++NumMemoryAccess;

    if (Inst->mayWriteToMemory()) ++NumStore;
    else                          ++NumLoad;

    AST.add(Inst);
  }
}
