// Operator normalization on guided operands.
//
// A pointer operator means something different once its operand is a Rust
// buffer or reference, so it is rewritten into the form whose Rust meaning is
// direct, before any flow is considered:
//
//     *p, p a buffer               (*xj_index_<p>(p, 0))
//     *(p + i), *(i + p), p[i]     (*xj_index_<p>(p, i))
//     p[0], p a single object      (*p)
//     &*p                          p
//     p++, p += n, p a shared      p = xj_slice_from_<p>(p, n)
//       slice, as a statement
//
// `xj_index_<p>` returns a raw pointer to the element, so its dereference is
// still an lvalue, and the transpiler reads it as `p[i]` without asking what
// `p` is. Guided arrays are indexed as written (`*a` becomes `a[0]`): C
// subscripts of arrays already translate to Rust indexing.
//
// Other pointer motion on a guided pointer (`p--`, `p += n` on a `Vec`),
// differences and orderings between guided pointers, and non-zero
// subscripts of single objects have no Rust counterpart; they are reported,
// not rewritten. The pointer passes that run first remove the motion they
// can prove, so what is left here is what they declined. Unevaluated
// operands are left alone.

#pragma once

#include "MarkerFactory.h"
#include "TUState.h"
#include "XjTyper.h"

#include "clang/AST/Expr.h"
#include "llvm/ADT/DenseSet.h"

namespace xj {

class Normalizer {
public:
  explicit Normalizer(TUState &S) : S(S), Types(S), Markers(S) {}
  void run();

  void visitDeref(const clang::UnaryOperator *UO);
  void visitAddrOf(const clang::UnaryOperator *UO);
  void visitSubscript(const clang::ArraySubscriptExpr *ASE);
  void visitMotion(const clang::UnaryOperator *UO);
  void visitBinary(const clang::BinaryOperator *BO);

private:
  TUState &S;
  XjTyper Types;
  MarkerFactory Markers;
  llvm::DenseSet<const clang::Expr *> Consumed;

  bool isGuidedPointer(const clang::Expr *E) const;
  bool isPointerBuffer(const clang::Expr *E) const;
  void addIndexLayer(const clang::Expr *Node, const clang::Expr *Base,
                     const clang::Expr *Index);
  bool advanceSlice(const clang::Expr *Node, const clang::Expr *Operand,
                    const clang::Expr *Step);
  bool isStatement(const clang::Expr *E) const;
  void report(llvm::StringRef Kind, const clang::Expr *Site,
              const clang::Expr *Operand, llvm::StringRef Message);
};

// Text of `E` usable as the operand of a postfix operator.
std::string postfixOperand(const EditSet &Edits, const clang::Expr *E);

} // namespace xj
