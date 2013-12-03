//===- ScriptingSupport.cpp - Support running LUA during HLS ----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the ScriptingPass, a MachineFunctionPass that allow user
// manipulate some data such as current generated RTL module with external script
//
//===----------------------------------------------------------------------===//
#include "vast/Passes.h"
#include "vast/Utilities.h"
#include "vast/VASTModule.h"
#include "vast/VASTModulePass.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Pass.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/Format.h"
#define DEBUG_TYPE "vtm-scripting-support"
#include "llvm/Support/Debug.h"

using namespace llvm;

static void printConstant(raw_ostream &OS, uint64_t Val, Type* Ty,
                          DataLayout *TD) {
  OS << '\'';
  if (TD->getTypeSizeInBits(Ty) == 1)
    OS << (Val ? '1' : '0');
  else {
    std::string FormatS =
      "%0" + utostr_32(TD->getTypeStoreSize(Ty) * 2) + "llx";
    OS << "0x" << format(FormatS.c_str(), Val);
  }
  OS << '\'';
}


static void ExtractConstant(raw_ostream &OS, Constant *C, DataLayout *TD) {
  if (ConstantInt *CI = dyn_cast<ConstantInt>(C)) {
    printConstant(OS, CI->getZExtValue(), CI->getType(), TD);
    return;
  }

  if (isa<ConstantPointerNull>(C)) {
    printConstant(OS, 0, C->getType(), TD);
    return;
  }

  if (ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(C)) {
    ExtractConstant(OS, CDS->getElementAsConstant(0), TD);
    for (unsigned i = 1, e = CDS->getNumElements(); i != e; ++i) {
      OS << ", ";
      ExtractConstant(OS, CDS->getElementAsConstant(i), TD);
    }
    return;
  }

  if (ConstantArray *CA = dyn_cast<ConstantArray>(C)) {
    ExtractConstant(OS, cast<Constant>(CA->getOperand(0)), TD);
    for (unsigned i = 1, e = CA->getNumOperands(); i != e; ++i) {
      OS << ", ";
      ExtractConstant(OS, cast<Constant>(CA->getOperand(i)), TD);
    }
    return;
  }

  llvm_unreachable("Unsupported constant type to bind to script engine!");
  OS << '0';
}

bool llvm::runScriptOnGlobalVariables(Module &M, DataLayout *TD,
                                      const std::string &Script,
                                      SMDiagnostic Err) {
  SmallVector<GlobalVariable*, 32> GVs;
  for (Module::global_iterator I = M.global_begin(), E = M.global_end();
       I != E; ++I)
    GVs.push_back(I);

  return runScriptOnGlobalVariables(GVs, TD, Script, Err);
}

bool llvm::runScriptOnGlobalVariables(ArrayRef<GlobalVariable*> GVs,
                                      DataLayout *TD,
                                      const std::string &ScriptToRun,
                                      SMDiagnostic Err) {
  // Put the global variable information to the script engine.
  if (!runScriptStr("GlobalVariables = {}\n", Err))
    llvm_unreachable("Cannot create globalvariable table in scripting pass!");

  std::string Script;
  raw_string_ostream SS(Script);
  // Push the global variable information into the script engine.
  for (unsigned i = 0; i < GVs.size(); ++i){
    GlobalVariable *GV = GVs[i];

    // GlobalVariable information:
    // GVInfo {
    //   bool isLocal
    //   unsigned AddressSpace
    //   unsigned Alignment
    //   unsigned NumElems
    //   unsigned ElemSize
    //   table Initializer.
    //}

    SS << "GlobalVariables." << ShangMangle(GV->getName()) << " = { ";
    SS << "isLocal = " << GV->hasLocalLinkage() << ", ";
    SS << "AddressSpace = " << GV->getType()->getAddressSpace() << ", ";
    SS << "Alignment = " << GV->getAlignment() << ", ";
    Type *Ty = cast<PointerType>(GV->getType())->getElementType();
    // The element type of a scalar is the type of the scalar.
    Type *ElemTy = Ty;
    unsigned NumElem = 1;
    // Try to expand multi-dimension array to single dimension array.
    while (const ArrayType *AT = dyn_cast<ArrayType>(ElemTy)) {
      ElemTy = AT->getElementType();
      NumElem *= AT->getNumElements();
    }
    SS << "NumElems = " << NumElem << ", ";

    SS << "ElemSize = " << TD->getTypeStoreSizeInBits(ElemTy) << ", ";

    // The initialer table: Initializer = { c0, c1, c2, ... }
    SS << "Initializer = ";
    if (!GV->hasInitializer())
      SS << "nil";
    else {
      Constant *C = GV->getInitializer();

      SS << "{ ";
      if (C->isNullValue()) {
        Constant *Null = Constant::getNullValue(ElemTy);

        ExtractConstant(SS, Null, TD);
        for (unsigned i = 1; i < NumElem; ++i) {
          SS << ", ";
          ExtractConstant(SS, Null, TD);
        }
      } else
        ExtractConstant(SS, C, TD);

      SS << "}";
    }

    SS << '}';

    SS.flush();
    if (!runScriptStr(Script, Err)) {
      llvm_unreachable("Cannot create globalvariable infomation!");
    }
    Script.clear();
  }

  // Run the script against the GlobalVariables table.
  return runScriptStr(ScriptToRun, Err);
}

void llvm::bindFunctionToScriptEngine(DataLayout &TD, VASTModule *Module) {
  SMDiagnostic Err;
  Function &F = Module->getLLVMFunction();
  // Push the function information into the script engine.
  // FuncInfo {
  //   String Name,
  //   unsinged ReturnSize,
  //   ArgTable : { ArgName = Size, ArgName = Size ... }
  // }

  std::string Script;
  raw_string_ostream SS(Script);

  SS << "FuncInfo = { ";
  SS << "Name = '" << F.getName() << "', ";

  SS << "ReturnSize = ";
  if (F.getReturnType()->isVoidTy())
    SS << '0';
  else
    SS << TD.getTypeStoreSizeInBits(F.getReturnType());
  SS << ", ";

  SS << "Args = { ";

  if (F.arg_size()) {
    Function::const_arg_iterator I = F.arg_begin();
    SS << "{ Name = '" << I->getName() << "', Size = "
      << TD.getTypeStoreSizeInBits(I->getType()) << "}";
    ++I;

    for (Function::const_arg_iterator E = F.arg_end(); I != E; ++I)
      SS << " , { Name = '" << I->getName() << "', Size = "
      << TD.getTypeStoreSizeInBits(I->getType()) << "}";
  }

  SS << "} }";

  SS.flush();
  if (!runScriptStr(Script, Err))
    llvm_unreachable("Cannot create function infomation!");
  Script.clear();

  bindToScriptEngine("CurModule", Module);
}
