// Transfer — the abstract semantics, in five rules.
//
//   T1  x = e,  destination cell D known
//         let K = the class the RHS denotes:
//               CellRead(c) -> classOf(c)
//               Address(c)  -> classFor(AddrOf(c))
//               Constant(k) -> classFor(Const(k))
//               Opaque      -> a new empty class
//         move D into K,
//         then detach every other cell the store invalidates: those whose
//         storage mayOverlap(D), and those whose path walks through D and
//         so no longer denotes what it did
//   T2  a store whose destination is not nameable
//                                         -> detach every cell not out of reach
//   T3  a call                            -> detach every cell not out of reach
//   T4  any element not matched above     -> detach every cell
//   T5  reads, casts, anything without an effect          -> no change
//
// T1 is the standard "strong update plus weak kill". T4 is the blanket
// rule. A DeclStmt initialiser is T1 with `D` the declared variable.
#pragma once

#include "Cell.h"
#include "Escape.h"
#include "SED.h"

#include "llvm/ADT/APSInt.h"

namespace clang
{
  class ASTContext;
  class CFGElement;
  class FunctionDecl;
} // namespace clang

namespace xj::analysis
{

  struct RValue
  {
    enum class Kind
    {
      CellRead,
      Address,
      Constant,
      Opaque
    };

    Kind K = Kind::Opaque;
    CellId Cell{};      // CellRead, Address
    llvm::APSInt Value; // Constant
  };

  RValue classify(const clang::Expr *E, const CellUniverse &Cells,
                  clang::ASTContext &Ctx);

  class Transfer
  {
  public:
    Transfer(clang::ASTContext &Ctx, const CellUniverse &Cells,
             const EscapeInfo &Escapes);

    void apply(const clang::CFGElement &Elt, SED &State) const;

    // Whether we can analyze this function (reasons for declining: contains setjmp, computed goto, etc)
    static bool declines(const clang::FunctionDecl &FD);

  private:
    // T1: move `Dest` into the class the rvalue denotes, then detach
    // everything the store invalidates.
    void store(CellId Dest, const RValue &V, SED &State) const;

    // T2/T3: detach every cell not out of reach. A callee cannot name a
    // non-escaping local, so it cannot write one — under any aliasing,
    // through any global, via any callback; and neither can a store whose
    // destination this function could not name. Everything else, it might.
    void havocReachable(SED &State) const;

    // The weak kill of T1: detach every cell the store to `Dest`
    // invalidates, other than `Dest`, which the caller has already moved.
    // Two disjoint reasons — see Escape.h. A cell may have had its
    // *contents* rewritten (`mayOverlap`), or it may have kept its contents
    // and lost its *meaning*, because the store moved a pointer the cell's
    // path walks through (`denotationDependsOn`).
    void havocInvalidated(CellId Dest, SED &State) const;

    // T4, the blanket rule: detach the entire universe.
    void havocAll(SED &State) const;

    clang::ASTContext &Ctx;
    const CellUniverse &Cells;
    const EscapeInfo &Escapes;
  };

} // namespace xj::analysis
