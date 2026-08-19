// MustEqualAnalysis — the facade.
//
// Runs the forward fixpoint over one function and exposes the resulting
// must-equality state at every program point: which cells provably hold the
// same value here, and what value a class carries if it carries a nameable
// one.

#pragma once

#include "Cell.h"
#include "Escape.h"
#include "SED.h"

#include "clang/AST/Decl.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

#include <memory>

namespace clang
{
  class ASTContext;
  class CFG;
  class CFGBlock;
  class Stmt;
} // namespace clang

namespace xj::analysis
{

  struct Options
  {
    // Backstop against a non-converging fixpoint.
    unsigned MaxBlockVisits = 20000;
  };

  class MustEqualAnalysis
  {
  public:
    // Fails only when the function is declined (`Transfer::declines`)
    // or the CFG could not be built.
    static llvm::Expected<MustEqualAnalysis> run(clang::ASTContext &Ctx,
                                                 const clang::FunctionDecl &FD,
                                                 Options Opts = {});

    MustEqualAnalysis(MustEqualAnalysis &&);
    MustEqualAnalysis &operator=(MustEqualAnalysis &&);
    MustEqualAnalysis(const MustEqualAnalysis &) = delete;
    MustEqualAnalysis &operator=(const MustEqualAnalysis &) = delete;
    ~MustEqualAnalysis();

    const CellUniverse &cells() const;
    // The escape facts the transfer was run under. Exposed because a client
    // asking what a value may be substituted for needs the same judgments.
    const EscapeInfo &escapes() const;
    const clang::CFG &cfg() const;

    // In-state of the block, i.e. the join over its predecessors.
    const SED &stateAtEntryOf(const clang::CFGBlock &B) const;

    // Calls `F` with every statement of every reachable block and the state
    // just before it, in CFG order. Per-statement states are replayed from
    // the block in-states rather than stored, so this walk is how a client
    // reads them.
    void forEachStmt(
        llvm::function_ref<void(const clang::Stmt *, const SED &)> F) const;

    // False when `S` sits in a block the fixpoint never reached, or the CFG
    // has no block for it at all.
    bool isReachable(const clang::Stmt *S) const;

  private:
    MustEqualAnalysis();

    // The driver's state — AnalysisDeclContext and the per-block states —
    // is deliberately not in the header: it is not part of the interface,
    // and keeping it out means consumers do not pull in clang/Analysis.
    struct Impl;
    std::unique_ptr<Impl> P;
  };

} // namespace xj::analysis
