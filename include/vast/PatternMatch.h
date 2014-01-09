//===---- vast/PatternMatch.h - Match on the VAST Expr DAG ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides a simple and efficient mechanism for performing general
// tree-based pattern matches on the VAST Expr DAG.  The power of these routines
// is that it allows you to write concise patterns that are expressive and easy
// to understand.  The other major advantage of this is that it allows you to
// trivially capture/bind elements in the pattern to variables. More information
// canbe found in llvm/Support/PatternMatch.h
//
//===----------------------------------------------------------------------===//

#ifndef VAST_PATTERN_MATCH_H
#define VAST_PATTERN_MATCH_H

#include "vast/VASTDatapathNodes.h"
#include "vast/VASTSeqValue.h"

namespace vast {
namespace PatternMatch {

template<typename Pattern>
typename Pattern::RetTy match(VASTExpr *Expr, const Pattern &P) {
  return P.match(Expr);
}

template<typename Pattern>
typename Pattern::RetTy match(VASTValue *V, const Pattern &P) {
  VASTExpr *Expr = dyn_cast<VASTExpr>(V);

  if (Expr == NULL)
    return NULL;

  return match<Pattern>(Expr, P);
}

template<typename Pattern, bool IgnoreInvert>
typename Pattern::RetTy match(VASTValPtr V, const Pattern &P) {
  if (V.isInverted() && !IgnoreInvert)
    return NULL;

  return match<Pattern>(V.get(), P);
}

template<typename Pattern>
typename Pattern::RetTy match(VASTValPtr V, const Pattern &P) {
  return match<Pattern, false>(V, P);
}

template<typename Pattern>
typename Pattern::RetTy matchUnderlying(VASTValPtr V, const Pattern &P) {
  return match<Pattern, true>(V, P);
}

struct True_match {
  typedef VASTExpr *RetTy;
  VASTExpr *match(VASTExpr *Expr) { return Expr; }
};

template<unsigned N>
struct ExtractOp {
  typedef VASTValPtr RetTy;
  VASTValPtr match(VASTExpr *Expr) const { return Expr->getOperand(N); }
};

/// Match an expression with the given opcode.
template<VASTExpr::Opcode Opcode, typename SubPattern_t>
struct Opcode_match {
  const SubPattern_t &SubPattern;
  typedef typename SubPattern_t::RetTy RetTy;

  Opcode_match(const SubPattern_t &SP) : SubPattern(SP) {}

  RetTy match(VASTExpr *Expr) const {
    if (Expr->getOpcode() != Opcode)
      return NULL;

    return SubPattern.match(Expr);
  }
};

template<VASTExpr::Opcode Opcode>
inline Opcode_match<Opcode, True_match> m_Opcode() {
  return Opcode_match<Opcode, True_match>(True_match());
}

template<typename LTy, typename RTy>
struct match_binbine_or {
  const LTy &LHS;
  const RTy &RHS;

  typedef typename LTy::RetTy RetTy;

  match_binbine_or(const LTy &LHS, const RTy &RHS) : LHS(LHS), RHS(RHS) {}
  
  RetTy match(VASTExpr *Expr) const {
    if (RetTy R = LHS.match(Expr))
      return R;

    return RHS.match(Expr);
  }
};

/// Pattern matching for generic NAry expression.
/// Match a Binary expression with only 1 non-const operand.
struct BinaryOpWithConst {
  typedef VASTExpr *RetTy;

  VASTExpr *match(VASTExpr *Expr) const {
    VASTExpr::Opcode Opcode = Expr->getOpcode();

    if (Opcode < VASTExpr::FirstFUOpc || Opcode >= VASTExpr::FirstICmpOpc)
      return NULL;

    VASTValue *V = NULL;
    for (unsigned i = 0; i < Expr->size(); ++i) {
      VASTValPtr Op = Expr->getOperand(i);
      if (isa<VASTConstant>(Op.get()))
        continue;

      // There is already a non constant operand.
      if (V != NULL)
        return NULL;

      V = Op.get();
    }

    return Expr;
  }
};

/// Match an expr extract bits from register.
struct ExtractSeqVal {
  typedef bool RetTy;

  bool match(VASTExpr *Expr) const {
    if (Expr->getOpcode() != VASTExpr::dpBitExtract)
      return false;

    return Expr->getOperand(0).getAsLValue<VASTSeqValue>() != NULL;
  }
};
//===----------------------------------------------------------------------===//
//
// Matcher for specificed expression type
inline match_binbine_or<Opcode_match<VASTExpr::dpSAnn, ExtractOp<0> >,
                        Opcode_match<VASTExpr::dpHAnn, ExtractOp<0> > >
extract_annotation() {
  typedef ExtractOp<0> ExtractOp0;
  typedef Opcode_match<VASTExpr::dpSAnn, ExtractOp0> MatchSAnn;
  typedef Opcode_match<VASTExpr::dpHAnn, ExtractOp0> MatchHAnn;
  typedef match_binbine_or<MatchSAnn, MatchHAnn> MatchAnn;
  return MatchAnn(MatchSAnn(ExtractOp0()), MatchHAnn(ExtractOp0()));
}

} // end of namespace PatternMatch
} // end of namespace vast

#endif
