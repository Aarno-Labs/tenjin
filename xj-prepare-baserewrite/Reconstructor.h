// Reconstructor — prove a base, substitute it, delete the pointer.
//
// This is the middle stage of the pointer_transform pipeline:
//
//     xj-prepare-pointertransform   total, syntactic (base, index) rewrite
//     xj-prepare-baserewrite        prove a base, substitute it, delete p
//     xj-prepare-slicetransform     detect and reshape
//
// The pointer pass is deliberately without an opinion about bases: it
// retains every tracked pointer as its own base and spells each access
// `p[p_index_xj]`. That is total but it is not progress on its own — a
// retained pointer lowers to `*mut T` plus `.offset()`. What turns it back
// into an index into something nameable is a *proof*, which is what
// xj::analysis::MustEqualAnalysis supplies: for pointer `p` it answers
// "is there one cell `p` equals at every one of its reachable use sites?".
//
// Given such a cell, the rewrite is mechanical:
//
//     T *p = <anything>;                  deleted (a for-init keeps its `;`)
//     p[E]                                B[E]
//     (p = ROOT, p_index_xj = OFF)        p_index_xj = OFF
//     (p + p_index_xj)                    (B + p_index_xj)
//     (p + p_index_xj) OP B + e           p_index_xj OP e
//     (p + p_index_xj) OP B               p_index_xj OP 0
//     (p + p_index_xj) - B                p_index_xj
//
// The last three are index-form recovery, and they are why folding lives
// here rather than in the slice pass: `detectRoots`' bound scan accepts
// `idx OP lenparam` but not `base + idx OP base + lenparam`, which is what
// plain substitution leaves behind for the (ptr, len) family. "Same cell"
// is decl identity plus field path, taken from the analysis — never source
// text.
//
// Declining is always safe, because the retained form is exactly what the
// pointer pass already emitted and already compiles.

#pragma once

#include "PtrIndexMetadata.h"

#include "ResolveParameter.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include <set>
#include <string>
#include <vector>

namespace xj
{

  // Run-wide tally, printed once at the end. The LValue count is the
  // figure this project set out to recover: on main, 23.5% of collapses
  // had a struct field for a base, and those are the ones that need the
  // memory rules rather than plain copy propagation.
  struct ReconstructionStats
  {
    unsigned Considered = 0; // pointers with a record and a decl position
    unsigned Params = 0;
    unsigned Locals = 0;
    unsigned LValues = 0;
    unsigned Unresolved = 0; // the analysis had no answer
    unsigned Declined = 0;   // resolved, but not safely substitutable
    unsigned FunctionsDeclined = 0;
  };

  class Reconstructor
  {
  public:
    Reconstructor(clang::Rewriter &Rewrite, PtrIndexMetadata &Meta,
                  std::set<std::string> &Handled, ReconstructionStats &Stats,
                  bool Verbose)
        : Rewrite(Rewrite), Meta(Meta), Handled(Handled), Stats(Stats),
          Verbose(Verbose) {}

    void run(clang::ASTContext &Ctx);

  private:
    // One pointer that survived every check, ready to be rewritten.
    struct Plan
    {
      // The pointer to replace
      const clang::VarDecl *Ptr = nullptr;
      // The index
      const clang::VarDecl *Index = nullptr;
      // The declaration
      const clang::DeclStmt *Decl = nullptr;
      // Is it in a `for` statement?
      bool DeclIsForInit = false;
      size_t Record = 0; // index into the function record's `pointers`
      analysis::CellId Cell{};
      analysis::CandidateKind Kind = analysis::CandidateKind::LValue;
      // The replacement pointer's spelling
      std::string Base;
      // Uses of `Ptr`, to be rewritten
      std::vector<const clang::DeclRefExpr *> Refs;
      // The comma expressions whose left arm stores to the pointer, as
      // recognized once by `checkSubstitutable`. Deleting that arm is all
      // of `buildEdits`' step 2.
      std::vector<const clang::BinaryOperator *> StoreCommas;
    };

    // One pending source edit, in *original* file offsets.
    //
    // Original coordinates throughout, and applied with an explicit
    // original length, is not a detail. The Rewriter's CharSourceRange
    // overloads re-measure the range against the rewrite buffer, and
    // `RemoveLineIfEmpty` records the line it takes at a rewrite-buffer
    // offset as though it were an original one — so one earlier edit
    // lower in the file makes a later range come out short, silently.
    // This is the same triple `clang::tooling::Replacement` carries, for
    // the same reason, and `applyEdits` converts to one to apply it.
    //
    // Offsets only: `buildEdits` resolves each range as it makes the edit
    // — end-of-token, file check, whole-line widening — and nothing after
    // that has any use for the range it came from. Widening in particular
    // moves `Begin` off the range's start, so the two would not agree.
    // A deletion is empty `Text`, which is what a `Replacement` says too.
    struct Edit
    {
      unsigned Begin = 0;
      unsigned End = 0;
      std::string Text;
    };

    void reconstructFunction(const clang::FunctionDecl &FD,
                             clang::ASTContext &Ctx);

    // Everything that has to hold before a proved base may be substituted,
    // beyond the proof itself. Resolves `IndexName` — the spelling the
    // pointer pass recorded — to the local it names, fills `P.Decl`,
    // `P.Index` and `P.Refs`, and returns nullopt when the pointer passes —
    // otherwise the reason it did not, which `explain()` cannot give because
    // it is not about the proof.
    std::optional<std::string> checkSubstitutable(const clang::FunctionDecl &FD, Plan &P,
                                                  llvm::StringRef IndexName,
                                                  const analysis::ParameterResolver &Resolver,
                                                  clang::ASTContext &Ctx);

    void buildEdits(const clang::FunctionDecl &FD, const Plan &P,
                    const analysis::ParameterResolver &Resolver,
                    clang::ASTContext &Ctx, std::vector<Edit> &Out);

    // `FID` is the function's own file, the one every edit's offsets are
    // measured in — `buildEdits` drops any plan that would reach outside
    // it.
    void applyEdits(std::vector<Edit> &Edits, clang::FileID FID,
                    clang::ASTContext &Ctx);

    clang::Rewriter &Rewrite;
    PtrIndexMetadata &Meta;
    // Function keys already reconstructed in this run. A function defined
    // in a header is parsed once per including TU; the first TU rewrites it
    // on disk, and the rest must not try again.
    std::set<std::string> &Handled;
    ReconstructionStats &Stats;
    bool Verbose;
  };

} // namespace xj
