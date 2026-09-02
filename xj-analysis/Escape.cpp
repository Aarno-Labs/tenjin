#include "Escape.h"

#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"

#include <algorithm>

namespace xj::analysis
{

  namespace
  {

    class EscapeCollector : public clang::RecursiveASTVisitor<EscapeCollector>
    {
    public:
      explicit EscapeCollector(llvm::DenseSet<const clang::VarDecl *> &Out)
          : Out(Out) {}

      bool VisitUnaryOperator(clang::UnaryOperator *UO)
      {
        if (UO->getOpcode() == clang::UO_AddrOf)
          add(UO->getSubExpr());
        return true;
      }

      bool VisitImplicitCastExpr(clang::ImplicitCastExpr *ICE)
      {
        // An array decays to a pointer to its first element, which hands
        // the object's address to whoever receives it. That is an escape as
        // surely as `&x` is.
        if (ICE->getCastKind() == clang::CK_ArrayToPointerDecay)
          add(ICE->getSubExpr());
        return true;
      }

    private:
      void add(const clang::Expr *E)
      {
        // Walk out to the variable whose storage this names. `&x.buf` hands
        // out a pointer *into* `x`, so it escapes `x` as surely as `&x`
        // does — and without this, `x.buf` stays out of reach at every call
        // while a callee holds a pointer to it.
        //
        // An arrow stops the walk. `&t->storage` names storage inside the
        // pointee, not inside `t`, and nothing a callee does with it can
        // write the variable `t`. The cell `t->storage` needs no help from
        // this rule: a path with a Deref is never out of reach anyway.
        while (E != nullptr)
        {
          E = E->IgnoreParenImpCasts();
          const auto *ME = llvm::dyn_cast<clang::MemberExpr>(E);
          if (ME == nullptr || ME->isArrow())
            break;
          E = ME->getBase();
        }
        if (E == nullptr)
          return;
        if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(E))
          if (const auto *VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl()))
            Out.insert(VD->getCanonicalDecl());
      }

      llvm::DenseSet<const clang::VarDecl *> &Out;
    };

  } // namespace

  EscapeInfo::EscapeInfo() = default;

  EscapeInfo EscapeInfo::compute(const clang::FunctionDecl &FD)
  {
    EscapeInfo E;
    if (const clang::Stmt *Body = FD.getBody())
    {
      EscapeCollector C(E.AddressTaken);
      C.TraverseStmt(const_cast<clang::Stmt *>(Body));
    }
    return E;
  }

  bool EscapeInfo::addressTaken(const clang::VarDecl *V) const
  {
    return V != nullptr && AddressTaken.count(V->getCanonicalDecl()) != 0;
  }

  bool EscapeInfo::escapes(const clang::VarDecl *V) const
  {
    if (V == nullptr)
      return true;
    // A parameter is a local: it has automatic storage, and nothing outside
    // the function can name it. `hasLocalStorage` is false for globals,
    // static locals and externs, all of which anyone may reach.
    if (!V->hasLocalStorage())
      return true;
    return addressTaken(V);
  }

  bool isOutOfReach(CellId C, const CellUniverse &Cells,
                    const EscapeInfo &Escapes)
  {
    const Cell &Cl = Cells.get(C);
    if (Escapes.escapes(Cl.Root))
      return false;
    // A path with a Deref names storage somewhere else entirely, and the
    // locality of the pointer says nothing about the locality of its
    // pointee.
    for (const Step &S : Cl.Path)
      if (S.kind() == Step::Kind::Deref)
        return false;
    return true;
  }

  namespace
  {
    // Do the steps of `Path` from `From` onwards leave the storage the path
    // had reached by then, by going out through a pointer?
    bool tailContainsDeref(llvm::ArrayRef<Step> Path, unsigned From)
    {
      for (unsigned I = From, N = Path.size(); I != N; ++I)
        if (Path[I].kind() == Step::Kind::Deref)
          return true;
      return false;
    }
  } // namespace

  bool mayOverlap(CellId P, CellId Q, const CellUniverse &Cells,
                  const EscapeInfo &Escapes)
  {
    if (P == Q)
      return true;

    const Cell &A = Cells.get(P);
    const Cell &B = Cells.get(Q);

    // We are writing into cell P. Can this change the value stored in Q?
    // Not if Q is out of reach (couldn't have reached P) and vice versa.
    if (A.Root != B.Root)
      return (!isOutOfReach(P, Cells, Escapes) &&
              !isOutOfReach(Q, Cells, Escapes));

    // Same root: walk the shared prefix.
    const unsigned N = std::min(A.Path.size(), B.Path.size());
    unsigned I = 0;
    while (I != N && A.Path[I] == B.Path[I])
      ++I;

    if (I == N)
    {
      // same root, one path a prefix of the other.
      // Suppose we have x.e1 = ...;
      // When might this touch x.e2.e3?

      // One path is a prefix of the other. Cells are interned, so `P != Q`
      // means one of them is strictly longer.
      const bool AIsShorter = A.Path.size() < B.Path.size();
      const CellId Shorter = AIsShorter ? P : Q;
      const Cell &Longer = AIsShorter ? B : A;

      // The extra steps stay inside the shorter cell, so the
      // longer one is a sub-object of it and a write to either reaches the
      // other: `x` and `x.buf`, whatever either address has done.
      if (!tailContainsDeref(Longer.Path, I))
        return true;

      // The extra steps leave through a pointer, so if the shorter
      // path is out of reach, it can not be the target of the longer
      // path's dereference.
      return !isOutOfReach(Shorter, Cells, Escapes);
    }

    // They diverge. Distinct members of one struct do not overlap.
    // Union members are excluded deliberately: that is where type punning
    // lives, and two members of a union share storage by construction.
    const Step &SA = A.Path[I];
    const Step &SB = B.Path[I];
    if (SA.kind() == Step::Kind::Field && SB.kind() == Step::Kind::Field &&
        SA.field() != SB.field() &&
        SA.field()->getParent() == SB.field()->getParent() &&
        SA.field()->getParent()->isStruct())
      return false;

    return true;
  }

  bool denotationDependsOn(CellId C, CellId Stored, const CellUniverse &Cells)
  {
    if (C == Stored)
      return false;

    const Cell &Cl = Cells.get(C);
    const Cell &S = Cells.get(Stored);

    // Only storage a path actually walks through can move it, and a path
    // walks through nothing but its own proper prefixes — which share its
    // root. A store that merely *overlaps* such a prefix moves it too, and
    // that case is left to `mayOverlap`: the pair is either two cells with
    // different roots, both necessarily in reach, or two union members.
    if (Cl.Root != S.Root || S.Path.size() >= Cl.Path.size())
      return false;
    for (unsigned I = 0, N = S.Path.size(); I != N; ++I)
      if (S.Path[I] != Cl.Path[I])
        return false;

    // Reaching `C` from `Stored` has to go out through a pointer for the
    // store to move anything. If the remaining steps are all fields then
    // `C` sits at a fixed offset inside `Stored`, and the store changes
    // what `C` holds rather than where `C` is — `mayOverlap`'s containment
    // case, and not this one.
    return tailContainsDeref(Cl.Path, S.Path.size());
  }

} // namespace xj::analysis
