//===--------- vast/LuaScript.h - Lua Scripting Support ------------===//
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

#ifndef VAST_LUA_SCRIPT_H
#define VAST_LUA_SCRIPT_H

#include "vast/FUInfo.h"

#include "llvm/IR/Function.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/IndexedMap.h"
#include "llvm/ADT/ArrayRef.h"

#include <map>

// Forward declaration.
struct lua_State;

namespace luabridge {
  class LuaRef;
}

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
  void initSimpleFU(luabridge::LuaRef FUs);

  friend bool loadConfig(const std::string &Path);

  void init();
  // Read the Function units information from script engine
  void updateFUs();
  // Update the status of script engine after script run.
  void updateStatus();

  luabridge::LuaRef
  getValueRecursively(luabridge::LuaRef Parent,
                      ArrayRef<const char*> Path) const;
public:

  LuaScript();
  ~LuaScript();

  // template<class T>
  // void bindToGlobals(const char *Name, T *O) {
  //  luabind::globals(State)[Name] = O;
  //}

  luabridge::LuaRef getValue(ArrayRef<const char*> Path) const;
  std::string getValueStr(ArrayRef<const char*> Path) const;

  luabridge::LuaRef getValue(const char *Name) const;
  std::string getValueStr(const char *Name) const;
  std::string getValueStr(const std::string &Name) const {
    return getValueStr(Name.c_str());
  }

  bool runScriptFile(const std::string &ScriptPath, SMDiagnostic &Err);
  bool runScriptStr(const std::string &ScriptStr, SMDiagnostic &Err);

  const std::string &getDataLayout() const { return DataLayout; }
};

LuaScript &scriptEngin();
}

#endif
