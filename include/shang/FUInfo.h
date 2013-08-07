//===------- VFunctionUnit.h - VTM Function Unit Information ----*- C++ -*-===//
//
// Copyright: 2011 by SYSU EDA Group. all rights reserved.
// IMPORTANT: This software is supplied to you by Hongbin Zheng in consideration
// of your agreement to the following terms, and your use, installation,
// modification or redistribution of this software constitutes acceptance
// of these terms.  If you do not agree with these terms, please do not use,
// install, modify or redistribute this software. You may not redistribute,
// install copy or modify this software without written permission from
// Hongbin Zheng.
//
//===----------------------------------------------------------------------===//
//
// This file contains define the function unit class in Verilog target machine.
//
//===----------------------------------------------------------------------===//

#ifndef VTM_FUNCTION_UNIT_H
#define VTM_FUNCTION_UNIT_H

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include<set>

namespace luabind {
  namespace adl {
    class object;
  }
  using adl::object;
}

namespace llvm {
class TargetRegisterClass;

namespace VFUs {
  enum FUTypes {
    Trivial = 0,
    AddSub = 1,
    Shift = 2,
    Mult = 3,
    ICmp = 4,
    MemoryBus = 5,
    Mux = 6,
    FirstFUType = Trivial,
    FirstNonTrivialFUType = AddSub,
    LastPostBindFUType = ICmp,
    NumPostBindFUs = LastPostBindFUType - FirstNonTrivialFUType + 1,
    LastCommonFUType = Mux,
    NumCommonFUs = LastCommonFUType - FirstFUType + 1,
    NumNonTrivialCommonFUs = LastCommonFUType - FirstNonTrivialFUType + 1,
    LastFUType = Mux,
    NumFUs = LastFUType - FirstFUType + 1,
    // Helper enumeration value, just for internal use as a flag to indicate
    // all kind of function units are selected.
    AllFUType = 0xf
  };

  extern const char *VFUNames[];

  extern unsigned LUTCost;
  extern unsigned RegCost;
  extern unsigned MaxLutSize;
  extern double Period;

  // Latency of clock enable multiplexer selector
  extern float LUTDelay;

  inline bool isFUCompatible(VFUs::FUTypes LHS, VFUs::FUTypes RHS) {
    if (LHS == RHS)
      return true;

    return false;
  }

  inline bool isNoTrivialFUCompatible(VFUs::FUTypes LHS, VFUs::FUTypes RHS) {
    if (LHS == VFUs::Trivial || RHS == VFUs::Trivial)
      return false;

    return isFUCompatible(LHS, RHS);
  }
}

class FuncUnitId {
  union {
    struct {
      unsigned Type  : 4;
      unsigned Num : 12;
    } ID;

    uint16_t data;
  } UID;

public:
  // The general FUId of a given type.
  inline explicit FuncUnitId(VFUs::FUTypes T, unsigned N) {
    UID.ID.Type = T;
    UID.ID.Num = N;
    assert(unsigned(T) == UID.ID.Type && UID.ID.Num == N
           && "Data overflow!");
  }

  /*implicit*/ FuncUnitId(VFUs::FUTypes T = VFUs::Trivial) {
    UID.ID.Type = T;
    UID.ID.Num = 0xfff;
  }

  explicit FuncUnitId(uint16_t Data) {
    UID.data = Data;
  }

  inline VFUs::FUTypes getFUType() const { return VFUs::FUTypes(UID.ID.Type); }
  inline unsigned getFUNum() const { return UID.ID.Num; }
  inline unsigned getData() const { return UID.data; }
  inline bool isUnknownInstance() const { return getFUNum() == 0xfff; }

  inline bool isTrivial() const { return getFUType() == VFUs::Trivial; }
  inline bool isBound() const {
    return !isTrivial() && getFUNum() != 0xfff;
  }

  inline bool operator==(const FuncUnitId X) const { return UID.data == X.UID.data; }
  inline bool operator!=(const FuncUnitId X) const { return !operator==(X); }
  inline bool operator< (const FuncUnitId X) const { return UID.data < X.UID.data; }

  void print(raw_ostream &OS) const;
  void dump() const;
};

inline static raw_ostream &operator<<(raw_ostream &O, const FuncUnitId &ID) {
  ID.print(O);
  return O;
}

/// @brief The description of Verilog target machine function units.
class VFUDesc {
  VFUDesc(const VFUDesc &);            // DO NOT IMPLEMENT
  void operator=(const VFUDesc &);  // DO NOT IMPLEMENT
protected:
  // The HWResource baseclass this node corresponds to
  const unsigned ResourceType;
  // Start interval
  const unsigned StartInt;

  VFUDesc(VFUs::FUTypes type, unsigned startInt)
    : ResourceType(type), StartInt(startInt) {}

  VFUDesc(VFUs::FUTypes type, const luabind::object &FUTable,
          float *Latencies, unsigned *Cost);

  static float    lookupLatency(const float *Table, unsigned SizeInBits);
  static unsigned lookupCost(const unsigned *Table, unsigned SizeInBits);
public:

  static const char *getTypeName(VFUs::FUTypes FU) {
    return VFUs::VFUNames[FU];
  }

  unsigned getType() const { return ResourceType; }
  const char *getTypeName() const {
    return getTypeName((VFUs::FUTypes)getType());
  }

  virtual void print(raw_ostream &OS) const;
};

class VFUMux : public VFUDesc {
  unsigned MuxCost[64];
  float    MuxLatencies[64];
public:
  const unsigned MaxAllowedMuxSize;

  VFUMux(luabind::object FUTable);

  float    getMuxLatency(unsigned Size);
  unsigned getMuxCost(unsigned Size, unsigned BitWidth);

  static VFUs::FUTypes getType() { return VFUs::Mux; };
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VFUMux *A) { return true; }
  static inline bool classof(const VFUDesc *A) {
    return A->getType() == VFUs::Mux;
  }
};

class VFUMemBus : public VFUDesc {
  unsigned AddrWidth;
  unsigned DataWidth;
  unsigned ReadLatency;
public:
  // The latency of the address MUX of the block RAM.
  const float AddrLatency;

  VFUMemBus(luabind::object FUTable);

  unsigned getAddrWidth() const { return AddrWidth; }
  unsigned getDataWidth() const { return DataWidth; }
  unsigned getReadLatency() const { return ReadLatency; }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const VFUMemBus *A) { return true; }
  static inline bool classof(const VFUDesc *A) {
    return A->getType() == VFUs::MemoryBus;
  }

  static VFUs::FUTypes getType() { return VFUs::MemoryBus; }
  static const char *getTypeName() { return VFUs::VFUNames[getType()]; }

  // Signal names of the function unit.
  inline static std::string getAddrBusName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "addr";
  }

  inline static std::string getInDataBusName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "in";
  }

  inline static std::string getOutDataBusName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "out";
  }

  // Dirty Hack: This should be byte enable.
  inline static std::string getByteEnableName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "be";
  }

  inline static std::string getCmdName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "cmd";
  }

  inline static std::string getEnableName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "en";
  }

  inline static std::string getReadyName(unsigned FUNum) {
    return "mem" + utostr(FUNum) + "rdy";
  }

  static const int CMDWidth = 4;
};

template<enum VFUs::FUTypes T>
class VSimpleFUDesc : public VFUDesc {
  float    Latencies[5];
  unsigned Cost[64];
public:
  explicit VSimpleFUDesc(const luabind::object &FUTable)
    : VFUDesc(T, FUTable, Latencies, Cost) {}
  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  template<enum VFUs::FUTypes OtherT>
  static inline bool classof(const VSimpleFUDesc<OtherT> *A) {
    return T == OtherT;
  }
  static inline bool classof(const VFUDesc *A) {
    return A->getType() == T;
  }

  static VFUs::FUTypes getType() { return T; };
  static const char *getTypeName() { return VFUs::VFUNames[getType()]; }

  float lookupLatency(unsigned SizeInBits) const {
    return VFUDesc::lookupLatency(Latencies, SizeInBits);
  }

  unsigned lookupCost(unsigned SizeInBits) {
    return VFUDesc::lookupCost(Cost, SizeInBits);
  }
};

typedef VSimpleFUDesc<VFUs::AddSub>  VFUAddSub;
typedef VSimpleFUDesc<VFUs::Shift>   VFUShift;
typedef VSimpleFUDesc<VFUs::Mult>    VFUMult;
typedef VSimpleFUDesc<VFUs::ICmp>    VFUICmp;

struct CommonFUIdentityFunctor
  : public std::unary_function<enum VFUs::FUTypes, unsigned>{

  unsigned operator()(enum VFUs::FUTypes T) const {
    return (unsigned)T - (unsigned)VFUs::FirstFUType;
  }
};

VFUDesc *getFUDesc(enum VFUs::FUTypes T);

template<class ResType>
ResType *getFUDesc() {
  return cast<ResType>(getFUDesc(ResType::getType()));
}
}

#endif
