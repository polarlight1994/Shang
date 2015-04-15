//===-- SIRAllocation.cpp - High-level Synthesis Resource Allocation in SIR --*- C++ -*-===//
//
//                                The SIR HLS framework                                    //
//
// This file is distributed under the University of Illinois Open Source License.
// See LICENSE.TXT for details.
//
//===-----------------------------------------------------------------------------------===//
//
// This file implement the resource allocation interface in the SIR HLS framework.
//
//===-----------------------------------------------------------------------------------===//

#include "sir/SIRBuild.h"
#include "sir/SIRAllocation.h"

#include "vast/LuaI.h"

#include "llvm/ADT/SmallPtrSet.h"

using namespace llvm;
using namespace vast;

namespace llvm {
struct SIRMemoryPartition : public ModulePass, public SIRAllocation {
	static char ID;

	// The map between value and memory bank
	ValueMap<const Value *, SIRMemoryBank *> Binding;

	// Look up the memory port allocation.
	virtual SIRMemoryBank *getMemoryBank(const LoadInst &I) const {
		return Binding.lookup(I.getPointerOperand());
	}

	virtual SIRMemoryBank *getMemoryBank(const StoreInst &I) const {
		return Binding.lookup(I.getPointerOperand());
	}

	virtual SIRMemoryBank *getMemoryBank(const GlobalVariable &GV) const {
		return Binding.lookup(&GV);
	}

	SIRMemoryPartition() : ModulePass(ID) {
		initializeSIRMemoryPartitionPass(*PassRegistry::getPassRegistry());
	}

	void getAnalysisUsage(AnalysisUsage &AU) const {
		SIRAllocation::getAnalysisUsage(AU);
		AU.addRequired<AliasAnalysis>();
		AU.addRequired<DataLayout>();
		AU.addRequired<SIRInit>();
		AU.setPreservesAll();
	}

	bool createSIRMemoryBank(AliasSet *AS, unsigned BankNum);

	bool runOnModule(Module &M);
	void runOnFunction(Function &F, AliasSetTracker &AST);

	/// getAdjustedAnalysisPointer - This method is used when a pass implements
	/// an analysis interface through multiple inheritance.  If needed, it
	/// should override this to adjust the this pointer as needed for the
	/// specified pass info.
	virtual void *getAdjustedAnalysisPointer(const void *ID) {
		if (ID == &SIRAllocation::ID)
			return (SIRAllocation*)this;
		return this;
	}
};
}

INITIALIZE_AG_PASS_BEGIN(SIRMemoryPartition, SIRAllocation,
	                       "sir-memory-partition", "SIR Memory Partition",
	                       false, true, false)
	INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
	INITIALIZE_PASS_DEPENDENCY(DataLayout)
	INITIALIZE_PASS_DEPENDENCY(SIRInit)
INITIALIZE_AG_PASS_END(SIRMemoryPartition, SIRAllocation,
	                     "sir-memory-partition", "SIR Memory Partition",
	                     false, true, false)

char SIRMemoryPartition::ID = 0;

Pass *llvm::createSIRMemoryPartitionPass() {
	return new SIRMemoryPartition();
}

bool SIRMemoryPartition::runOnModule(Module &M) {
	InitializeSIRAllocation(this);

	AliasSetTracker AST(getAnalysis<AliasAnalysis>());
	TD = &(getAnalysis<DataLayout>());
	SM = &(*getAnalysis<SIRInit>());	

	typedef Module::global_iterator global_iterator;
	for (global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I) {
		GlobalVariable *GV = I;

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

		if (createSIRMemoryBank(AS, CurPortNum))
			++CurPortNum;
	}

	return false;
}

bool SIRMemoryPartition::createSIRMemoryBank(AliasSet *AS, unsigned BankNum) {
	SIRCtrlRgnBuilder *SCRB = new SIRCtrlRgnBuilder(SM, *TD);	
	unsigned ReadLatency = LuaI::Get<VFUMemBus>()->getReadLatency();

	SmallVector<Value *, 8> Pointers;
	SmallPtrSet<Type *, 8> AccessedTypes;
	SmallVector<std::pair<GlobalVariable *, unsigned>, 8> Objects;

	unsigned BankSizeInBytes = 0, MaxElementSizeInBytes = 0;

	for (AliasSet::iterator AI = AS->begin(), AE = AS->end(); AI != AE; ++AI) {
		// Extract the load/store element from the instruction.
		Value *V = AI.getPointer();
		Type *ElemTy = cast<PointerType>(V->getType())->getElementType();
		unsigned ElementSizeInBytes = TD->getTypeStoreSize(ElemTy);
		
		if (GlobalVariable *GV = dyn_cast<GlobalVariable>(V)) {
			// Do not allocate local memory port if the pointers alias with external
			// global variables.
			assert(GV->hasInternalLinkage() || GV->hasPrivateLinkage()
				     && "Unexpected linkage GV!");

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
				                            uint64_t(ElementSizeInBytes));
			unsigned CurArraySize = NumElem * ElementSizeInBytes;

			// Accumulate the element size.
			BankSizeInBytes += CurArraySize;
			Objects.push_back(std::make_pair(GV, CurArraySize));
		} else
			Pointers.push_back(V);

		AccessedTypes.insert(ElemTy);
		// Update the max size of the accessed type.
		MaxElementSizeInBytes = std::max(MaxElementSizeInBytes, ElementSizeInBytes);
	}

	unsigned AddrWidth = Log2_32_Ceil(BankSizeInBytes);

	// Create the memory bus.
	SIRMemoryBank *SMB = SCRB->createMemoryBank(BankNum, AddrWidth,
		                                          MaxElementSizeInBytes, ReadLatency);

	// Remember the binding and add the global variable to the memory bank.
	while (!Objects.empty()) {
		std::pair<GlobalVariable*, unsigned> Obj = Objects.pop_back_val();
		GlobalVariable *GV = Obj.first;
		DEBUG(dbgs() << "Assign " << *GV << " to Memory #" << BankNum << "\n");

		SMB->addGlobalVariable(GV, Obj.second);
		bool inserted = Binding.insert(std::make_pair(GV, SMB)).second;
		assert(inserted && "Allocation not inserted!");
	}


	// Remember the pointer operand binding
	while (!Pointers.empty()) {
		Value *Ptr = Pointers.pop_back_val();

		bool inserted = Binding.insert(std::make_pair(Ptr, SMB)).second;
		assert(inserted && "Allocation not inserted!");
		(void) inserted;
	}

	return true;
}

void SIRMemoryPartition::runOnFunction(Function &F, AliasSetTracker &AST) {
	for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
		Instruction *Inst = &*I;

		if (!(isa<LoadInst>(Inst) || isa<StoreInst>(Inst))) continue;

		AST.add(Inst);
	}
}