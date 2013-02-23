//===----- VASTHandle.h - VASTNode Smart Pointer Class ----------*- C++ -*-===//
//
//                      The Shang HLS frameowrk                               //
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the VASTHandle class.
//
//===----------------------------------------------------------------------===//

#ifndef SHANG_VAST_HANDLE_H
#define SHANG_VAST_HANDLE_H

#include "shang/VASTNodeBases.h"

#include "llvm/Support/Casting.h"

namespace llvm {
class raw_ostream;

class VASTHandle : public VASTNode {
  VASTUse U;

  void unlinkFromUser() {
    if (!U.isInvalid()) U.unlinkUseFromUser();
  }
public:
  VASTHandle(VASTValue *Ptr = 0)
    : VASTNode(VASTNode::vastHandle), U(this, Ptr) {}
  
  VASTHandle(VASTValPtr Ptr)
    : VASTNode(VASTNode::vastHandle), U(this, Ptr) {}
  
  VASTHandle(const VASTHandle &RHS)
    : VASTNode(VASTNode::vastHandle), U(this, RHS) {}
  
  VASTValPtr operator=(VASTValPtr V) {
    unlinkFromUser();

    if (V) U.set(V);

    return U.unwrap();
  }

  VASTValPtr operator=(VASTValue *RHS) {
    return operator=(VASTValPtr(RHS));
  }

  VASTValPtr operator=(const VASTHandle &RHS) {
    return operator=(VASTValPtr(RHS));
  }

  VASTValPtr operator->() const { return U.unwrap(); }

  bool operator==(VASTValPtr RHS) const {
    return U.unwrap() == RHS;
  }

  bool operator!=(VASTValPtr RHS) const {
    return !operator==(RHS);
  }

  operator VASTValPtr() const { return U.unwrap(); }
  operator bool() const { return !U.isInvalid(); }

  ~VASTHandle() { unlinkFromUser(); }

  void print(raw_ostream &OS) const;
};

template<> struct simplify_type<VASTHandle> {
  typedef       VASTValPtr SimpleType;        // The real type this represents...

  // An accessor to get the real value...
  static SimpleType getSimplifiedValue(const VASTHandle &Val) {
    return VASTValPtr(Val);
  }
};
}

#endif
