//===--- SIRAllocation.h - High-level Synthesis Resource Allocation in SIR ---*- C++ -*-===//
//
//                                The SIR HLS framework                                    //
//
// This file is distributed under the University of Illinois Open Source License.
// See LICENSE.TXT for details.
//
//===-----------------------------------------------------------------------------------===//
//
// This file define the resource allocation interface in the SIR HLS framework.
//
//===-----------------------------------------------------------------------------------===//

#ifndef SIR_ALLOCATION_H
#define SIR_ALLOCATION_H

#include "sir/SIR.h"
#include "sir/Passes.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"

namespace llvm {
class SIRAllocation {
protected:
	SIR *SM;
	const DataLayout *TD;

	SIRAllocation() : SM(0), TD(0) {}

	void InitializeSIRAllocation(Pass *P);

	/// getAnalysisUsage - All HLSAllocation implementations should invoke this
	/// directly (using HLSAllocation::getAnalysisUsage(AU)).
	virtual void getAnalysisUsage(AnalysisUsage &AU) const;
public:
	static char ID;

	virtual ~SIRAllocation();

	SIR &getModule() const { return *SM; }
	SIR *operator->() const { return SM; }

	// Memory Bank allocation queries.
	virtual SIRMemoryBank *getMemoryBank(const GlobalVariable &GV) const;
	virtual SIRMemoryBank *getMemoryBank(const LoadInst &I) const;
	virtual SIRMemoryBank *getMemoryBank(const StoreInst &I) const;
	SIRMemoryBank *getMemoryBank(const Value &V) const;
};
}

#endif