//===--- vast/LuaI.h - The wrapper of the embedded LUA Interpreter --------===//
//
//                      The VAST HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the LuaI class, which provide basic lua scripting support.
//
//===----------------------------------------------------------------------===//

#ifndef VAST_LUA_I_H
#define VAST_LUA_I_H

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
class LuaI {
  // DO NOT IMPLEMENT
  LuaI(const LuaI&) LLVM_DELETED_FUNCTION;
  // DO NOT IMPLEMENT
  const LuaI &operator=(const LuaI&)LLVM_DELETED_FUNCTION;

  lua_State *State;

  IndexedMap<VFUDesc*, CommonFUIdentityFunctor> FUSet;
  std::string DataLayout;

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

  luabridge::LuaRef getValue(ArrayRef<const char*> Path) const;

  template<typename T>
  T getValueT(ArrayRef<const char*> Path) const;

  // template<class T>
  // void bindToGlobals(const char *Name, T *O) {
  //  luabind::globals(State)[Name] = O;
  //}

  std::string getValueStr(ArrayRef<const char*> Path) const;

  luabridge::LuaRef getValue(const char *Name) const;
  std::string getValueStr(const char *Name) const;
  std::string getValueStr(const std::string &Name) const {
    return getValueStr(Name.c_str());
  }

  bool runScriptFile(const std::string &ScriptPath, SMDiagnostic &Err);
  bool runScriptStr(const std::string &ScriptStr, SMDiagnostic &Err);
public:

  LuaI();
  ~LuaI();

  static bool EvalString(const std::string &ScriptStr, SMDiagnostic &Err);

  static std::string GetString(const char *Name);
  static std::string GetString(ArrayRef<const char*> Path);

  static float GetFloat(ArrayRef<const char*> Path);

  static bool Load(const std::string &Path);
  static std::string GetDataLayout();
  static LuaI &Get();
  static VFUDesc *Get(enum VFUs::FUTypes T);
  template<class ResType>
  static ResType *Get() {
    return cast<ResType>(Get(ResType::getType()));
  }
};
}

#endif
