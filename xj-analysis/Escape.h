// Memory queries for MustEquals

#pragma once

#include "Cell.h"

#include "llvm/ADT/DenseSet.h"

namespace xj::analysis
{

  // A variable escapes if the *text* of the function lets anyone else
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

  // The cell can not be modified except through a path with the same root
  // or: can not be modified via a pointer dereference.
  // E.g.
  //  x whose address is never taken:  out of reach.
  //  (*x) in reach, since x may have an alias.
  bool isOutOfReach(CellId C, const CellUniverse &Cells,
                    const EscapeInfo &Escapes);

  // A store invalidates a cell two ways, and they are different questions
  // with different answers. `mayOverlap` is the first, `denotationDependsOn`
  // the second; T1's weak kill is their disjunction.

  // Can a write to P change the value stored in Q?
  bool mayOverlap(CellId P, CellId Q, const CellUniverse &Cells,
                  const EscapeInfo &Escapes);

  // 2. May a write to `Stored` change _which storage_ `C` _denotes_?
  //
  // A cell is a syntactic path, so what it names is a function of the
  // pointers it walks through. After `t = u`, `t->storage` names a field of
  // a different object although not one byte of the old object moved: the
  // recorded equality was about storage the path no longer reaches, so `C`
  // must leave its class. Nothing overlapped anything.
  //
  // True when `Stored` is a proper prefix of `C` and the steps below it
  // pass through at least one Deref. Deliberately no escape test — this is
  // not an aliasing question, and it holds whether or not anything in sight
  // is addressable.
  bool denotationDependsOn(CellId C, CellId Stored, const CellUniverse &Cells);

} // namespace xj::analysis
