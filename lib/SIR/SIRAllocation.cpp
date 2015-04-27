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
#include "sir/Passes.h"

#include "vast/LuaI.h"

#include "llvm/ADT/SmallPtrSet.h"

using namespace llvm;
using namespace vast;

SIRAllocation::SIRAllocation() : SM(0), TD(0), ModulePass(ID) {
	initializeSIRAllocationPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(SIRAllocation,
	                    "sir-allocation", "SIR Allocation",
	                    false, true, false)
	INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
	INITIALIZE_PASS_DEPENDENCY(DataLayout)
INITIALIZE_PASS_END(SIRAllocation,
	                  "sir-allocation", "SIR Allocation",
	                   false, true, false)

char SIRAllocation::ID = 0;

Pass *llvm::createSIRAllocationPass() {
	return new SIRAllocation();
}

void SIRAllocation::getAnalysisUsage(AnalysisUsage &AU) const {
	AU.addRequired<AliasAnalysis>();		
	AU.addRequired<DataLayout>();
	AU.setPreservesAll();
}

bool SIRAllocation::runOnModule(Module &M) {
	AliasSetTracker AST(getAnalysis<AliasAnalysis>());
	TD = &(getAnalysis<DataLayout>());
	
	// Create the SIR from the Functions in module.
	typedef Module::iterator iterator;
	for (iterator I = M.begin(), E = M.end(); I != E; ++I) {
		Function *F = I;

		// Do not creat SIR for useless Function.
		if (F->isDeclaration() || !F->use_empty())
			continue;

		// Create the SIR.
		assert(SM == NULL && "SIR module already exist!");
		SM = new SIR(F);
	}

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

bool SIRAllocation::createSIRMemoryBank(AliasSet *AS, unsigned BankNum) {
	SIRCtrlRgnBuilder *SCRB = new SIRCtrlRgnBuilder(SM, *TD);	
	unsigned ReadLatency = LuaI::Get<VFUMemBus>()->getReadLatency();

	SmallVector<Value *, 8> Pointers;
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

		// Update the max size of the accessed type.
		MaxElementSizeInBytes = std::max(MaxElementSizeInBytes, ElementSizeInBytes);
	}

	unsigned AddrWidth = Log2_32_Ceil(BankSizeInBytes);

	// Create the memory bus.
	SIRMemoryBank *SMB = SCRB->createMemoryBank(BankNum, AddrWidth,
		                                          MaxElementSizeInBytes * 8, ReadLatency);

	// Remember the binding and add the global variable to the memory bank.
	while (!Objects.empty()) {
		std::pair<GlobalVariable*, unsigned> Obj = Objects.pop_back_val();
		GlobalVariable *GV = Obj.first;
		DEBUG(dbgs() << "Assign " << *GV << " to Memory #" << BankNum << "\n");

		SMB->addGlobalVariable(GV, Obj.second);
		bool inserted = Binding.insert(std::make_pair(GV, SMB)).second;
		assert(inserted && "Allocation not inserted!");

		// Get the OriginalPtrSize which will be used in GetElementPtrInst to
		// calculate the offset address for each GV inserted into this SMB.
		unsigned OriginalPtrSize = TD->getTypeStoreSizeInBits(GV->getType());
		SMB->indexGV2OriginalPtrSize(GV, OriginalPtrSize);
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

void SIRAllocation::runOnFunction(Function &F, AliasSetTracker &AST) {
	for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
		Instruction *Inst = &*I;

		if (!(isa<LoadInst>(Inst) || isa<StoreInst>(Inst))) continue;

		AST.add(Inst);
	}
}