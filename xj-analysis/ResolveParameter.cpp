#include "ResolveParameter.h"

#include "clang/AST/RecursiveASTVisitor.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <string>

namespace xj::analysis
{

  namespace
  {

    // The syntactic pre-pass: which pointers to track, where they are read,
    // and which reads are actually writes in disguise.
    class UseCollector : public clang::RecursiveASTVisitor<UseCollector>
    {
    public:
      bool VisitDeclStmt(clang::DeclStmt *DS)
      {
        for (const clang::Decl *D : DS->decls())
          if (const auto *VD = llvm::dyn_cast<clang::VarDecl>(D))
            if (VD->getType()->isPointerType() && VD->hasLocalStorage() &&
                !VD->getName().empty() &&
                !VD->getType().isVolatileQualified())
              Locals.push_back(VD);
        return true;
      }

      bool VisitDeclRefExpr(clang::DeclRefExpr *DRE)
      {
        Refs.push_back(DRE);
        return true;
      }

      bool VisitBinaryOperator(clang::BinaryOperator *BO)
      {
        if (BO->isAssignmentOp())
          if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(
                  BO->getLHS()->IgnoreParenImpCasts()))
            Defs.insert(DRE);
        return true;
      }

      bool VisitUnaryOperator(clang::UnaryOperator *UO)
      {
        if (!UO->isIncrementDecrementOp())
          return true;
        if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(
                UO->getSubExpr()->IgnoreParenImpCasts()))
          Defs.insert(DRE);
        return true;
      }

      // A use is a read: every DeclRefExpr except the LHS occurrence of an
      // assignment and the operand of ++/--. That is what makes
      //
      //     p = t->storage;  use(p);  p = NULL;
      //
      // resolve: every *use* agrees, and deleting `p` deletes the dead
      // store with it.
      bool isDef(const clang::DeclRefExpr *DRE) const
      {
        return Defs.count(DRE) != 0;
      }

      llvm::SmallVector<const clang::VarDecl *, 8> Locals;
      llvm::SmallVector<clang::DeclRefExpr *, 32> Refs;

    private:
      llvm::DenseSet<const clang::DeclRefExpr *> Defs;
    };

    CandidateKind kindOf(const Cell &C)
    {
      if (!C.Path.empty())
        return CandidateKind::LValue;
      return llvm::isa<clang::ParmVarDecl>(C.Root) ? CandidateKind::Param
                                                   : CandidateKind::Local;
    }

    // param > local > lvalue, then shortest path, then spelling. Ties have
    // to break deterministically or the corpus run is not reproducible.
    bool preferredOver(CellId A, CellId B, const CellUniverse &Cells)
    {
      const Cell &CA = Cells.get(A);
      const Cell &CB = Cells.get(B);
      CandidateKind KA = kindOf(CA);
      CandidateKind KB = kindOf(CB);
      if (KA != KB)
        return static_cast<int>(KA) < static_cast<int>(KB);
      if (CA.Path.size() != CB.Path.size())
        return CA.Path.size() < CB.Path.size();
      return Cells.print(A) < Cells.print(B);
    }

  } // namespace

  ParameterResolver ParameterResolver::run(const clang::FunctionDecl &FD,
                                           const MustEqualAnalysis &A)
  {
    ParameterResolver R(A);

    const CellUniverse &Cells = A.cells();
    for (CellId C : Cells.ids())
      if (Cells.get(C).Root->hasLocalStorage())
        R.Candidates.push_back(C);

    UseCollector UC;
    UC.TraverseStmt(const_cast<clang::Stmt *>(FD.getBody()));
    R.Tracked = UC.Locals;

    llvm::DenseMap<const clang::Stmt *, const clang::VarDecl *> UseOf;
    for (clang::DeclRefExpr *DRE : UC.Refs)
    {
      if (UC.isDef(DRE))
        continue;
      const auto *VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl());
      if (VD == nullptr || !llvm::is_contained(R.Tracked, VD))
        continue;
      UseOf[DRE] = VD;
    }

    llvm::DenseSet<const clang::Stmt *> Seen;
    A.forEachStmt([&](const clang::Stmt *S, const SED &State)
                  {
      auto It = UseOf.find(S);
      if (It == UseOf.end())
        return;
      Seen.insert(S);
      R.Sites[It->second].push_back(
          R.factAt(It->second, llvm::cast<clang::DeclRefExpr>(S), State)); });

    // A use the walk never reached is either in an unreachable block — in
    // which case it is skipped, because dead code must not veto a good fact
    // — or something the CFG did not surface as an element, which we cannot
    // vouch for and so record as disagreement.
    for (clang::DeclRefExpr *DRE : UC.Refs)
    {
      auto It = UseOf.find(DRE);
      if (It == UseOf.end() || Seen.count(DRE) != 0)
        continue;
      SiteFact F;
      F.Use = DRE;
      F.Loc = DRE->getBeginLoc();
      F.Reachable = A.isReachable(DRE);
      R.Sites[It->second].push_back(std::move(F));
    }

    return R;
  }

  SiteFact ParameterResolver::factAt(const clang::VarDecl *V,
                                     const clang::DeclRefExpr *Use,
                                     const SED &State) const
  {
    SiteFact F;
    F.Use = Use;
    F.Loc = Use->getBeginLoc();
    F.Reachable = true;

    auto Self = A->cells().lookup(V);
    if (!Self)
      return F;
    F.Value = State.labelOf(*Self);

    // `membersOf(*Self)` minus itself, intersected with the candidate set —
    // spelled as a walk over the candidates so that `Agreeing` comes back
    // in candidate order. Note what is *not* here: no test that the class
    // carries a label. An unlabelled class is a perfectly good equality,
    // and demanding a label would be the old T test in a new costume.
    for (CellId C : Candidates)
      if (C != *Self && State.sameClass(C, *Self))
        F.Agreeing.push_back(C);
    return F;
  }

  std::optional<Resolution>
  ParameterResolver::resolve(const clang::VarDecl *B,
                             llvm::function_ref<bool(CellId)> Accept) const
  {
    // `&p` reads the variable's *storage*, not its value, so a cursor whose
    // address is taken is not substitutable however well its value agrees:
    // rewriting `take(&p)` to `take(&buf)` hands out the address of a
    // different object, and the cursor cannot be deleted while something
    // still points at it. This is the distinction `UseCollector` draws for
    // the LHS of an assignment, one level up — there it decides what counts
    // as a site, here it decides whether any number of agreeing sites can be
    // believed.
    if (A->escapes().addressTaken(B))
      return std::nullopt;

    auto It = Sites.find(B);
    if (It == Sites.end() || It->second.empty())
      return std::nullopt; // zero uses is not vacuous truth

    llvm::SmallVector<CellId, 4> Agreed;
    unsigned Count = 0;
    bool First = true;

    for (const SiteFact &F : It->second)
    {
      if (!F.Reachable)
        continue; // dead code does not veto a good fact
      ++Count;
      if (First)
      {
        Agreed.assign(F.Agreeing.begin(), F.Agreeing.end());
        First = false;
        continue;
      }
      // Intersection
      Agreed.erase(std::remove_if(Agreed.begin(), Agreed.end(),
                                  [&](CellId C)
                                  {
                                    return !llvm::is_contained(F.Agreeing, C);
                                  }),
                   Agreed.end());
    }

    // Narrowing after the fold rather than at each site: `Accept` is a
    // property of the cell, so filtering commutes with the intersection.
    Agreed.erase(std::remove_if(Agreed.begin(), Agreed.end(),
                                [&](CellId C)
                                { return !Accept(C); }),
                 Agreed.end());

    if (Count == 0 || Agreed.empty())
      return std::nullopt;

    CellId Best = Agreed.front();
    for (CellId C : Agreed)
      if (preferredOver(C, Best, A->cells()))
        Best = C;

    Resolution R;
    R.Cell = Best;
    R.Kind = kindOf(A->cells().get(Best));
    R.Sites = Count;

    // Root-anchored: the class holds the parameter's *incoming* value, so
    // the pointer equals the caller's argument and not merely whatever the
    // parameter holds at the use. `InitOf` is the label only the entry
    // state can produce, which is what makes this a check on the label
    // rather than a separate capture of the entry state.
    //
    // Every reachable site has to carry it, not just the first: the label
    // can be lost at one site and re-established at another only for
    // `AddrOf` and `Const`, never for `InitOf`, but the quantifier is the
    // same one `Agreed` is folded under and it costs one pass to be exact.
    if (R.Kind == CandidateKind::Param)
    {
      const Label Anchor = Label::initOf(Best);
      R.EntryAnchored = true;
      for (const SiteFact &F : It->second)
        if (F.Reachable && F.Value != Anchor)
          R.EntryAnchored = false;
    }
    return R;
  }

  llvm::ArrayRef<SiteFact>
  ParameterResolver::sitesFor(const clang::VarDecl *B) const
  {
    auto It = Sites.find(B);
    if (It == Sites.end())
      return {};
    return It->second;
  }

  Justification ParameterResolver::explain(const clang::VarDecl *B) const
  {
    Justification J;
    llvm::ArrayRef<SiteFact> Facts = sitesFor(B);
    J.SitesTotal = Facts.size();
    for (const SiteFact &F : Facts)
    {
      if (!F.Reachable)
      {
        ++J.SitesUnreachable;
        continue;
      }
      if (!F.Agreeing.empty())
        ++J.SitesAgreed;
    }

    std::string S;
    llvm::raw_string_ostream OS(S);
    if (auto R = resolve(B))
      OS << "same class at " << R->Sites << "/" << J.SitesTotal
         << " sites: " << A->cells().print(R->Cell);
    else if (A->escapes().addressTaken(B))
      OS << "address taken: not substitutable at any number of sites";
    else if (J.SitesTotal == 0)
      OS << "no use sites";
    else
      OS << "no candidate agreed at all " << J.SitesTotal << " sites";
    J.Summary = OS.str();
    return J;
  }

} // namespace xj::analysis
