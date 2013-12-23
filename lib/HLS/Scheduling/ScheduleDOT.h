//===---- ScheduleDOT.h - DOTGraphTraits for Schedule Graph -------*- C++ -*-===//
//
//                      The VAST HLS frameowrk                                //
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
#include "SchedulerBase.h"

#include "llvm/Support/GraphWriter.h"
#include "llvm/ADT/StringExtras.h"

namespace llvm {
using namespace vast;

template<> struct DOTGraphTraits<VASTSchedUnit*> : public DefaultDOTGraphTraits{

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
    case VASTDep::Synchronize:     return "color=red,style=dashed";
    case VASTDep::Generic:         return "color=yellow";
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

template<> struct DOTGraphTraits<VASTSchedGraph*>
  : public DOTGraphTraits<VASTSchedUnit*> {

  explicit DOTGraphTraits(bool isSimple = false)
    : DOTGraphTraits<VASTSchedUnit*>(isSimple) {}
};

template<> struct DOTGraphTraits<SchedulerBase*>
  : public DOTGraphTraits<VASTSchedGraph*> {

  DOTGraphTraits(bool isSimple = false)
    : DOTGraphTraits<VASTSchedGraph*>(isSimple) {}

  template<typename GraphType>
  std::string getNodeLabel(const VASTSchedUnit *Node, const GraphType &G) {
    std::string Str;
    raw_string_ostream ss(Str);
    Node->print(ss);
    ss << '[' << G->getASAPStep(Node) << ", " << G->getALAPStep(Node) << ']';
    return ss.str();
  }
};

struct SchedGraphWrapper {
  VASTSchedUnit *Entry;
  std::set<VASTSchedUnit*> SUs;

  /*implicit*/ inline SchedGraphWrapper(VASTSchedUnit *U) : Entry(U) {
    SUs.insert(Entry);
  }
};

template<>
struct GraphTraits<SchedGraphWrapper*> : public GraphTraits<VASTSchedUnit*> {
  typedef VASTSchedUnit NodeType;
  typedef VASTSchedUnit::use_iterator ChildIteratorType;

  static NodeType *getEntryNode(const SchedGraphWrapper* G) {
    return G->Entry;
  }

  typedef std::set<VASTSchedUnit*>::iterator nodes_iterator;

  static nodes_iterator nodes_begin(SchedGraphWrapper *G) {
    return G->SUs.begin();
  }

  static nodes_iterator nodes_end(SchedGraphWrapper *G) {
    return G->SUs.end();
  }
};

template<> struct DOTGraphTraits<SchedGraphWrapper*>
  : public DOTGraphTraits<VASTSchedUnit*> {

  DOTGraphTraits(bool isSimple = false)
    : DOTGraphTraits<VASTSchedUnit*>(isSimple) {}

  static std::string getNodeAttributes(const VASTSchedUnit *Node,
                                       const SchedGraphWrapper *W) {
    std::string Attr = "shape=Mrecord";
    if (Node == W->Entry) Attr += ", fontcolor=blue";
    return Attr;
  }
};
}
#endif
