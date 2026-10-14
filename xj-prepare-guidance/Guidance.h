// The parts of xj-guidance.json this pass consumes, and declaration matching.
//
// A declaration specifier is `file@fn:var#line` with every part but `fn`
// optional. Parameters of prototypes match `fn:var` as parameters of
// definitions do. When several specifiers match one declaration, the most
// specific wins (see DeclSpec::specificity), and among equally specific ones
// the first in the JSON.

#pragma once

#include "RustType.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <optional>
#include <string>
#include <vector>

namespace xj {

// The suffix `_plan_static_uniquification` in cli/translation_preparation.py
// appends to a renamed static (`_xjtr_N`).
inline constexpr llvm::StringRef UniqueSuffix = "_xjtr";

// `spec` names `instance`, ignoring a uniquification suffix on the instance.
bool isDerivedName(llvm::StringRef Spec, llvm::StringRef Instance);

struct DeclSpec {
  std::string Text;
  std::string File;
  std::string Fn;
  std::string Var;
  unsigned Line = 0;

  static std::optional<DeclSpec> parse(llvm::StringRef S);
  unsigned specificity() const;
};

// What matching needs to know about a declaration.
struct DeclFacts {
  std::string Name;
  // Enclosing function, or empty at file scope.
  std::string ParentFn;
  // For fields: the record's name as the transpiler spells it.
  std::string Record;
  bool IsField = false;
  bool IsFileScope = false;
  unsigned Line = 0;
  std::string File;
};

bool matches(const DeclSpec &Spec, const DeclFacts &D);

template <typename V> struct Rule {
  DeclSpec Spec;
  V Value;
  // Position in a deterministic order of the JSON, for ties.
  unsigned Order;
};

// The rule `Guidance::matchType` chose for a declaration.
struct TypeMatch {
  const Rule<RustType> *Best = nullptr;
  // An equally specific rule naming a different type, if any.
  const Rule<RustType> *Conflict = nullptr;

  explicit operator bool() const { return Best != nullptr; }
};

struct Guidance {
  std::vector<Rule<RustType>> VarTypes;
  // Specifiers name the function alone, in `Fn`.
  std::vector<Rule<RustType>> FnReturnTypes;
  std::vector<Rule<bool>> VarMut;
  llvm::json::Object Raw;

  static llvm::Expected<Guidance> load(llvm::StringRef Path);

  bool alreadyInstrumented() const;

  // The most specific matching rule; ties are broken by JSON order.
  TypeMatch matchType(const DeclFacts &D) const;
  const Rule<bool> *matchMut(const DeclFacts &D) const;
  // The `fn_return_type` rule naming `FnName`.
  const Rule<RustType> *returnType(llvm::StringRef FnName) const;
};

} // namespace xj
