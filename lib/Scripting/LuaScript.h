//===--------- shang/LuaScript.h - Lua Scripting Support ------------===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the LuaScript class, which provide basic lua scripting
// support.
//
//===----------------------------------------------------------------------===//

#ifndef VTM_LUA_SCRIPT_H
#define VTM_LUA_SCRIPT_H

#include "shang/FUInfo.h"

#include "llvm/IR/Function.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/IndexedMap.h"
#include "llvm/ADT/ArrayRef.h"

// This is the only header we need to include for LuaBind to work
#include "luabind/luabind.hpp"

#include <map>

// Forward declaration.
struct lua_State;

namespace llvm {
// Forward declaration.
class raw_ostream;
class SMDiagnostic;

// Lua scripting support.
class LuaScript {
  // DO NOT IMPLEMENT
  LuaScript(const LuaScript&);
  // DO NOT IMPLEMENT
  const LuaScript &operator=(const LuaScript&);

  lua_State *State;

  IndexedMap<VFUDesc*, CommonFUIdentityFunctor> FUSet;
  std::string DataLayout;

  friend VFUDesc *getFUDesc(enum VFUs::FUTypes T);

  template<enum VFUs::FUTypes T>
  void initSimpleFU(luabind::object FUs);

  friend bool loadConfig(const std::string &Path);

  void init();
  // Read the Function units information from script engine
  void updateFUs();
  // Update the status of script engine after script run.
  void updateStatus();
public:

  LuaScript();
  ~LuaScript();

  template<class T>
  void bindToGlobals(const char *Name, T *O) {
    luabind::globals(State)[Name] = O;
  }

  template<class T>
  T getValue(ArrayRef<const char*> Path) const {
    luabind::object o = luabind::globals(State);
    for (unsigned i = 0; i < Path.size(); ++i)
      o = o[Path[i]];

    boost::optional<T> Res = luabind::object_cast_nothrow<T>(o);

    // If the value not found, just construct then with default constructor.
    if (!Res) return T();

    return Res.get();
  }

  template<class T>
  T getValue(const char *Name) {
    const char *Path[] = { Name };
    return getValue<T>(Path);
  }

  std::string getValueStr(const char *Name) const {
    const char *Path[] = { Name };
    return getValue<std::string>(Path);
  }

  std::string getValueStr(const std::string &Name) const {
    return getValueStr(Name.c_str());
  }

  luabind::object getModTemplate(const std::string &Name) const {
    return luabind::globals(State)["Modules"][Name];
  }

  bool runScriptFile(const std::string &ScriptPath, SMDiagnostic &Err);
  bool runScriptStr(const std::string &ScriptStr, SMDiagnostic &Err);

  const std::string &getDataLayout() const { return DataLayout; }
};

LuaScript &scriptEngin();
}

#endif
