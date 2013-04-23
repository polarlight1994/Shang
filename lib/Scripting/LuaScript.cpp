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
#include "BindingTraits.h"
#include "LuaScript.h"

#include "shang/Passes.h"
#include "shang/Utilities.h"

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

using namespace llvm;

// The text template processing lua module
#ifdef _MSC_VER
static void load_luapp_lo(lua_State *L) {
#endif
#include "luapp.inc"
#ifdef _MSC_VER
}
#endif

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

  // Bind our class.
  luabind::open(State);

  // Bind the C++ classes.
  luabind::module(State)[
    BindingTraits<VASTPort>::register_("VASTPort"),

    BindingTraits<VASTModule>::register_("VASTModule")
  ];

  // Bind the object.
  luabind::globals(State)["ExternalTool"] = luabind::newtable(State);
  luabind::globals(State)["FUs"] = luabind::newtable(State);
  luabind::globals(State)["Functions"] = luabind::newtable(State);
  luabind::globals(State)["Modules"] = luabind::newtable(State);
  // The scripting pass table.
  luabind::globals(State)["Passes"] = luabind::newtable(State);
  // Synthesis attribute
  luabind::globals(State)["SynAttr"] = luabind::newtable(State);
  // Table for Miscellaneous information
  luabind::globals(State)["Misc"] = luabind::newtable(State);
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
void LuaScript::initSimpleFU(luabind::object FUs) {
  FUSet[T] = new VSimpleFUDesc<T>(FUs[VFUDesc::getTypeName(T)]);
}

void LuaScript::updateFUs() {
  luabind::object FUs = luabind::globals(State)["FUs"];
  // Initialize the functional unit descriptions.
  FUSet[VFUs::MemoryBus]
    = new VFUMemBus(FUs[VFUDesc::getTypeName(VFUs::MemoryBus)]);
  FUSet[VFUs::BRam] = new VFUBRAM(FUs[VFUDesc::getTypeName(VFUs::BRam)]);

  initSimpleFU<VFUs::AddSub>(FUs);
  initSimpleFU<VFUs::Shift>(FUs);
  initSimpleFU<VFUs::Mult>(FUs);
  initSimpleFU<VFUs::ICmp>(FUs);
  initSimpleFU<VFUs::Sel>(FUs);
  initSimpleFU<VFUs::Reduction>(FUs);

  FUSet[VFUs::Mux] = new VFUMux(FUs[VFUDesc::getTypeName(VFUs::Mux)]);

  // Read other parameters.
#define READPARAMETER(PARAMETER, T) \
  if (boost::optional<T> PARAMETER \
      = luabind::object_cast_nothrow<T>(FUs[#PARAMETER])) \
    VFUs::PARAMETER = PARAMETER.get();

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
  VASTNode::DirectClkEnAttr = getValue<std::string>(Path);
  Path[1] = "ParallelCaseAttr";
  VASTNode::ParallelCaseAttr = getValue<std::string>(Path);
  Path[1] = "FullCaseAttr";
  VASTNode::FullCaseAttr = getValue<std::string>(Path);

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

// Dirty Hack: Allow we invoke some scripting function in the libraries
// compiled with no-rtti
void llvm::bindToScriptEngine(const char *name, VASTModule *M) {
  Script->bindToGlobals(name, M);
}

unsigned llvm::getIntValueFromEngine(ArrayRef<const char*> Path) {
  return Script->getValue<unsigned>(Path);
}

std::string llvm::getStrValueFromEngine(ArrayRef<const char*> Path) {
  return Script->getValue<std::string>(Path);
}

bool llvm::runScriptFile(const std::string &ScriptPath, SMDiagnostic &Err) {
  return Script->runScriptFile(ScriptPath, Err);
}

bool llvm::runScriptStr(const std::string &ScriptStr, SMDiagnostic &Err) {
  return Script->runScriptStr(ScriptStr, Err);
}

namespace llvm {
bool loadConfig(const std::string &Path,
                std::map<std::string, std::string> &ConfigTable,
                StringMap<std::string> &TopHWFunctions,
                std::map<std::string, std::pair<std::string, std::string> >
                &Passes) {
  Script->init();

  SMDiagnostic Err;
  if (!Script->runScriptFile(Path, Err)){
    report_fatal_error(Err.getMessage());
    return true;
  }

  Script->updateStatus();

  ConfigTable["InputFile"] = Script->getValueStr("InputFile");
  ConfigTable["SoftwareIROutput"] = Script->getValueStr("SoftwareIROutput");
  ConfigTable["RTLOutput"] = Script->getValueStr("RTLOutput");
  ConfigTable["MCPDataBase"] = Script->getValueStr("MCPDataBase");
  ConfigTable["DataLayout"] = Script->getDataLayout();

  typedef luabind::iterator iterator;
  for (iterator I = iterator(luabind::globals(Script->State)["Functions"]);
       I != iterator(); ++I)
    TopHWFunctions.GetOrCreateValue(luabind::object_cast<std::string>(I.key()),
                                    luabind::object_cast<std::string>(*I));

  for (iterator I = iterator(luabind::globals(Script->State)["Passes"]);
       I != iterator(); ++I) {
    const luabind::object &o = *I;
    Passes.insert(std::make_pair(luabind::object_cast<std::string>(I.key()),
                  std::make_pair(luabind::object_cast<std::string>(o["FunctionScript"]),
                                 luabind::object_cast<std::string>(o["GlobalScript"]))));
  }

  return false;
}
}
