// The guidance an expression carries.
//
// After rewriting, clang would read this off the typedef sugar in each
// expression's type. The rewrite has not happened yet, so this computes the
// same answer from the declarations the retyping stage chose: a reference to
// a retyped declaration carries its typedef, subscripting a nested buffer
// yields the element typedef, and `&` adds a pointer level. Arithmetic,
// explicit casts and merges of differently-guided values carry nothing,
// exactly as clang drops sugar there.

#pragma once

#include "TUState.h"

#include "clang/AST/Expr.h"

namespace xj {

class XjTyper {
public:
  explicit XjTyper(const TUState &S) : S(S) {}

  XjType of(const clang::Expr *E) const;
  XjType ofDecl(const clang::Decl *D) const;
  XjType ofReturn(const clang::FunctionDecl *FD) const;
  XjType element(XjType X) const;

  // Guided as a buffer (`&[T]`, `Vec<T>`, `[T; N]`).
  bool isBuffer(const clang::Expr *E) const;
  // Guided as one object behind a reference or box.
  bool isSingleObject(const clang::Expr *E) const;

private:
  const TUState &S;

  XjType ofCall(const clang::CallExpr *CE) const;
  XjType ofConditional(const clang::ConditionalOperator *CO) const;
  XjType ofUnary(const clang::UnaryOperator *UO) const;
};

// `E` without parentheses and the implicit casts that keep its value:
// lvalue-to-rvalue, decay, qualification, and bit casts between pointers.
const clang::Expr *stripValueCasts(const clang::Expr *E);

} // namespace xj
