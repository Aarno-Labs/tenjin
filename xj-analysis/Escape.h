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
  // int x =
  // E.g.
  //  x whose address is never taken:  out of reach.
  //  (*x) in reach, since x may have an alias.
  bool isOutOfReach(CellId C, const CellUniverse &Cells,
                    const EscapeInfo &Escapes);

  // 1. May a write to P's storage change the contents of Q's storage?
  // 2. May a write to P change _which storage_ Q _denotes_?
  // Since we track paths,
  bool mayAlias(CellId P, CellId Q, const CellUniverse &Cells,
                const EscapeInfo &Escapes);

} // namespace xj::analysis
