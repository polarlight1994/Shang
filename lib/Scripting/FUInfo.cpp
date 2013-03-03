//===----- VFunctionUnit.cpp - VTM Function Unit Information ----*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains implementation of the function unit class in
// Verilog target machine.
//
//===----------------------------------------------------------------------===//
#include "LuaScript.h"

#include "shang/FUInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#define DEBUG_TYPE "vtm-fu-info"
#include "llvm/Support/Debug.h"
#include <assert.h>
using namespace llvm;

//===----------------------------------------------------------------------===//
// Helper functions for reading function unit table from script.
template<typename PropType, typename IdxType>
static PropType getProperty(const luabind::object &FUTable, IdxType PropName,
                            PropType DefaultRetVal = PropType()) {
    if (luabind::type(FUTable) != LUA_TTABLE) return DefaultRetVal;

    boost::optional<PropType> Result =
      luabind::object_cast_nothrow<PropType>(FUTable[PropName]);

    if (!Result) return DefaultRetVal;

    return Result.get();
}

static unsigned ComputeOperandSizeInByteLog2Ceil(unsigned SizeInBits) {
  return std::max(Log2_32_Ceil(SizeInBits), 3u) - 3;
}

static void initLogicLevelsTable(luabind::object LuaLatTable, unsigned *LLTable,
                                 unsigned Size) {
  for (unsigned i = 0; i < Size; ++i)
    // Lua array starts from 1
    LLTable[i] = getProperty<unsigned>(LuaLatTable, i + 1, LLTable[i]);
}

static void initLatenciesTable(luabind::object LuaLatTable, float *LatTable,
                               unsigned Size) {
  for (unsigned i = 0; i < Size; ++i)
    // Lua array starts from 1
    LatTable[i] = getProperty<float>(LuaLatTable, i + 1, LatTable[i]);
}

//===----------------------------------------------------------------------===//
/// Hardware resource.
void VFUDesc::print(raw_ostream &OS) const {
}

namespace llvm {
  namespace VFUs {
    const char *VFUNames[] = {
      "Trivial", "AddSub", "Shift", "Mult", "ICmp", "Sel", "Reduction", 
      "MemoryBus", "BRam", "Mux", "CalleeFN"
    };

    // Default area cost parameter.
    unsigned LUTCost = 64;
    unsigned RegCost = 4;
    unsigned MaxLutSize = 4;
    float LUTDelay = 0.5f;
    double Period = 10.0;

    void computeCost(unsigned StartY, unsigned EndY, int Size,
                     unsigned StartX, unsigned Index, unsigned *CostTable){
      float Slope = float((EndY - StartY)) / float((Size - 1));
      int Intercept = StartY - Slope * StartX;
      for (int i = 0; i < Size; ++i){
        CostTable[Index + i] = Slope * (StartX + i) + Intercept;
      }
    }

    void initCostTable(luabind::object LuaCostTable, unsigned *CostTable,
                       unsigned Size) {
        SmallVector<unsigned, 8> CopyTable(Size);
        for (unsigned i = 0; i < Size; ++i)
          // Lua array starts from 1
          CopyTable[i] = getProperty<unsigned>(LuaCostTable, i + 1, CopyTable[i]);

        //Initial the array form a[0] to a[6]
        computeCost(CopyTable[0], CopyTable[1], 7, 1, 0, CostTable);
        //Initial the array form a[7] to a[14]
        computeCost(CopyTable[1], CopyTable[2], 8, 8, 7, CostTable);
        //Initial the array form a[15] to a[30]
        computeCost(CopyTable[2], CopyTable[3], 16, 16, 15, CostTable);
        //Initial the array form a[31] to a[63]
        computeCost(CopyTable[3], CopyTable[4], 33, 32, 31, CostTable);
    }
  }
}


VFUDesc::VFUDesc(VFUs::FUTypes type, const luabind::object &FUTable,
                 unsigned *LogicLevels, float *Latencies, unsigned *Cost)
  : ResourceType(type), StartInt(getProperty<unsigned>(FUTable, "StartInterval")),
    ChainingThreshold(getProperty<unsigned>(FUTable, "ChainingThreshold")) {
  luabind::object LatenciesTable = FUTable["Latencies"];
  initLatenciesTable(LatenciesTable, Latencies, 5);
  luabind::object LogicLevelsTable = FUTable["LogicLevels"];
  initLogicLevelsTable(LogicLevelsTable, LogicLevels, 5);
  luabind::object CostTable = FUTable["Costs"];
  VFUs::initCostTable(CostTable, Cost, 5);
}

float VFUDesc::lookupLatency(const float *Table, unsigned SizeInBits) {
  if (SizeInBits == 0) return 0.0f;

  int i = ComputeOperandSizeInByteLog2Ceil(SizeInBits);
  // All latency table only contains 4 entries.
  assert(i < 4 && "Bad" && "Bad index!");

  float RoundUpLatency = Table[i + 1],
        RoundDownLatency = Table[i];
  unsigned SizeRoundUpToByteInBits = 8 << i;
  unsigned SizeRoundDownToByteInBits = i ? (8 << (i - 1)) : 1;
  float PerBitLatency =
    RoundUpLatency / (SizeRoundUpToByteInBits - SizeRoundDownToByteInBits) -
    RoundDownLatency / (SizeRoundUpToByteInBits - SizeRoundDownToByteInBits);
  // Scale the latency according to the actually width.
  return (RoundDownLatency
          + PerBitLatency * (SizeInBits - SizeRoundDownToByteInBits));
}

unsigned VFUDesc::lookupLogicLevels(const unsigned *Table, unsigned SizeInBits) {
  if (SizeInBits == 0) return 0;

  int i = ComputeOperandSizeInByteLog2Ceil(SizeInBits);
  // All latency table only contains 4 entries.
  assert(i < 4 && "Bad" && "Bad index!");

  const unsigned Scale = 1024;

  unsigned RoundUpLatency = Table[i + 1] * Scale,
           RoundDownLatency = Table[i] * Scale;
  unsigned SizeRoundUpToByteInBits = 8 << i;
  unsigned SizeRoundDownToByteInBits = i ? (8 << (i - 1)) : 1;
  unsigned PerBitLatency =
    RoundUpLatency / (SizeRoundUpToByteInBits - SizeRoundDownToByteInBits) -
    RoundDownLatency / (SizeRoundUpToByteInBits - SizeRoundDownToByteInBits);
  // Scale the latency according to the actually width.
  return (RoundDownLatency
          + PerBitLatency * (SizeInBits - SizeRoundDownToByteInBits)) / Scale;
}

unsigned VFUDesc::lookupCost(const unsigned *Table, unsigned SizeInBits) {
  assert(SizeInBits > 0 && SizeInBits <= 64 && "Bit Size is not appropriate");

  return Table[SizeInBits - 1];
}

VFUMux::VFUMux(luabind::object FUTable)
  : VFUDesc(VFUs::Mux, 1),
    MaxAllowedMuxSize(getProperty<unsigned>(FUTable, "MaxAllowedMuxSize", 1)) {
  assert(MaxAllowedMuxSize <= array_lengthof(MuxLatencies)
         && MaxAllowedMuxSize <= array_lengthof(MuxLogicLevels)
         && "MaxAllowedMuxSize too big!");

  luabind::object LLTable = FUTable["LogicLevels"];
  initLogicLevelsTable(LLTable, MuxLogicLevels, MaxAllowedMuxSize);
  luabind::object LatTable = FUTable["Latencies"];
  initLatenciesTable(LatTable, MuxLatencies, MaxAllowedMuxSize);

  for (unsigned i = 0; i < MaxAllowedMuxSize - 1; i++) {
    luabind::object CostTable = FUTable["Costs"][i+1];
    VFUs::initCostTable(CostTable, MuxCost[i], 5);
  }
}

float VFUMux::getMuxLatency(unsigned Size) {
  if (Size < 2) return 0;

  float ratio = std::max(float(Size) / float(MaxAllowedMuxSize), 1.0f);
  Size = std::min(Size, MaxAllowedMuxSize);

  return MuxLatencies[Size - 2] * ratio;
}

unsigned VFUMux::getMuxLogicLevels(unsigned Size) {
  if (Size < 2) return 0;

  float ratio = std::max(float(Size) / float(MaxAllowedMuxSize), 1.0f);
  Size = std::min(Size, MaxAllowedMuxSize);

  return ceil(MuxLogicLevels[Size - 2] * ratio);
}

unsigned VFUMux::getMuxCost(unsigned Size, unsigned BitWidth) {
  if (Size < 2) return 0.0f;

  float ratio = std::min(float(Size) / float(MaxAllowedMuxSize), 1.0f);
  Size = std::min(Size, MaxAllowedMuxSize);

  // DirtyHack: Allow 65 bit MUX at the moment.
  assert(BitWidth <= 65 && "Bad Mux Size!");
  BitWidth = std::max(BitWidth, 64u);

  return std::ceil(MuxCost[Size - 2][BitWidth] * ratio);
}

VFUMemBus::VFUMemBus(luabind::object FUTable)
  : VFUDesc(VFUs::MemoryBus, getProperty<unsigned>(FUTable, "StartInterval")),
    AddrWidth(getProperty<unsigned>(FUTable, "AddressWidth")),
    DataWidth(getProperty<unsigned>(FUTable, "DataWidth")),
    ReadLatency(getProperty<float>(FUTable, "ReadLatency")) {}

VFUBRAM::VFUBRAM(luabind::object FUTable)
  : VFUDesc(VFUs::BRam, getProperty<unsigned>(FUTable, "StartInterval")),
    DataWidth(getProperty<unsigned>(FUTable, "DataWidth")),
    Latency(getProperty<float>(FUTable, "Latency")),
    Mode(BRAMMode(getProperty<unsigned>(FUTable, "Mode"))),
    Prefix(getProperty<std::string>(FUTable, "Prefix")),
    Template(getProperty<std::string>(FUTable, "Template")),
    InitFileDir(getProperty<std::string>(FUTable, "InitFileDir")) {}

void FuncUnitId::print(raw_ostream &OS) const {
  OS << VFUs::VFUNames[getFUType()];
  // Print the function unit id if necessary.
  if (isBound()) OS << " Bound to " << getFUNum();
}

void FuncUnitId::dump() const {
  print(dbgs());
}

std::string VFUs::instantiatesModule(const std::string &ModName, unsigned ModNum,
                                     ArrayRef<std::string> Ports) {
  std::string Script;
  raw_string_ostream ScriptBuilder(Script);

  luabind::object ModTemplate = scriptEngin().getModTemplate(ModName);
  std::string Template = getProperty<std::string>(ModTemplate, "InstTmplt");

  std::string ResultName = ModName + utostr_32(ModNum) + "_inst";
  // FIXME: Use LUA api directly?
  // Call the preprocess function.
  ScriptBuilder <<
    /*"local " <<*/ ResultName << ", message = require \"luapp\" . preprocess {"
  // The inpute template.
                << "input=[=[";
  if (Template.empty()) {
    ScriptBuilder << "// " << ModName << " not available!\n";
    errs() << "Instantiation template for external module :" << ModName
           << " not available!\n";
    // Dirty Hack: create the finish signal.
    return "parameter " + Ports[3] + "= 1'b1;\n";
  } else
    ScriptBuilder << Template;
  ScriptBuilder <<"]=],"
  // And the look up.
                   "lookup={ num=" << ModNum << ", clk='" << Ports[0]
                << "', rst = '" <<  Ports[1] << "', en = '" <<  Ports[2]
                << "', fin = '" <<  Ports[3];
  // The output ports.
  for (unsigned i = 4, e = Ports.size(); i < e; ++i)
    ScriptBuilder << "', out" << (i - 4) << " = '" <<  Ports[i];

  // End the look up and the function call.
  ScriptBuilder << "'}}\n";
  DEBUG(ScriptBuilder << "print(" << ResultName << ")\n");
  DEBUG(ScriptBuilder << "print(message)\n");
  ScriptBuilder.flush();
  DEBUG(dbgs() << "Going to execute:\n" << Script);

  SMDiagnostic Err;
  if (!scriptEngin().runScriptStr(Script, Err))
    report_fatal_error("External module instantiation:" + Err.getMessage());

  return scriptEngin().getValueStr(ResultName);
}

static bool generateOperandNames(const std::string &ModName, luabind::object O,
                                 unsigned FNNum,
                                 SmallVectorImpl<VFUs::ModOpInfo> &OpInfo) {

  unsigned NumOperands = getProperty<unsigned>(O, "NumOperands");
  if (NumOperands == 0)  return false;

  O = O["OperandInfo"];
  if (luabind::type(O) != LUA_TTABLE) return false;

  std::string Script;
  raw_string_ostream ScriptBuilder(Script);

  std::string ResultName = ModName + utostr_32(FNNum) + "_operand";
  for (unsigned i = 1; i <= NumOperands; ++i) {
    luabind::object OpTab = O[i];
    // FIXME: Use LUA api directly?
    // Call the preprocess function.
    ScriptBuilder <<
      /*"local " <<*/ ResultName << ", message = require \"luapp\" . preprocess {"
      // The inpute template.
      "input=[=[" << getProperty<std::string>(OpTab, "Name") << "]=],"
      // And the look up.
      "lookup={ num=" << FNNum << "}}\n";
    DEBUG(ScriptBuilder << "print(" << ResultName << ")\n");
    DEBUG(ScriptBuilder << "print(message)\n");
    ScriptBuilder.flush();
    DEBUG(dbgs() << "Going to execute:\n" << Script);

    SMDiagnostic Err;
    if (!scriptEngin().runScriptStr(Script, Err))
      report_fatal_error("External module starting:" + Err.getMessage());

    std::string OpName = scriptEngin().getValueStr(ResultName);
    unsigned OpSize = getProperty<unsigned>(OpTab, "SizeInBits");
    OpInfo.push_back(VFUs::ModOpInfo(OpName, OpSize));

    Script.clear();
  }
  return true;
}

unsigned VFUs::getModuleOperands(const std::string &ModName, unsigned FNNum,
                                 SmallVectorImpl<ModOpInfo> &OpInfo) {
  luabind::object O = scriptEngin().getModTemplate(ModName);
  if (luabind::type(O) != LUA_TTABLE) return 0;

  O = O["TimingInfo"];
  if (luabind::type(O) != LUA_TTABLE) return 0;

  unsigned Latency = getProperty<unsigned>(O, "Latency");

  if (generateOperandNames(ModName, O, FNNum, OpInfo))
    return Latency;

  return 0;
}
