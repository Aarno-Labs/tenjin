// State shared by the per-translation-unit stages, and the run-wide results
// the rewriting sweep accumulates.

#pragma once

#include "Guidance.h"
#include "GuidanceEdits.h"
#include "Registry.h"
#include "TypeSpeller.h"

#include "clang/AST/ASTContext.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/JSON.h"

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace xj {

struct Diagnostic {
  std::string Kind;
  std::string Site;
  std::string Subject;
  std::string Message;

  auto tie() const { return std::tie(Kind, Site, Subject, Message); }
  bool operator<(const Diagnostic &O) const { return tie() < O.tie(); }
};

// Collected by the rewriting sweep only, so each fact is reported once.
struct RunResults {
  std::vector<Diagnostic> Diagnostics;
  // Every TU including a header reports what is wrong in it.
  std::set<Diagnostic> Reported;
  std::set<std::string> MatchedSpecs;
  std::map<std::string, bool> MutResolved;

  llvm::json::Array diagnosticsJson() const;
};

// Guidance carried by an expression: the marker typedef of the declaration
// it reads, possibly under some address-of operators (`&s` is a pointer to
// the typedef of `s`).
struct XjType {
  const TypedefKey *Key = nullptr;
  unsigned AddrOf = 0;

  explicit operator bool() const { return Key != nullptr; }
  // The Rust type of the value itself; null for none or for `&x`.
  const RustType *rust() const { return Key && !AddrOf ? &Key->Rust : nullptr; }
  std::string rustText() const;
};

// Definitions that cannot live in the header. Each goes once per translation
// unit, before the first top-level declaration that needs it.
class LocalPlacement {
public:
  void placeTypedef(const TypedefKey &K, unsigned Offset);
  void placeMarker(const MarkerKey &K, unsigned Offset);
  void emit(GuidanceEdits &Edits, const Registry &R) const;

private:
  std::map<std::string, std::pair<unsigned, const TypedefKey *>> Typedefs;
  std::map<std::string, std::pair<unsigned, const MarkerKey *>> Markers;
};

struct TUState {
  TUState(clang::ASTContext &Ctx, const Guidance &G, Registry &R,
          RunResults *Results);

  clang::ASTContext &Ctx;
  clang::SourceManager &SM;
  const Guidance &G;
  Registry &Reg;
  // Null while collecting keys.
  RunResults *Results;
  TypeSpeller Speller;
  GuidanceEdits Edits;
  LocalPlacement Local;
  llvm::DenseMap<const clang::Decl *, const TypedefKey *> DeclKeys;
  llvm::DenseMap<const clang::FunctionDecl *, const TypedefKey *> ReturnKeys;

  bool inMainFile(clang::SourceLocation Loc) const;
  std::string site(clang::SourceLocation Loc) const;
  void diagnose(llvm::StringRef Kind, clang::SourceLocation Loc,
                llvm::StringRef Subject, llvm::StringRef Message);
  // Offset of the top-level declaration containing `Loc`.
  unsigned topLevelOffset(clang::SourceLocation Loc) const;
  // Edits `St`; an edit that is refused is reported as `edit-refused`.
  bool edit(const clang::Stmt *St, Edit E);
  void placeLocal(const MarkerKey &K, clang::SourceLocation Use);
  void placeLocal(const TypedefKey &K, clang::SourceLocation Use);

private:
  // Main-file offset ranges of the top-level declarations, sorted and
  // disjoint.
  std::vector<std::pair<unsigned, unsigned>> TopLevelRanges;
  void computeTopLevelRanges();
};

} // namespace xj
