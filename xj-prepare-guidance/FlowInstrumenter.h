// Value flows into guided places, and null tests of guided values.
//
// A flow is a value reaching a place of known type: a call argument, the
// right side of `=`, a local initializer (element-wise through initializer
// lists), a returned value, or the operand of an explicit pointer cast.
// Conditional expressions are transparent; each arm flows on its own.
//
// If the value is pointer-shaped and its base is an array or a guided buffer,
// its shape (`b`, `&b[i]`, `b + i`) and the sink's demand (element
// reference, slice, raw pointer) choose one of three place markers. Otherwise,
// if source and sink carry different guidance, the value is wrapped in a
// coercion marker. A guided value is its own representation, so a flow
// between equal guidance on differently written C types (`char *` into
// `const char *`, an array into a pointer) is wrapped too, in an identity
// coercion: no C conversion is left to apply to it. Constant initializers,
// unevaluated operands and variadic arguments are never wrapped.
//
// `p == NULL`, `p != 0`, `!p` and `p` in a condition, for guided `p`, become
// `xj_is_null_k(p)`: clang converts the null constant to `p`'s type, and a
// coercion there would turn a null test into an empty-string test.

#pragma once

#include "MarkerFactory.h"
#include "XjTyper.h"

#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"

namespace xj {

class FlowInstrumenter {
public:
  explicit FlowInstrumenter(TUState &S) : S(S), Types(S), Markers(S) {}
  void run();

  void visitCall(const clang::CallExpr *CE);
  void visitAssign(const clang::BinaryOperator *BO);
  void visitLocalInit(const clang::VarDecl *VD);
  void visitReturn(const clang::ReturnStmt *RS, const clang::FunctionDecl *Fn);
  void visitExplicitCast(const clang::CStyleCastExpr *CE);
  void visitNullComparison(const clang::BinaryOperator *BO);
  void visitLogicalNot(const clang::UnaryOperator *UO);
  void visitCondition(const clang::Expr *Cond);

private:
  TUState &S;
  XjTyper Types;
  MarkerFactory Markers;

  enum class Shape { Whole, Element, Suffix, Backward };
  struct Place {
    Shape Kind;
    const clang::Expr *Base;
    const clang::Expr *Index;
  };
  enum class Demand { Element, Slice, Raw, None };

  void flow(const clang::Expr *Src, const Sink &K);
  void flowInit(const clang::Expr *Init, const Sink &K);
  void flowInitList(const clang::InitListExpr *ILE, const Sink &K);
  bool placeFlow(const clang::Expr *Src, const Sink &K);
  void coerceFlow(const clang::Expr *Src, const Sink &K);
  bool needsCoercion(XjType FX, clang::QualType Written, const Sink &K) const;
  bool retypesGuidedValue(XjType FX, clang::QualType Written,
                          const Sink &K) const;

  Place placeOf(const clang::Expr *Core) const;
  Demand demandOf(const Sink &K) const;
  static Family familyFor(Shape Sh, Demand D);
  void wrapPlace(const MarkerKey &M, const clang::Expr *Core, const Place &P);

  bool isGuidedPointer(const clang::Expr *E) const;
  void nullTest(const clang::Expr *Whole, const clang::Expr *Operand,
                bool Negate);
};

// `E` before the conversions that fit it to its sink, and that value's type.
const clang::Expr *valueExpr(const clang::Expr *E);
clang::QualType valueType(const clang::Expr *E);
// The type of that value as written, before array decay.
clang::QualType writtenType(const clang::Expr *E);

} // namespace xj
