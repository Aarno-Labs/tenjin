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

#include <map>
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

struct Guidance {
  std::vector<Rule<RustType>> VarTypes;
  std::map<std::string, RustType> FnReturnTypes;
  std::vector<Rule<bool>> VarMut;
  llvm::json::Object Raw;

  static llvm::Expected<Guidance> load(llvm::StringRef Path);

  bool alreadyInstrumented() const;
  bool isEmpty() const;

  // The most specific matching rule; ties are broken by JSON order. A tie
  // between rules that disagree is reported through `Conflict`.
  const Rule<RustType> *matchType(const DeclFacts &D,
                                  const Rule<RustType> **Conflict) const;
  const Rule<bool> *matchMut(const DeclFacts &D) const;
  // The `fn_return_type` entry naming `FnName`.
  const std::pair<const std::string, RustType> *
  returnType(llvm::StringRef FnName) const;
};

} // namespace xj
