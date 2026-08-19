// Escape, out-of-reach, and may-alias — the memory reasoning.
//
// Every *negative* judgment the analysis makes comes from here, and all
// of it is syntactic: no clause consults the dataflow state, and — the
// invariant to check any future rule against — no clause concludes
// disjointness from two cells merely being distinct. Negative answers come
// from escape (nobody else can name the object) or from C's object model
// (distinct members of one struct do not overlap).
//
// With `CellUniverse::collect(..., /*MemoryCandidates=*/false)` every path
// is empty and all three functions collapse to something trivial, which is
// why M1 is plain copy propagation and M2 is the same analysis over a
// bigger alphabet.
//
// See base_resolution_domain_design.md §2.

#pragma once

#include "Cell.h"

#include "llvm/ADT/DenseSet.h"

namespace xj::analysis
{

  // E1. A variable escapes if the *text* of the function lets anyone else
  // name it. Nothing here is a dataflow fact.
  class EscapeInfo
  {
  public:
    static EscapeInfo compute(const clang::FunctionDecl &FD);

    // `&V` appears anywhere in the body.
    bool addressTaken(const clang::VarDecl *V) const;
    // Address taken, or not a local at all: global, static local, extern.
    bool escapes(const clang::VarDecl *V) const;

  private:
    EscapeInfo();

    llvm::DenseSet<const clang::VarDecl *> AddressTaken;
  };

  // E2. Nothing outside this function can name the cell: its root is a
  // non-escaping local *and* its path contains no Deref step. Both halves
  // are required —
  //
  //     struct S x;  char *p = x.buf;   g();  // x.buf: out of reach
  //     struct S *q; char *p = q->buf;  g();  // q->buf: not, the pointee
  //                                           // is somebody else's object
  //
  // A deref-free path rooted at a non-escaping local names storage inside
  // that local; a path with a deref names storage somewhere else entirely,
  // and the locality of the pointer says nothing about its pointee.
  bool isOutOfReach(CellId C, const CellUniverse &Cells,
                    const EscapeInfo &Escapes);

  // E3. May the two cells denote overlapping storage?
  //
  //   P == Q                                        -> true
  //   different roots                               -> !(outOfReach(P) ||
  //                                                      outOfReach(Q))
  //   same root, one path a prefix of the other      -> true
  //   same root, diverge at distinct struct fields   -> false
  //   otherwise (union members, anything else)       -> true
  //
  // Only the struct clause buys anything, and it buys a lot: `t->len = n`
  // no longer kills `t->storage`, because the shared prefix `Var(t).Deref`
  // names one object on any execution and distinct members of one struct
  // do not overlap. That is a fact about C's object model, not an
  // assumption — and it is precision the clang::dataflow route could not
  // have had, where cells are opaque locations whose distinctness proves
  // nothing.
  //
  // Union members alias, which is where the punning rule lives. The rest
  // of punning — stores through `char *`, `void *`, or a cast — needs no
  // rule, because such a destination has no cell and is handled as an
  // unresolvable store.
  bool mayAlias(CellId P, CellId Q, const CellUniverse &Cells,
                const EscapeInfo &Escapes);

} // namespace xj::analysis
