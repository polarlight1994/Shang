//===----- Scripting.cpp - Scripting engine for verilog backend --*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the LuaI class, which allow users pass some
// information into the program with lua script. 
//
//===----------------------------------------------------------------------===//
#include "vast/LuaI.h"
#include "vast/VASTNodeBases.h"

#include "llvm/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/ADT/STLExtras.h"
#define DEBUG_TYPE "vast-lua"
#include "llvm/Support/Debug.h"

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

LuaI::LuaI() : State(lua_open()) {
  FUSet.grow(VFUs::LastCommonFUType);
}

LuaI::~LuaI() {
  // FIXME: Release the function unit descriptors and function settings.
  //for (size_t i = 0, e = array_lengthof(FUSet); i != e; ++i)
  //  if(VFUDesc *Desc = FUSet[i]) delete Desc;

  lua_close(State);
}

void LuaI::init() {
  // Open lua libraries.
  luaL_openlibs(State);

  // load_luapp_lo(State);

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

static void ReportNILPath(ArrayRef<const char*> Path) {
  for (unsigned i = 0; i < Path.size(); ++i) {
    if (i > 0)
      errs() << '.';

    errs() << Path[i];
  }

  errs() << " is missed in the configuration script!\n";
}

LuaRef
LuaI::getValueRecursively(LuaRef Parent,
                               ArrayRef<const char*> Path) const {
  if (Parent.isNil())
    return Parent;

  LuaRef R = Parent[Path.front()];

  if (Path.size() == 1)
    return R;

  return getValueRecursively(R, Path.slice(1));
}

LuaRef LuaI::getValue(ArrayRef<const char*> Path) const {
  LuaRef Root = getGlobal(State, Path[0]);

  if (Path.size() == 1)
    return Root;

  return getValueRecursively(Root, Path.slice(1));
}

template<typename T>
T LuaI::getValueT(ArrayRef<const char*> Path) const {
  LuaRef R = getValue(Path);

  if (R.isNil()) {
    ReportNILPath(Path);
    return T();
  }

  return R.cast<T>();
}

LuaRef LuaI::getValue(const char *Name) const {
  const char *Path[] = { Name };
  return getValue(Path);
}

std::string LuaI::getValueStr(const char *Name) const {
  const char *Path[] = { Name };
  return getValueStr(Path);
}


std::string LuaI::getValueStr(ArrayRef<const char*> Path) const {
  return getValueT<std::string>(Path);
}

bool LuaI::runScriptStr(const std::string &ScriptStr, SMDiagnostic &Err) {
  // Run the script.
  if (luaL_dostring(State, ScriptStr.c_str())) {
    Err = SMDiagnostic(ScriptStr, SourceMgr::DK_Warning, lua_tostring(State, -1));
    return false;
  }

  return true;
}

bool LuaI::runScriptFile(const std::string &ScriptPath, SMDiagnostic &Err) {
  // Run the script.
  if (luaL_dofile(State, ScriptPath.c_str())) {
    Err = SMDiagnostic(ScriptPath, SourceMgr::DK_Warning, lua_tostring(State, -1));
    return false;
  }

  return true;
}

template<enum VFUs::FUTypes T>
void LuaI::initSimpleFU(LuaRef FUs) {
  FUSet[T] = new VSimpleFUDesc<T>(FUs[VFUDesc::getTypeName(T)]);
}

void LuaI::updateFUs() {
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
#define READPARAMETER(PARAMETER, T) { \
    LuaRef R = FUs[#PARAMETER]; \
    if (R.isNil()) \
      errs() << "FUs."#PARAMETER \
             << " is missed in the configuration script!\n"; \
    else \
      VFUs::PARAMETER = R.cast<T>(); \
  }

  READPARAMETER(LUTCost, unsigned);
  READPARAMETER(RegCost, unsigned);

  READPARAMETER(Period, double);
  READPARAMETER(LUTDelay, float);
  READPARAMETER(ClkEnDelay, float);
  READPARAMETER(RegDelay, float);
  READPARAMETER(MaxLutSize, unsigned);
}

void LuaI::updateStatus() {
  updateFUs();

  // Read the synthesis attributes.
  const char *Path[] = { "SynAttr", "DirectClkEnAttr" };
  VASTNode::DirectClkEnAttr = getValueStr(Path);
  Path[1] = "ParallelCaseAttr";
  VASTNode::ParallelCaseAttr = getValueStr(Path);
  Path[1] = "FullCaseAttr";
  VASTNode::FullCaseAttr = getValueStr(Path);

  // Build the data layout.
  raw_string_ostream s(DataLayout);

  // FIXME: Set the correct endian.
  s << 'e';

  s << '-';

  // Setup the address width (pointer width).
  unsigned PtrSize = Get<VFUMemBus>()->getAddrWidth();
  s << "p:" << PtrSize << ':' << PtrSize << ':' << PtrSize << '-';

  // FIXME: Setup the correct integer layout.
  s << "i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-";
  s << "n8:16:32:64";

  s.flush();
}

ManagedStatic<LuaI> Interpreter;

VFUDesc *LuaI::Get(enum VFUs::FUTypes T) {
  return Interpreter->FUSet[T];
}

LuaI &LuaI::Get() { return *Interpreter; }

std::string LuaI::GetDataLayout() {
  return Interpreter->DataLayout;
}

bool LuaI::Load(const std::string &Path) {
  Interpreter->init();

  SMDiagnostic Err;
  if (!Interpreter->runScriptFile(Path, Err)){
    report_fatal_error(Err.getMessage());
    return true;
  }

  Interpreter->updateStatus();

  return false;
}

std::string LuaI::GetString(ArrayRef<const char*> Path) {
  return Interpreter->getValueStr(Path);
}

std::string LuaI::GetString(const char *Name) {
  return Interpreter->getValueStr(Name);
}

bool LuaI::GetBool(ArrayRef<const char*> Path) {
  return Interpreter->getValueT<bool>(Path);
}

float LuaI::GetFloat(ArrayRef<const char*> Path) {
  return Interpreter->getValueT<float>(Path);
}

bool LuaI::EvalString(const std::string &ScriptStr, SMDiagnostic &Err) {
  return Interpreter->runScriptStr(ScriptStr, Err);
}
