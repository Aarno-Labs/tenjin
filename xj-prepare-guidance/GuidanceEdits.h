// The pass's rewrites of one file, as data, over an xj::EditForest.
//
// Rewrites here compose: `*p` becomes `p[0]`, and the same expression may
// then be wrapped in a marker call, whose argument may itself contain
// rewritten subexpressions. The forest renders edits that nest; what it does
// not allow is two edits on the same text, which happens here whenever a
// later stage wraps what an earlier one rewrote. So the edits on one range
// are kept as a stack of layers, and the forest sees the range once: a layer
// gets the text the layers below it made, and the first layer gets the
// range's text with every edit strictly inside it applied. Only a wrap reads
// that text; a rewrite that does not must be the first layer on its range,
// or it would discard the layers below it.
//
// An edit that crosses one already made, or that would discard one, is
// refused and reported by its result. A rewrite that drops nested edits on
// purpose says so with discard(); any other nested edit it leaves out is
// found by the forest when the edits are applied, and the file is then left
// alone. Insertions go before whatever starts at their offset; they are used
// at the start of top-level declarations, which no edit straddles.

#pragma once

#include "Registry.h"

#include "EditForest.h"

#include "clang/AST/Expr.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace xj {

// One rewrite of an expression or declaration. `Base` and `Index` are
// subexpressions of what is rewritten, rendered with the edits inside them;
// a null `Base` is the text the edit wraps.
struct Edit {
  enum class Kind {
    Call,      // M(Base, Index)
    Subscript, // Base[Index]
    Deref,     // (*Base)
    Keep,      // Base
    Advance,   // Base = M(Base, Index)
    Retype,    // Before <typedef> After
  };
  // Written around a call.
  enum class Around { None, Deref, Not };

  Kind K;
  const MarkerKey *Marker = nullptr;
  const clang::Expr *Base = nullptr;
  const clang::Expr *Index = nullptr;
  // Written for a null `Index`; with neither, the call has one argument.
  std::string IndexText;
  Around Wrapper = Around::None;
  const TypedefKey *Typedef = nullptr;
  std::string Before, After;

  // `M(<wrapped>)`, or `M(<wrapped>, Extra)`.
  static Edit wrapIn(const MarkerKey &M, std::string Extra = "");
  // `M(Base, Index)`; a null `Index` is `NoIndex`.
  static Edit call(const MarkerKey &M, const clang::Expr *Base,
                   const clang::Expr *Index, std::string NoIndex = "");
  static Edit subscript(const clang::Expr *Base, const clang::Expr *Index);
  static Edit deref(const clang::Expr *Base);
  static Edit keep(const clang::Expr *Base);
  // `Base = M(Base, Step)`; a null `Step` is 1.
  static Edit advance(const MarkerKey &M, const clang::Expr *Base,
                      const clang::Expr *Step);
  static Edit retype(const TypedefKey &K, std::string Before,
                     std::string After);

  Edit around(Around A) && {
    Wrapper = A;
    return std::move(*this);
  }
  // Reads the text below it, so it can go on top of other layers.
  bool wraps() const { return K == Kind::Call && !Base; }
};

class GuidanceEdits {
public:
  enum class Result { Made, OutsideMainFile, CrossesEdit, DiscardsEdit };
  // An edit the forest would lose, at the start of its range.
  struct Lost {
    clang::SourceLocation Loc;
    std::string Text;
    const char *What;
  };

  GuidanceEdits(const clang::SourceManager &SM, const clang::LangOptions &LO,
                const Registry &Reg);

  // The main-file text a token range covers, if it is written there.
  std::optional<FileRange> mainFileRange(clang::SourceRange R) const;

  Result add(clang::SourceRange R, Edit E);
  // Forgets the edits on exactly `R`, which an edit containing it has made
  // moot.
  void discard(clang::SourceRange R);
  void insertBefore(unsigned Offset, std::string Text);
  static llvm::StringRef describe(Result R);

  // Hands every edit and insertion to `RW`, unless the forest finds an
  // edit it would lose: then nothing is applied, and those are returned.
  std::vector<Lost> apply(clang::Rewriter &RW) const;

private:
  using Renderer = EditForest::NestedEditRenderer;
  using Offsets = std::pair<unsigned, unsigned>;
  struct Layers {
    clang::SourceRange Range;
    std::vector<Edit> Edits;
  };

  const clang::SourceManager &SM;
  const clang::LangOptions &LO;
  const Registry &Reg;
  std::map<Offsets, Layers> Planned;
  std::map<unsigned, std::string> Insertions;

  bool crossesExisting(const FileRange &R) const;
  std::string render(const Layers &L, const Renderer &In) const;
  std::string render(const Edit &E, const std::string &Below,
                     const Renderer &In) const;
};

} // namespace xj
