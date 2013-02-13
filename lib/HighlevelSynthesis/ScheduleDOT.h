//===---- ScheduleDOT.h - DOTGraphTraits for Schedule Graph -------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the DOTGraphTraits for Schedule Graph.
//
//===----------------------------------------------------------------------===//
//

#ifndef VAST_SCHEDULE_DOT
#define VAST_SCHEDULE_DOT
#include "VASTScheduling.h"
//#include "SchedulingBase.h"

#include "llvm/Support/GraphWriter.h"
#include "llvm/ADT/StringExtras.h"

namespace llvm {
template<>
struct DOTGraphTraits<VASTSchedUnit*> : public DefaultDOTGraphTraits {

  explicit DOTGraphTraits(bool isSimple=false)
    : DefaultDOTGraphTraits(isSimple) {}

  /// If you want to override the dot attributes printed for a particular
  /// edge, override this method.
  template<typename GraphType>
  static std::string getEdgeAttributes(VASTSchedUnit *Node,
                                       VASTSchedUnit::use_iterator EI,
                                       const GraphType &) {
    VASTSchedUnit *Use = *EI;
    VASTDep UseEdge = Use->getEdgeFrom(Node);

    switch (UseEdge.getEdgeType()) {
    case VASTDep::ValDep:          return "";
    case VASTDep::MemDep:          return "color=blue,style=dashed";
    case VASTDep::CtrlDep:         return "color=green,style=dashed";
    case VASTDep::FixedTiming:     return "color=red";
    case VASTDep::LinearOrder:     return "color=green";
    case VASTDep::Conditional:     return "color=blue";
    case VASTDep::Predicate:       return "color=navyblue";
    }

    llvm_unreachable("Unexpected edge type!");
    return "";
  }

  static std::string getEdgeSourceLabel(VASTSchedUnit *Node,
                                        VASTSchedUnit::use_iterator EI) {
    VASTSchedUnit *Use = *EI;
    VASTDep UseEdge = Use->getEdgeFrom(Node);

    return utostr(UseEdge.getLatency()) + ',' + itostr(UseEdge.getDistance());
  }

  template<typename GraphType>
  std::string getNodeLabel(const VASTSchedUnit *Node, const GraphType &) {
    std::string Str;
    raw_string_ostream ss(Str);
    Node->print(ss);
    return ss.str();
  }

  template<typename GraphType>
  static std::string getNodeAttributes(const void *,
                                       const GraphType &) {
    return "shape=Mrecord";
  }
};

template<>
struct DOTGraphTraits<VASTSchedGraph*>
  : public DOTGraphTraits<VASTSchedUnit*> {

  explicit DOTGraphTraits(bool isSimple = false)
    : DOTGraphTraits<VASTSchedUnit*>(isSimple) {}


};
}

#endif
