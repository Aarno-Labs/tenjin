// Questions about the shape of an expression that the stages share: which
// implicit casts to look through, and for what; the operands of `p + i`; `&*p`;
// and where an expression sits in its parent.
//
// Three views of an expression look through different implicit casts:
//
//   stripValueCasts      The place or pointer the expression denotes, the
//                        same value whatever its C type. Used to classify a
//                        source by shape (`b`, `&b[i]`, `b + i`).
//   stripGuidanceCasts   The expression whose typedef sugar clang keeps in
//                        the cast's type: lvalue-to-rvalue, decay and
//                        qualification casts. A bit cast changes the type, so
//                        sugar, and with it guidance, is dropped there.
//   valueExpr            The value before the conversions that fit it to the
//                        place it flows into; the lvalue-to-rvalue or decay
//                        cast itself is kept.

#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"

#include <optional>

namespace xj {

// `E` without parentheses and the implicit casts that keep its value:
// lvalue-to-rvalue, decay, qualification, and bit casts between pointers.
const clang::Expr *stripValueCasts(const clang::Expr *E);

// `E` without parentheses and the implicit casts that keep its typedef
// sugar: lvalue-to-rvalue, decay and qualification.
const clang::Expr *stripGuidanceCasts(const clang::Expr *E);

// `E` before the conversions that fit it to its sink, and that value's type.
const clang::Expr *valueExpr(const clang::Expr *E);
clang::QualType valueType(const clang::Expr *E);
// The type of that value as written, before array decay.
clang::QualType writtenType(const clang::Expr *E);

// The pointer and integer operands of `p + i` or `i + p`.
struct PointerSum {
  const clang::Expr *Base;
  const clang::Expr *Index;
};
// `E`, already stripped as the caller wants, as a pointer sum; none when it
// is not one.
std::optional<PointerSum> asPointerSum(const clang::Expr *E);

// The `*p` of `&*p`, through parentheses; null when `AddrOf` is not that.
const clang::UnaryOperator *cancelledDeref(const clang::UnaryOperator *AddrOf);

// The statement or expression containing `E`, above any parentheses; null
// when there is not exactly one.
const clang::Stmt *parentThroughParens(clang::ASTContext &Ctx,
                                       const clang::Expr *E);
// `E` is read: C converts it from the place it names to its value.
bool isRead(clang::ASTContext &Ctx, const clang::Expr *E);
// `E` is evaluated for its effect alone: an expression statement, or the
// init or increment of a `for`. An initializer, a returned value or a
// condition uses its value.
bool isStatement(clang::ASTContext &Ctx, const clang::Expr *E);

// A RecursiveASTVisitor over evaluated code: the operands of `sizeof`,
// `_Alignof` and `_Generic` are not visited.
template <typename Derived>
class EvaluatedCodeVisitor : public clang::RecursiveASTVisitor<Derived> {
public:
  bool TraverseUnaryExprOrTypeTraitExpr(clang::UnaryExprOrTypeTraitExpr *) {
    return true;
  }
  bool TraverseGenericSelectionExpr(clang::GenericSelectionExpr *) {
    return true;
  }
};

} // namespace xj
