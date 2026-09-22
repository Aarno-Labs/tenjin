// Nested textual edits over one file.
//
// Rewrites here compose: `*p` becomes `p[0]`, and the same expression may
// then be wrapped in a marker call, whose argument may itself contain
// rewritten subexpressions. clang's Rewriter handles overlapping edits
// badly, so edits are kept as layers on source ranges and rendered
// recursively: a layer sees the text of its range with every edit strictly
// inside it already applied. Layers on the same range apply in the order
// they were added. Insertions go before whatever starts at their offset and
// are only emitted at the top level; they are used at the start of
// top-level declarations, which no edit straddles.

#pragma once

#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace xj {

class EditSet {
public:
  using Layer = std::function<std::string(const std::string &Inner)>;

  EditSet(const clang::SourceManager &SM, const clang::LangOptions &LO)
      : SM(SM), LO(LO) {}

  // File offsets of a token range in the main file, if it has any.
  std::optional<std::pair<unsigned, unsigned>>
  offsets(clang::SourceRange R) const;

  bool addLayer(clang::SourceRange R, Layer L);
  bool addLayer(const clang::Stmt *S, Layer L) {
    return addLayer(S->getSourceRange(), std::move(L));
  }
  void insertBefore(unsigned Offset, std::string Text);

  bool hasEdits(const clang::Stmt *S) const;
  bool empty() const { return Nodes.empty() && Insertions.empty(); }

  // Text of `S` with every edit inside it (and on it) applied.
  std::string render(const clang::Stmt *S) const;
  std::string renderFile() const;

private:
  using Range = std::pair<unsigned, unsigned>;
  struct ByBeginThenOuter {
    bool operator()(const Range &A, const Range &B) const {
      return A.first != B.first ? A.first < B.first : A.second > B.second;
    }
  };

  const clang::SourceManager &SM;
  const clang::LangOptions &LO;
  std::map<Range, std::vector<Layer>, ByBeginThenOuter> Nodes;
  std::map<unsigned, std::string> Insertions;

  bool crossesExisting(Range R) const;
  std::string slice(unsigned Begin, unsigned End) const;
  std::string renderNode(Range R) const;
  std::string renderChildren(Range R, bool TopLevel) const;
};

} // namespace xj
