//===----- Scripting.cpp - Scripting engine for verilog backend --*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the LuaScript class, which allow users pass some
// information into the program with lua script. 
//
//===----------------------------------------------------------------------===//
#include "LuaScript.h"

#include "vast/Passes.h"
#include "vast/VASTNodeBases.h"
#include "vast/Utilities.h"

#include "llvm/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/ADT/STLExtras.h"

// Include the lua headers (the extern "C" is a requirement because we're
// using C++ and lua has been compiled as C code)
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#include "LuaBridge/LuaBridge.h"

using namespace llvm;
using namespace luabridge;

// The text template processing lua module
#include "luapp.inc"

LuaScript::LuaScript() : State(lua_open()) {
  FUSet.grow(VFUs::LastCommonFUType);
}

LuaScript::~LuaScript() {
  // FIXME: Release the function unit descriptors and function settings.
  //for (size_t i = 0, e = array_lengthof(FUSet); i != e; ++i)
  //  if(VFUDesc *Desc = FUSet[i]) delete Desc;

  lua_close(State);
}

void LuaScript::init() {
  // Open lua libraries.
  luaL_openlibs(State);

  load_luapp_lo(State);

  // Bind the object.
  setGlobal(State, newTable(State), "TimingAnalysis");
  // Cost/latency of functional units
  setGlobal(State, newTable(State), "FUs");
  // Functions to be mapped to hardware
  setGlobal(State, newTable(State), "Functions");
  // Predefined modules
  setGlobal(State, newTable(State), "Modules");
  // Synthesis attribute
  setGlobal(State, newTable(State), "SynAttr");
  // Table for Miscellaneous information
  setGlobal(State, newTable(State), "Misc");
}

LuaRef LuaScript::getValue(ArrayRef<const char*> Path) const {
  LuaRef o = getGlobal(State, Path[0]);
  for (unsigned i = 1; i < Path.size(); ++i)
    o = o[Path[i]];

  return o;
}

LuaRef LuaScript::getValue(const char *Name) const {
  const char *Path[] = { Name };
  return getValue(Path);
}

std::string LuaScript::getValueStr(const char *Name) const {
  const char *Path[] = { Name };
  return getValue(Path).cast<std::string>();
}

bool LuaScript::runScriptStr(const std::string &ScriptStr, SMDiagnostic &Err) {
  // Run the script.
  if (luaL_dostring(State, ScriptStr.c_str())) {
    Err = SMDiagnostic(ScriptStr, SourceMgr::DK_Warning, lua_tostring(State, -1));
    return false;
  }

  return true;
}

bool LuaScript::runScriptFile(const std::string &ScriptPath, SMDiagnostic &Err) {
  // Run the script.
  if (luaL_dofile(State, ScriptPath.c_str())) {
    Err = SMDiagnostic(ScriptPath, SourceMgr::DK_Warning, lua_tostring(State, -1));
    return false;
  }

  return true;
}

template<enum VFUs::FUTypes T>
void LuaScript::initSimpleFU(LuaRef FUs) {
  FUSet[T] = new VSimpleFUDesc<T>(FUs[VFUDesc::getTypeName(T)]);
}

void LuaScript::updateFUs() {
  LuaRef FUs = getGlobal(State, "FUs");
  // Initialize the functional unit descriptions.
  FUSet[VFUs::MemoryBus]
    = new VFUMemBus(FUs[VFUDesc::getTypeName(VFUs::MemoryBus)]);

  initSimpleFU<VFUs::AddSub>(FUs);
  initSimpleFU<VFUs::Shift>(FUs);
  initSimpleFU<VFUs::Mult>(FUs);
  initSimpleFU<VFUs::ICmp>(FUs);

  FUSet[VFUs::Mux]
    = new VFUMux(FUs[VFUDesc::getTypeName(VFUs::Mux)]);
  
  // Read other parameters.
#define READPARAMETER(PARAMETER, T) \
  VFUs::PARAMETER = FUs[#PARAMETER].cast<T>();

  READPARAMETER(LUTCost, unsigned);
  READPARAMETER(RegCost, unsigned);

  READPARAMETER(Period, double);
  READPARAMETER(LUTDelay, float);
  READPARAMETER(MaxLutSize, unsigned);
}

void LuaScript::updateStatus() {
  updateFUs();

  // Read the synthesis attributes.
  const char *Path[] = { "SynAttr", "DirectClkEnAttr" };
  VASTNode::DirectClkEnAttr = getValue(Path).cast<std::string>();
  Path[1] = "ParallelCaseAttr";
  VASTNode::ParallelCaseAttr = getValue(Path).cast<std::string>();
  Path[1] = "FullCaseAttr";
  VASTNode::FullCaseAttr = getValue(Path).cast<std::string>();

  // Build the data layout.
  raw_string_ostream s(DataLayout);

  // FIXME: Set the correct endian.
  s << 'e';

  s << '-';

  // Setup the address width (pointer width).
  unsigned PtrSize = getFUDesc<VFUMemBus>()->getAddrWidth();
  s << "p:" << PtrSize << ':' << PtrSize << ':' << PtrSize << '-';

  // FIXME: Setup the correct integer layout.
  s << "i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-";
  s << "n8:16:32:64";

  s.flush();
}

ManagedStatic<LuaScript> Script;

VFUDesc *llvm::getFUDesc(enum VFUs::FUTypes T) {
  return Script->FUSet[T];
}

LuaScript &llvm::scriptEngin() { return *Script; }

unsigned llvm::getIntValueFromEngine(ArrayRef<const char*> Path) {
  return Script->getValue(Path).cast<unsigned>();
}

float llvm::getFloatValueFromEngine(ArrayRef<const char*> Path) {
  return Script->getValue(Path).cast<float>();
}

std::string llvm::getStrValueFromEngine(ArrayRef<const char*> Path) {
  return Script->getValue(Path).cast<std::string>();
}

std::string llvm::getStrValueFromEngine(const char *VariableName) {
  const char *Path[] = { VariableName };
  return getStrValueFromEngine(Path);
}

bool llvm::runScriptFile(const std::string &ScriptPath, SMDiagnostic &Err) {
  return Script->runScriptFile(ScriptPath, Err);
}

bool llvm::runScriptStr(const std::string &ScriptStr, SMDiagnostic &Err) {
  return Script->runScriptStr(ScriptStr, Err);
}

namespace llvm {
std::string getDataLayoutFromEngine() {
  return Script->getDataLayout();
}

bool loadConfig(const std::string &Path) {
  Script->init();

  SMDiagnostic Err;
  if (!Script->runScriptFile(Path, Err)){
    report_fatal_error(Err.getMessage());
    return true;
  }

  Script->updateStatus();

  return false;
}
}
