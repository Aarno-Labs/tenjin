// ParameterResolver — a client of MustEqualAnalysis.
//
// Answers, per tracked pointer, "does this hold the same value as some
// nameable thing at every one of its use sites?", by reading the
// must-equality state at each site out of a finished analysis.

#pragma once

#include "Cell.h"
#include "Evidence.h"
#include "MustEqual.h"
#include "SED.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceLocation.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>

namespace xj::analysis
{
  enum class CandidateKind : unsigned char
  {
    Param,
    Local,
    LValue
  };

  // One use site of a tracked pointer, and the candidates that agreed
  // there. A "use" is a read: every DeclRefExpr to the pointer except the
  // LHS occurrence of an assignment to it, so that
  //
  //     p = t->storage;  use(p);  p = NULL;   /* no uses after */
  //
  // resolves — every use agrees, and the trailing store is dead.
  //
  // Excluding stores from the site set is only half of what makes that
  // safe: a store the consumer cannot *delete* would have the base
  // substituted into its left-hand side, turning a dead store to `p` into
  // a live one to `t->storage`. So `xj-prepare-baserewrite` reconstructs
  // only pointers whose every store is one it can delete — the left arm
  // of the comma the pointer pass emits, which is the form that store
  // arrives in by the time it gets there.
  struct SiteFact
  {
    const clang::DeclRefExpr *Use = nullptr;
    clang::SourceLocation Loc;
    // False for sites in blocks the fixpoint never reached. Such a site is
    // *skipped*, not counted as disagreement — otherwise dead code
    // silently vetoes a good fact.
    bool Reachable = false;
    // The label on the class the pointer was in here, if it carried one.
    // Reporting only: what makes a candidate agree is sharing the pointer's
    // class, not sharing a label — an unlabelled class is still a class.
    std::optional<Label> Value;
    llvm::SmallVector<CellId, 4> Agreeing;
  };

  // The reduced, flow-insensitive fact.
  struct Resolution
  {
    // The cell to substitute, and also the grouping key the slice pass
    // should use in place of `base_text` equality: two pointers share a
    // base iff they resolve to the same cell. A class is not available for
    // that job — it exists only within one program point's partition,
    // whereas this is a fact about the whole function.
    CellId Cell{};
    CandidateKind Kind = CandidateKind::LValue;

    // `Kind::Param` only: at every reachable use site the pointer's class
    // was labelled `InitOf(Cell)`
    bool EntryAnchored = false;

    // Size of the site set that had to agree. Never 0 — a decl with no
    // uses resolves to nothing.
    unsigned Sites = 0;
  };

  class ParameterResolver
  {
  public:
    // Collects the use sites of every tracked pointer of `FD` and the
    // candidates that agreed at each. `A` must be a finished analysis of
    // `FD`, and must outlive the resolver.
    static ParameterResolver run(const clang::FunctionDecl &FD,
                                 const MustEqualAnalysis &A);

    // Every pointer the resolver tracked, in declaration order.
    llvm::ArrayRef<const clang::VarDecl *> tracked() const { return Tracked; }

    // `B` resolves to `C` iff `B` has at least one use site and, at
    // *every* reachable use site, both cells are in the same class.

    // When several candidates survive, the preferred one is param > local
    // > lvalue, then shortest path, then spelling, for determinism.
    std::optional<Resolution> resolve(const clang::VarDecl *B) const
    {
      return resolve(B, [](CellId) { return true; });
    }

    // The same, with the surviving candidates narrowed: `Accept` is
    // consulted for every cell that agreed everywhere, before the
    // preference order picks among them, and `std::nullopt` comes back if
    // it rejects them all. `xj-prepare-baserewrite` uses this to avoid
    // naming a pointer it is itself about to delete.
    std::optional<Resolution>
    resolve(const clang::VarDecl *B,
            llvm::function_ref<bool(CellId)> Accept) const;

    // The unreduced form: `resolve` is exactly this folded by
    // intersection.
    llvm::ArrayRef<SiteFact> sitesFor(const clang::VarDecl *B) const;

    Justification explain(const clang::VarDecl *B) const;

    const CellUniverse &cells() const { return A->cells(); }

  private:
    explicit ParameterResolver(const MustEqualAnalysis &A) : A(&A) {}

    // The per-site comparison: the label on `V`'s class here, and which
    // candidates shared that class.
    SiteFact factAt(const clang::VarDecl *V, const clang::DeclRefExpr *Use,
                    const SED &State) const;

    const MustEqualAnalysis *A;

    llvm::SmallVector<const clang::VarDecl *, 8> Tracked;
    llvm::DenseMap<const clang::VarDecl *, llvm::SmallVector<SiteFact, 4>> Sites;

    // Candidates a resolution may name: every cell except those rooted at a
    // global or a static local. A signal handler can fire between two
    // statements with no call in between, so a global is never a stable
    // enough thing to substitute.
    llvm::SmallVector<CellId, 16> Candidates;
  };

} // namespace xj::analysis
