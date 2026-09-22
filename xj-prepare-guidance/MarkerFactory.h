// Marker functions: identity functions whose name and signature carry the
// decision the transpiler would otherwise have to reconstruct.
//
//     xj_slice_all_k(b)        the whole buffer b
//     xj_slice_from_k(b, i)    the suffix of b from i
//     xj_elem_ref_k(b, i)      element i of b
//     xj_index_k(b, i)         a raw pointer to element i of b, written
//                              `(*xj_index_k(b, i))` for the element itself
//     xj_coerce_k(x)           x, changing representation
//     xj_is_null_k(p)          p == 0
//
// The parameter and return types carry the typedef sugar of the source and
// the sink, so the transpiler reads (from, to) off the declaration.

#pragma once

#include "TUState.h"

#include "clang/AST/Expr.h"

#include <optional>

namespace xj {

// Where a value flows: the C type of the place and its guidance.
struct Sink {
  clang::QualType Type;
  XjType X;
};

class MarkerFactory {
public:
  explicit MarkerFactory(TUState &S) : S(S) {}

  const MarkerKey *place(Family F, clang::QualType BasePointer,
                         const Sink &K, clang::SourceLocation Use);
  const MarkerKey *index(clang::QualType BasePointer, XjType BaseX,
                         XjType ElemX, clang::SourceLocation Use);
  const MarkerKey *coerce(clang::QualType From, XjType FX, const Sink &K,
                          clang::SourceLocation Use);
  const MarkerKey *isNull(clang::QualType T, XjType X,
                          clang::SourceLocation Use);

private:
  TUState &S;

  std::optional<Spelling> spellValue(clang::QualType T, XjType X,
                                     llvm::StringRef Declarator) const;
  std::optional<Spelling> spellReturn(clang::QualType T, XjType X) const;
  clang::QualType rawPlaceType(clang::QualType BasePointer,
                               clang::QualType SinkType) const;
  const MarkerKey *intern(MarkerKey K, clang::SourceLocation Use);
};

} // namespace xj
