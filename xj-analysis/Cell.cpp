#include "Cell.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"

#include "llvm/Support/raw_ostream.h"

#include <utility>

namespace xj::analysis
{

  Step Step::deref()
  {
    return Step(Kind::Deref, nullptr);
  }

  Step Step::field(const clang::FieldDecl *F)
  {
    return Step(Kind::Field, F);
  }

  bool operator==(Step A, Step B)
  {
    return A.K == B.K && A.F == B.F;
  }

  bool operator==(const Cell &A, const Cell &B)
  {
    if (A.Root != B.Root || A.Path.size() != B.Path.size())
    {
      return false;
    }
    for (unsigned I = 0, N = A.Path.size(); I != N; ++I)
    {
      if (A.Path[I] != B.Path[I])
      {
        return false;
      }
    }
    return true;
  }

  CellUniverse::CellUniverse() = default;

  CellId CellUniverse::intern(const Cell &C)
  {
    if (C.Path.empty())
    {
      auto It = ByVar.find(C.Root);
      if (It != ByVar.end())
      {
        return id(It->second);
      }
      unsigned Index = Cells.size();
      Cells.push_back(C);
      ByVar[C.Root] = Index;
      return id(Index);
    }
    for (unsigned I = 0, N = Cells.size(); I != N; ++I)
      if (Cells[I] == C)
        return id(I);
    Cells.push_back(C);
    return id(Cells.size() - 1);
  }

  namespace
  {

    // The cell an expression names, or nullopt when this domain cannot name
    // it. array subscripts, call results, casts between unrelated types,
    // paths over the depth cap, anything not rooted at a `VarDecl`.
    //
    // Must agree with `lookup`
    std::optional<Cell> cellOf(const clang::Expr *E)
    {
      if (E == nullptr)
        return std::nullopt;
      E = E->IgnoreParens();

      if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(E))
      {
        const auto *VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl());
        if (VD == nullptr || VD->getName().empty() ||
            VD->getType().isVolatileQualified())
          return std::nullopt;
        Cell C;
        // A global can have several declarations; key on the canonical one
        // so that two references to it land in the same cell.
        C.Root = VD->getCanonicalDecl();
        return C;
      }

      if (const auto *ME = llvm::dyn_cast<clang::MemberExpr>(E))
      {
        // An anonymous struct or union member arrives as an
        // IndirectFieldDecl, which names storage this path grammar cannot
        // spell.
        const auto *FD = llvm::dyn_cast<clang::FieldDecl>(ME->getMemberDecl());
        if (FD == nullptr || FD->getType().isVolatileQualified())
          return std::nullopt;
        // The base of `t->f` carries an lvalue-to-rvalue conversion, which
        // does not change which object is named.
        std::optional<Cell> C = cellOf(ME->getBase()->IgnoreParenImpCasts());
        if (!C)
          return std::nullopt;
        if (ME->isArrow())
          C->Path.push_back(Step::deref());
        C->Path.push_back(Step::field(FD));

        if (C->Path.size() > CellUniverse::MaxPathDepth)
          return std::nullopt;
        return C;
      }

      return std::nullopt;
    }

    class CellCollector : public clang::RecursiveASTVisitor<CellCollector>
    {
    public:
      CellCollector(llvm::SmallVectorImpl<Cell> &Out)
          : Out(Out) {}

      bool VisitDeclStmt(clang::DeclStmt *DS)
      {
        for (const clang::Decl *D : DS->decls())
          if (const auto *VD = llvm::dyn_cast<clang::VarDecl>(D))
            addVar(VD);
        return true;
      }

      bool VisitDeclRefExpr(clang::DeclRefExpr *DRE)
      {
        if (const auto *VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl()))
          addVar(VD);
        return true;
      }

      // Every `x.f` and `x->f` the body mentions.
      bool VisitMemberExpr(clang::MemberExpr *ME)
      {
        if (auto C = cellOf(ME))
          Out.push_back(std::move(*C));
        return true;
      }

    private:
      void addVar(const clang::VarDecl *VD)
      {
        if (VD == nullptr || VD->getName().empty() ||
            VD->getType().isVolatileQualified())
          return;
        Out.push_back(Cell{VD->getCanonicalDecl(), {}});
      }

      llvm::SmallVectorImpl<Cell> &Out;
    };

  } // namespace

  CellUniverse CellUniverse::collect(clang::ASTContext &Ctx,
                                     const clang::FunctionDecl &FD)
  {
    (void)Ctx;

    CellUniverse U;

    // Parameters first, so that ids are stable and parameters sort ahead of
    // locals when a resolution has to pick a preferred candidate.
    for (const clang::ParmVarDecl *P : FD.parameters())
      if (!P->getName().empty() && !P->getType().isVolatileQualified())
        U.intern(Cell{P, {}});

    llvm::SmallVector<Cell, 16> Mentioned;
    if (const clang::Stmt *Body = FD.getBody())
    {
      CellCollector CC(Mentioned);
      CC.TraverseStmt(const_cast<clang::Stmt *>(Body));
    }
    for (const Cell &C : Mentioned)
      U.intern(C);

    return U;
  }

  std::optional<CellId> CellUniverse::lookup(const clang::Expr *E) const
  {
    std::optional<Cell> C = cellOf(E);
    if (!C)
      return std::nullopt;
    if (C->Path.empty())
      return lookup(C->Root);
    // A nameable path the alphabet does not contain cannot arise from a body
    // `collect` walked, but returning nullopt is the sound answer if it ever
    // does: the store becomes unresolvable rather than silently untracked.
    for (unsigned I = 0, N = Cells.size(); I != N; ++I)
      if (Cells[I] == *C)
        return id(I);
    return std::nullopt;
  }

  std::optional<CellId> CellUniverse::lookup(const clang::VarDecl *V) const
  {
    if (V == nullptr)
      return std::nullopt;
    auto It = ByVar.find(V->getCanonicalDecl());
    if (It == ByVar.end())
      return std::nullopt;
    return id(It->second);
  }

  const Cell &CellUniverse::get(CellId C) const
  {
    return Cells[index(C)];
  }

  clang::QualType CellUniverse::typeOf(CellId C) const
  {
    const Cell &Cl = get(C);
    clang::QualType T = Cl.Root->getType();
    for (const Step &S : Cl.Path)
      T = S.kind() == Step::Kind::Deref ? T->getPointeeType()
                                        : S.field()->getType();
    return T;
  }

  std::string CellUniverse::print(CellId C) const
  {
    const Cell &Cl = get(C);
    std::string S = Cl.Root->getName().str();
    // A Deref immediately followed by a Field prints as `->`; a bare Deref
    // prints as a prefix `*`, which needs the prefix parenthesized.
    for (unsigned I = 0, N = Cl.Path.size(); I != N; ++I)
    {
      if (Cl.Path[I].kind() == Step::Kind::Deref)
      {
        if (I + 1 < N && Cl.Path[I + 1].kind() == Step::Kind::Field)
        {
          S += "->";
          S += Cl.Path[I + 1].field()->getName();
          ++I;
        }
        else
        {
          S = "(*" + S + ")";
        }
        continue;
      }
      S += ".";
      S += Cl.Path[I].field()->getName();
    }
    return S;
  }

  unsigned CellUniverse::size() const
  {
    return Cells.size();
  }

} // namespace xj::analysis
