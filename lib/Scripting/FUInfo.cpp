//===----- VFunctionUnit.cpp - VTM Function Unit Information ----*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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

#include "vast/FUInfo.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SourceMgr.h"
#define DEBUG_TYPE "vtm-fu-info"
#include "llvm/Support/Debug.h"

// Include the lua headers (the extern "C" is a requirement because we're
// using C++ and lua has been compiled as C code)
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

#include "LuaBridge/LuaBridge.h"

#include <assert.h>

using namespace llvm;
using namespace vast;
using namespace luabridge;

//===----------------------------------------------------------------------===//
// Helper functions for reading function unit table from script.
template<typename PropType, typename IdxType>
static PropType getProperty(const LuaRef &FUTable, IdxType PropName,
                            PropType DefaultRetVal = PropType()) {
  if (FUTable.type() != LUA_TTABLE)
    return DefaultRetVal;

  return FUTable[PropName].cast<PropType>();
}

static unsigned ComputeOperandSizeInByteLog2Ceil(unsigned SizeInBits) {
  return std::max(Log2_32_Ceil(SizeInBits), 3u) - 3;
}

template<typename T>
static void
initializeArray(const LuaRef &LuaLatTable, T *LatTable, unsigned Size) {
  for (unsigned i = 0; i < Size; ++i)
    // Lua array starts from 1
    LatTable[i] = getProperty<T>(LuaLatTable, i + 1, LatTable[i]);
}

//===----------------------------------------------------------------------===//
/// Hardware resource.
void VFUDesc::print(raw_ostream &OS) const {
}

namespace vast {
using namespace llvm;

  namespace VFUs {
    const char *VFUNames[] = {
      "Trivial", "AddSub", "Shift", "Mult", "ICmp",  "MemoryBus", "Mux"
    };

    // Default area cost parameter.
    unsigned LUTCost = 64;
    unsigned RegCost = 4;
    unsigned MaxLutSize = 4;
    float LUTDelay = 0.5f;
    float RegDelay = 0.5f;
    float ClkEnDelay = 0.5f;
    double Period = 10.0;

    void computeCost(unsigned StartY, unsigned EndY, int Size,
                     unsigned StartX, unsigned Index, unsigned *CostTable){
      float Slope = float((EndY - StartY)) / float((Size - 1));
      int Intercept = StartY - Slope * StartX;
      for (int i = 0; i < Size; ++i){
        CostTable[Index + i] = Slope * (StartX + i) + Intercept;
      }
    }

    void initCostTable(const LuaRef &LuaCostTable, unsigned *CostTable,
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


VFUDesc::VFUDesc(VFUs::FUTypes type, const LuaRef &FUTable,
                 float *Latencies, unsigned *Cost)
  : ResourceType(type), StartInt(getProperty<unsigned>(FUTable, "StartInterval")) {
  LuaRef LatenciesTable = FUTable["Latencies"];
  initializeArray(LatenciesTable, Latencies, 5);
  LuaRef CostTable = FUTable["Costs"];
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

unsigned VFUDesc::lookupCost(const unsigned *Table, unsigned SizeInBits) {
  assert(SizeInBits > 0 && SizeInBits <= 64 && "Bit Size is not appropriate");

  return Table[SizeInBits - 1];
}

VFUMux::VFUMux(const LuaRef &FUTable)
  : VFUDesc(VFUs::Mux, 1),
    MaxAllowedMuxSize(getProperty<unsigned>(FUTable, "MaxAllowedMuxSize", 1)) {
  assert(MaxAllowedMuxSize <= array_lengthof(MuxLatencies)
         && "MaxAllowedMuxSize too big!");

  LuaRef LatTable = FUTable["Latencies"];
  initializeArray(LatTable, MuxLatencies, MaxAllowedMuxSize);
  LuaRef CostTable = FUTable["Costs"];
  initializeArray(CostTable, MuxCost, MaxAllowedMuxSize);
}

unsigned VFUMux::getMaxAllowdMuxSize(unsigned Bitwidth) const {
  unsigned L = Log2_32_Ceil(std::max(2u, Bitwidth));
  return 48 / L;
}

float VFUMux::getMuxLatency(unsigned Size) {
  if (Size < 2) return 0;

  float ratio = std::max(float(Size) / float(MaxAllowedMuxSize), 1.0f);
  Size = std::min(Size, MaxAllowedMuxSize);

  return MuxLatencies[Size - 2] * ratio;
}

unsigned VFUMux::getMuxCost(unsigned Size, unsigned BitWidth) {
  if (Size < 2) return 0.0f;

  float ratio = std::min(float(Size) / float(MaxAllowedMuxSize), 1.0f);
  Size = std::min(Size, MaxAllowedMuxSize);

  // DirtyHack: Allow 65 bit MUX at the moment.
  assert(BitWidth <= 65 && "Bad Mux Size!");
  BitWidth = std::max(BitWidth, 64u);

  return MuxCost[Size - 2] * BitWidth * ratio;
}

VFUMemBus::VFUMemBus(const LuaRef &FUTable)
  : VFUDesc(VFUs::MemoryBus, getProperty<unsigned>(FUTable, "StartInterval")),
    AddrWidth(getProperty<unsigned>(FUTable, "AddressWidth")),
    DataWidth(getProperty<unsigned>(FUTable, "DataWidth")),
    ReadLatency(getProperty<float>(FUTable, "ReadLatency")),
    AddrLatency(getProperty<float>(FUTable, "AddrLatency")) {}

void FuncUnitId::print(raw_ostream &OS) const {
  OS << VFUs::VFUNames[getFUType()];
  // Print the function unit id if necessary.
  if (isBound()) OS << " Bound to " << getFUNum();
}

void FuncUnitId::dump() const {
  print(dbgs());
}
