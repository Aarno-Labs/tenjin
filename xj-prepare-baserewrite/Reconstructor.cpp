// `run` walks the translation unit and hands every non-system function
// definition to `reconstructFunction`, which is where all of it lives:
//
//   1. Look the function up in the side-file by `functionKey`, and skip it
//      if an earlier TU already rewrote it.
//
//   2. Run `MustEqualAnalysis` over the body.
//
//   3. Collect candidates: pointers the analysis tracks *and* the pointer
//      pass recorded, matched on the decl position in the record and
//      cross-checked against the recorded name. Two sets matter here and
//      they are not the same one:
//
//        `Candidates`      may be deleted (recorded, so it has an index var)
//        `R.tracked()`     may be substituted (proved, whoever recorded it)
//
//      The analysis tracks every pointer local, including ones the pointer
//      pass left alone, and those make perfectly good bases. That gap is
//      what step 4a and step 5 are about.
//
//   4. Plan each candidate. Either half may decline, and the reason is
//      reported rather than swallowed:
//
//        a. `resolve` it to a cell, preferring one this run will not
//           itself delete. When every agreeing cell is a casualty, take
//           the library's own pick and leave it to step 5.
//        b. `checkSubstitutable` — the syntactic side conditions the proof
//           says nothing about: macro expansions, `sizeof`, `p += n`,
//           side-effecting stores, stores in positions that cannot be
//           deleted on their own, and a base whose spelling something else
//           in the function shadows.
//
//   5. Backstop for 4a: drop any surviving plan whose base is rooted at a
//      pointer another surviving plan deletes. Iterated to a fixpoint,
//      which terminates because dropping is monotone.
//
//   6. `buildEdits` per plan, in *original* file offsets: delete the
//      declaration, delete each store's comma arm, fold the index forms,
//      then substitute the base at every remaining mention. A plan with an
//      edit outside the function's own file is dropped whole rather than
//      half-applied.
//
//   7. `applyEdits` once for the function: outermost edit wins over any it
//      overlaps, and edits land back-to-front so earlier offsets stay
//      valid.
//
//   8. Write the proved base back into the side-file, for the committed
//      plans only, so an empty `base_text` still means "its own base".
//
// Steps 4b and 6 share one notion of what a store to the pointer looks
// like: the left arm of the comma the pointer pass emits, recognized in
// `checkSubstitutable` and recorded on the `Plan` for `buildEdits` to
// delete. That is deliberate. They used to recognize it separately, and
// `checkSubstitutable` admitted a shape — a store that is a statement of
// its own — that `buildEdits` did not delete, so the base was substituted
// into the store's left-hand side and `p = NULL;` silently became
// `t->storage = NULL;`.

#include "Reconstructor.h"

#include "FunctionKey.h"

#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <utility>

using namespace clang;

namespace xj
{
  namespace
  {

    // The variable an expression names, stripped of the parens and implicit
    // conversions that carry no meaning here.
    const VarDecl *bareVar(const Expr *E)
    {
      if (E == nullptr)
        return nullptr;
      const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
      return DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
    }

    std::string textOf(const Expr *E, ASTContext &Ctx)
    {
      return Lexer::getSourceText(
                 CharSourceRange::getTokenRange(E->getSourceRange()),
                 Ctx.getSourceManager(), Ctx.getLangOpts())
          .str();
    }

    // Either end inside a macro expansion is enough to disqualify: neither
    // end can be rewritten, and the two need not agree.
    bool inMacro(const Stmt *S)
    {
      return S->getBeginLoc().isMacroID() || S->getEndLoc().isMacroID();
    }

    bool mentionsVar(const Stmt *S, const VarDecl *V)
    {
      if (S == nullptr)
        return false;
      if (const auto *DRE = dyn_cast<DeclRefExpr>(S))
        if (dyn_cast<VarDecl>(DRE->getDecl()) == V)
          return true;
      for (const Stmt *C : S->children())
        if (mentionsVar(C, V))
          return true;
      return false;
    }

    // `(p + p_index_xj)` — the form the pointer pass emits wherever the
    // pointer's *value* is read rather than an element of it.
    bool isBasePlusIndex(const Expr *E, const VarDecl *Ptr, const VarDecl *Idx)
    {
      if (E == nullptr)
        return false;
      const auto *BO = dyn_cast<BinaryOperator>(E->IgnoreParenImpCasts());
      return BO != nullptr && BO->getOpcode() == BO_Add &&
             bareVar(BO->getLHS()) == Ptr && bareVar(BO->getRHS()) == Idx;
    }

    // The single parent statement of `S`, looking through parentheses.
    const Stmt *parentThroughParens(const Stmt *S, ASTContext &Ctx)
    {
      while (S != nullptr)
      {
        DynTypedNodeList Parents = Ctx.getParents(*S);
        if (Parents.size() != 1)
          return nullptr;
        const Stmt *P = Parents[0].get<Stmt>();
        if (P == nullptr || !isa<ParenExpr>(P))
          return P;
        S = P;
      }
      return nullptr;
    }

    // Everything about one pointer's occurrences that bears on whether it
    // can be reconstructed, gathered in a single walk of the body.
    class RefCollector : public RecursiveASTVisitor<RefCollector>
    {
    public:
      RefCollector(const VarDecl *V, ASTContext &Ctx) : V(V), Ctx(Ctx) {}

      const VarDecl *V;
      ASTContext &Ctx;

      std::vector<const DeclRefExpr *> Refs;
      std::vector<const BinaryOperator *> Assigns;
      const DeclStmt *Decl = nullptr;

      bool InMacro = false;
      bool MultiDeclarator = false;
      // A use whose value depends on the pointer's *type* rather than its
      // value: `sizeof p` is not `sizeof buf` when the base is an array.
      bool Unevaluated = false;
      // The pointer's own storage is written by something other than a
      // plain `p = RHS`, so no substitution can stand in for it.
      bool Mutated = false;
      // Deleting a store means deleting its right-hand side, which is only
      // safe when evaluating it did nothing else.
      bool SideEffectingStore = false;

      bool VisitDeclRefExpr(DeclRefExpr *DRE)
      {
        if (dyn_cast<VarDecl>(DRE->getDecl()) != V)
          return true;
        Refs.push_back(DRE);
        InMacro |= inMacro(DRE);
        return true;
      }

      bool VisitDeclStmt(DeclStmt *DS)
      {
        if (!llvm::is_contained(DS->decls(), V))
          return true;
        Decl = DS;
        MultiDeclarator |= !DS->isSingleDecl();
        InMacro |= inMacro(DS);
        if (const Expr *Init = V->getInit())
          SideEffectingStore |= Init->HasSideEffects(Ctx);
        return true;
      }

      bool VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *E)
      {
        if (!E->isArgumentType() && mentionsVar(E->getArgumentExpr(), V))
          Unevaluated = true;
        return true;
      }

      bool VisitBinaryOperator(BinaryOperator *BO)
      {
        if (!BO->isAssignmentOp() || bareVar(BO->getLHS()) != V)
          return true;
        // `p += n` moves the pointer itself. The pointer pass rewrites
        // those onto the index, so one here is something it left alone.
        if (BO->getOpcode() != BO_Assign)
        {
          Mutated = true;
          return true;
        }
        Assigns.push_back(BO);
        SideEffectingStore |= BO->getRHS()->HasSideEffects(Ctx);
        return true;
      }

      bool VisitUnaryOperator(UnaryOperator *UO)
      {
        if (UO->isIncrementDecrementOp() && bareVar(UO->getSubExpr()) == V)
          Mutated = true;
        return true;
      }
    };

    // Every variable `FD` declares: its parameters, then the locals in its
    // body. Parameters are visited by hand because traversing the body does
    // not reach them.
    void forEachVarDecl(const FunctionDecl &FD,
                        llvm::function_ref<void(const VarDecl *)> F)
    {
      class Walker : public RecursiveASTVisitor<Walker>
      {
      public:
        explicit Walker(llvm::function_ref<void(const VarDecl *)> F) : F(F) {}

        bool VisitVarDecl(VarDecl *VD)
        {
          F(VD);
          return true;
        }

      private:
        llvm::function_ref<void(const VarDecl *)> F;
      } W(F);

      for (const ParmVarDecl *P : FD.parameters())
        F(P);
      W.TraverseStmt(const_cast<Stmt *>(FD.getBody()));
    }

    // Is any variable other than `Root` in this function spelled the same?
    // The substituted text is a *spelling*, so a shadowing declaration
    // between the proof and the use would silently rebind it.
    bool rootNameIsAmbiguous(const FunctionDecl &FD, const VarDecl *Root)
    {
      const VarDecl *Canonical = Root->getCanonicalDecl();
      bool Ambiguous = false;
      forEachVarDecl(FD, [&](const VarDecl *VD)
                     {
        if (VD->getName() == Root->getName() &&
            VD->getCanonicalDecl() != Canonical)
          Ambiguous = true; });
      return Ambiguous;
    }

    const VarDecl *findLocalVarNamed(const FunctionDecl &FD, StringRef Name)
    {
      const VarDecl *Found = nullptr;
      forEachVarDecl(FD, [&](const VarDecl *VD)
                     {
        if (Found == nullptr && !isa<ParmVarDecl>(VD) && VD->getName() == Name)
          Found = VD; });
      return Found;
    }

    // Widen [Begin, End) to the whole line when the rest of that line is
    // blank, so a deleted declaration leaves no empty line behind.
    //
    // Done here, in original offsets, rather than with the Rewriter's
    // RemoveLineIfEmpty: see the note on Reconstructor::Edit for why that
    // option cannot be mixed with other edits.
    void expandToWholeLine(llvm::StringRef Text, unsigned &Begin, unsigned &End)
    {
      unsigned LineStart = Begin;
      while (LineStart > 0 && Text[LineStart - 1] != '\n')
      {
        if (!isHorizontalWhitespace(Text[LineStart - 1]))
          return; // something else shares the line
        --LineStart;
      }
      unsigned LineEnd = End;
      while (LineEnd < Text.size() && Text[LineEnd] != '\n')
      {
        if (!isHorizontalWhitespace(Text[LineEnd]))
          return;
        ++LineEnd;
      }
      Begin = LineStart;
      End = LineEnd < Text.size() ? LineEnd + 1 : LineEnd;
    }

  } // namespace

  void Reconstructor::run(ASTContext &Ctx)
  {
    class DefCollector : public RecursiveASTVisitor<DefCollector>
    {
    public:
      std::vector<const FunctionDecl *> Defs;

      bool VisitFunctionDecl(FunctionDecl *FD)
      {
        if (FD->doesThisDeclarationHaveABody() && FD->getBody() != nullptr)
          Defs.push_back(FD);
        return true;
      }
    } C;
    C.TraverseDecl(Ctx.getTranslationUnitDecl());

    SourceManager &SM = Ctx.getSourceManager();
    for (const FunctionDecl *FD : C.Defs)
    {
      if (SM.isInSystemHeader(FD->getLocation()))
        continue;
      reconstructFunction(*FD, Ctx);
    }
  }

  void Reconstructor::reconstructFunction(const FunctionDecl &FD,
                                          ASTContext &Ctx)
  {
    SourceManager &SM = Ctx.getSourceManager();
    const std::string Key = functionKey(&FD, SM);

    // Every way out of this function is a decline, and each one names the
    // pointer, the base it would have used (if it got that far) and why.
    auto decline = [&](unsigned &Counter, llvm::StringRef Name,
                       llvm::StringRef Base, llvm::StringRef Why)
    {
      ++Counter;
      if (Verbose)
        llvm::errs() << "[declined] " << Key << " '" << Name << "'"
                     << (Base.empty() ? "" : " -> ") << Base << ": " << Why
                     << "\n";
    };

    // 1. Should we run on this function?
    auto It = Meta.functions.find(Key);
    if (It == Meta.functions.end())
      return;
    if (!Handled.insert(Key).second)
      return;

    std::vector<PtrIndexPointerRecord> &Ptrs = It->second.pointers;

    // Records are matched on the position the pointer pass recorded, which
    // it mapped through its own Rewriter so that it names this file. A bare
    // name would be ambiguous under shadowing.
    std::map<std::pair<int, int>, size_t> ByPos;
    for (size_t I = 0; I != Ptrs.size(); ++I)
      if (Ptrs[I].decl_line != 0)
        ByPos[{Ptrs[I].decl_line, Ptrs[I].decl_col}] = I;
    if (ByPos.empty())
      return;

    // 2. Run must-equals
    auto A = analysis::MustEqualAnalysis::run(Ctx, FD);
    if (!A)
    {
      llvm::consumeError(A.takeError());
      ++Stats.FunctionsDeclined;
      return;
    }

    auto Resolver = analysis::ParameterResolver::run(FD, *A);

    // 3. The pointers this run may delete: those the analysis tracks *and*
    // the pointer pass recorded.

    // Pairs of <V, I> s.t. V is the Decl corresponding to the record
    // with index I.
    llvm::SmallVector<std::pair<const VarDecl *, size_t>, 8> Candidates;
    llvm::SmallPtrSet<const VarDecl *, 8> Reconstructable;
    for (const VarDecl *V : Resolver.tracked())
    {
      SourceLocation Loc = V->getLocation();
      auto PosIt =
          ByPos.find({static_cast<int>(SM.getSpellingLineNumber(Loc)),
                      static_cast<int>(SM.getSpellingColumnNumber(Loc))});
      if (PosIt == ByPos.end())
        continue; // not a pointer this pipeline rewrote
      // Cross-check the mapped position against the name it was recorded
      // under. A wrong mapping is the one silent failure mode here, and it
      // would otherwise reconstruct against the wrong base.
      const PtrIndexPointerRecord &Rec = Ptrs[PosIt->second];
      if (Rec.name != V->getNameAsString() || Rec.index_var.empty())
        continue;
      Candidates.push_back({V, PosIt->second});
      Reconstructable.insert(V->getCanonicalDecl());
    }

    // 4. One plan per candidate that survives both halves.
    std::vector<Plan> Plans;
    for (const auto &[V, RecordIndex] : Candidates)
    {
      PtrIndexPointerRecord &Rec = Ptrs[RecordIndex];
      ++Stats.Considered;

      // 4.
      // Prefer a base that cannot itself be reconstructed away.
      // `resolve` knows nothing about which pointers this run intends to
      // delete, so its preferred candidate can be one of them: in
      //
      //     cp_pixel_t *pix = img->pix;
      //     cp_pixel_t *a = pix;  cp_pixel_t *b = pix;
      //
      // `a`, `b` and `pix` share a class, `b` beats `pix` on spelling, and
      // `a` would resolve to a name about to go away. Narrowing the
      // candidate set puts both cursors on `pix`, which the pointer pass
      // never rewrote and this tool will not touch.
      auto isSafeBase = [&](analysis::CellId C)
      { return !Reconstructable.contains(A->cells().get(C).Root); };
      std::optional<analysis::Resolution> Res = Resolver.resolve(V, isSafeBase);
      if (!Res)
        // Nothing safe agreed everywhere. Fall back to the unrestricted
        // answer and let step 5 decide whether it survives.
        Res = Resolver.resolve(V);
      if (!Res)
      {
        decline(Stats.Unresolved, Rec.name, "", Resolver.explain(V).Summary);
        continue;
      }

      Plan P;
      P.Ptr = V;
      P.Record = RecordIndex;
      P.Cell = Res->Cell;
      P.Kind = Res->Kind;
      P.Base = A->cells().print(P.Cell);

      auto Why = checkSubstitutable(FD, P, Rec.index_var, Resolver, Ctx);
      if (Why.has_value())
      {
        decline(Stats.Declined, Rec.name, P.Base, *Why);
        continue;
      }
      Plans.push_back(std::move(P));
    }

    // 5. The backstop for the fallback in 4: when *every* candidate that
    // agreed was itself a pointer this run intends to delete, there was
    // nothing safe to switch to, and substituting a name that is about to
    // go away would not compile. Dropping is monotone — a dropped plan can
    // only free others — so iterating settles it.
    for (bool Changed = true; Changed;)
    {
      Changed = false;
      llvm::SmallPtrSet<const VarDecl *, 8> Deleted;
      for (const Plan &P : Plans)
        Deleted.insert(P.Ptr->getCanonicalDecl());
      for (auto PI = Plans.begin(); PI != Plans.end(); ++PI)
        if (Deleted.contains(A->cells().get(PI->Cell).Root))
        {
          decline(Stats.Declined, PI->Ptr->getName(), PI->Base,
                  "its base is itself being reconstructed away");
          Plans.erase(PI);
          Changed = true;
          break;
        }
    }

    // 6. Edits, per plan, all-or-nothing.
    std::vector<Edit> Edits;
    std::vector<const Plan *> Committed;
    for (const Plan &P : Plans)
    {
      std::vector<Edit> Mine;
      buildEdits(FD, P, Resolver, Ctx, Mine);
      if (Mine.empty())
      {
        decline(Stats.Declined, P.Ptr->getName(), P.Base,
                "an edit fell outside the function's own file");
        continue;
      }
      Edits.insert(Edits.end(), Mine.begin(), Mine.end());
      Committed.push_back(&P);

      switch (P.Kind)
      {
      case analysis::CandidateKind::Param:
        ++Stats.Params;
        break;
      case analysis::CandidateKind::Local:
        ++Stats.Locals;
        break;
      case analysis::CandidateKind::LValue:
        ++Stats.LValues;
        break;
      }
      if (Verbose)
        llvm::errs() << "[reconstructed] " << Key << " '" << P.Ptr->getName()
                     << "' -> " << P.Base << "\n";
    }

    // 7. and 8.
    applyEdits(Edits, SM.getFileID(FD.getBody()->getBeginLoc()), Ctx);

    // The base is the fact the slice pass consumes in place of the guess
    // the pointer pass used to make. It is written only for pointers that
    // were actually substituted, so an empty base_text still means exactly
    // "this pointer is its own base".
    for (const Plan *P : Committed)
      Ptrs[P->Record].base_text = P->Base;
  }

  // Returns the reason the pointer cannot be substituted, or nullopt.
  std::optional<std::string>
  Reconstructor::checkSubstitutable(const FunctionDecl &FD, Plan &P,
                                    llvm::StringRef IndexName,
                                    const analysis::ParameterResolver &Resolver,
                                    ASTContext &Ctx)
  {
    RefCollector RC(P.Ptr, Ctx);
    RC.TraverseStmt(const_cast<Stmt *>(FD.getBody()));

    if (RC.Decl == nullptr)
      return "no declaration found in this function";
    if (RC.MultiDeclarator)
      return "declared alongside another declarator";
    if (RC.InMacro)
      return "declared or used inside a macro expansion";
    if (RC.Unevaluated)
      return "used in sizeof, where the base's type need not match";
    if (RC.Mutated)
      return "moved by something other than a plain assignment";
    if (RC.SideEffectingStore)
      return "a store to it has a side-effecting right-hand side";

    // A declaration in a for-init cannot simply be deleted: the `;` that
    // ends it is the first `;` of the `for` header, and has to stay.
    if (const auto *For =
            dyn_cast_or_null<ForStmt>(parentThroughParens(RC.Decl, Ctx)))
      P.DeclIsForInit = For->getInit() == RC.Decl;

    // Every store has to be the left arm of the comma the pointer pass
    // emits, which is the one shape whose deletion is local. A store that
    // is a statement of its own would need its `;` taken with it, and one
    // anywhere else cannot be removed without changing what its context
    // evaluates to; the pointer stays in both cases.
    //
    // Recording the commas is what lets `buildEdits` delete them without
    // recognizing the shape a second time — the two recognizers used to be
    // separate, and disagreed.
    for (const BinaryOperator *BO : RC.Assigns)
    {
      const auto *Comma =
          dyn_cast_or_null<BinaryOperator>(parentThroughParens(BO, Ctx));
      if (Comma == nullptr || Comma->getOpcode() != BO_Comma ||
          Comma->getLHS()->IgnoreParens() != BO)
        return "a store to it is not the left arm of a comma";
      P.StoreCommas.push_back(Comma);
    }

    const VarDecl *Root = Resolver.cells().get(P.Cell).Root;
    if (Root == nullptr)
      return "the proved cell has no root";
    if (rootNameIsAmbiguous(FD, Root))
      return "another declaration of '" + Root->getNameAsString() +
             "' would shadow the substituted text";

    P.Index = findLocalVarNamed(FD, IndexName);
    if (P.Index == nullptr)
      return "its index variable '" + IndexName.str() + "' is not a local here";
    P.Decl = RC.Decl;
    P.Refs = std::move(RC.Refs);
    return std::nullopt;
  }

  void Reconstructor::buildEdits(const FunctionDecl &FD, const Plan &P,
                                 const analysis::ParameterResolver &Resolver,
                                 ASTContext &Ctx, std::vector<Edit> &Out)
  {
    SourceManager &SM = Ctx.getSourceManager();
    const FileID FID = SM.getFileID(FD.getBody()->getBeginLoc());
    const llvm::StringRef Buffer = SM.getBufferData(FID);

    // Every edit is resolved to original file offsets as it is made, so
    // that nothing downstream holds a range in two coordinate systems at
    // once. `Escaped` is the one failure: offsets are only comparable
    // within one file, and a location that is not a plain file location
    // cannot be rewritten at all. Both are ruled out by the macro check,
    // so hitting it means dropping the pointer rather than emitting half
    // a rewrite.
    bool Escaped = false;
    auto emit = [&](CharSourceRange Range, llvm::StringRef Text, bool WholeLine)
    {
      SourceLocation B = Range.getBegin();
      SourceLocation E = Range.getEnd();
      if (Range.isTokenRange())
        E = Lexer::getLocForEndOfToken(E, 0, SM, Ctx.getLangOpts());
      if (!B.isFileID() || !E.isFileID() || SM.getFileID(B) != FID ||
          SM.getFileID(E) != FID)
      {
        Escaped = true;
        return;
      }
      unsigned Begin = SM.getFileOffset(B);
      unsigned End = SM.getFileOffset(E);
      // Widens `Begin` backwards past the range's start, which is why an
      // edit cannot be re-derived from its range once it is made.
      if (WholeLine)
        expandToWholeLine(Buffer, Begin, End);
      Out.push_back(Edit{Begin, End, Text.str()});
    };

    auto add = [&](CharSourceRange Range, llvm::StringRef Text)
    { emit(Range, Text, /*WholeLine=*/false); };

    // 1. The declaration goes away entirely, along with the line it sat
    //    on. Its `;` is the DeclStmt's own end location — which in a
    //    for-init is the header's own first `;`, so there the declaration
    //    is replaced by that semicolon rather than removed.
    const CharSourceRange DeclRange =
        CharSourceRange::getTokenRange(P.Decl->getSourceRange());
    if (P.DeclIsForInit)
      add(DeclRange, ";");
    else
      emit(DeclRange, "", /*WholeLine=*/true);

    // 2. Each store's arm goes away, leaving the index assignment beside
    //    it: `(p = ROOT, p_index_xj = OFF)` becomes `(p_index_xj = OFF)`.
    //    `checkSubstitutable` already proved every store has this shape,
    //    so there is nothing left to recognize here.
    for (const BinaryOperator *Comma : P.StoreCommas)
      add(CharSourceRange::getCharRange(Comma->getLHS()->getBeginLoc(),
                                        Comma->getRHS()->getBeginLoc()),
          "");

    // 3. Index-form recovery.
    class IndexFormWalker : public RecursiveASTVisitor<IndexFormWalker>
    {
    public:
      using AddFn = llvm::function_ref<void(CharSourceRange, llvm::StringRef)>;

      IndexFormWalker(const Plan &P,
                      const analysis::ParameterResolver &Resolver,
                      ASTContext &Ctx, AddFn Add)
          : P(P), Resolver(Resolver), Ctx(Ctx), Add(Add) {}

      bool VisitBinaryOperator(BinaryOperator *BO)
      {
        // Both sides name the same *cell*, which is decl identity plus
        // field path — never matching spellings.
        if (!isBasePlusIndex(BO->getLHS(), P.Ptr, P.Index))
          return true;
        const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
        const std::string Idx = P.Index->getNameAsString();
        const CharSourceRange Whole =
            CharSourceRange::getTokenRange(BO->getSourceRange());

        // `(p + idx) - B` is the index outright.
        if (BO->getOpcode() == BO_Sub && sameCell(RHS))
        {
          Add(Whole, Idx);
          return true;
        }
        if (!BO->isComparisonOp())
          return true;
        const std::string Op =
            BinaryOperator::getOpcodeStr(BO->getOpcode()).str();
        // `(p + idx) OP B`
        if (sameCell(RHS))
        {
          Add(Whole, Idx + " " + Op + " 0");
          return true;
        }
        // `(p + idx) OP B + e` — the (ptr, len) family, and the one shape
        // plain substitution would leave in a form `detectRoots` rejects.
        if (const auto *Sum = dyn_cast<BinaryOperator>(RHS))
        {
          if (Sum->getOpcode() != BO_Add)
            return true;
          const Expr *Other = nullptr;
          if (sameCell(Sum->getLHS()))
            Other = Sum->getRHS();
          else if (sameCell(Sum->getRHS()))
            Other = Sum->getLHS();
          if (Other != nullptr)
            Add(Whole, Idx + " " + Op + " " + textOf(Other, Ctx));
        }
        return true;
      }

    private:
      bool sameCell(const Expr *E) const
      {
        if (E == nullptr)
          return false;
        auto C = Resolver.cells().lookup(E->IgnoreParenImpCasts());
        return C && *C == P.Cell;
      }

      const Plan &P;
      const analysis::ParameterResolver &Resolver;
      ASTContext &Ctx;
      AddFn Add;
    } W(P, Resolver, Ctx, add);
    W.TraverseStmt(const_cast<Stmt *>(FD.getBody()));

    // 4. Every remaining mention of the pointer becomes the base. Edits
    //    that a deletion or a fold already swallowed are dropped by
    //    applyEdits, which keeps the outermost of any overlapping pair.
    for (const DeclRefExpr *DRE : P.Refs)
      add(CharSourceRange::getTokenRange(DRE->getSourceRange()), P.Base);

    // One edit that could not be resolved drops the whole plan.
    if (Escaped)
      Out.clear();
  }

  void Reconstructor::applyEdits(std::vector<Edit> &Edits, FileID FID,
                                 ASTContext &Ctx)
  {
    // Outermost first, so that a fold swallowing a subscript wins over the
    // substitution inside it.
    std::stable_sort(Edits.begin(), Edits.end(),
                     [](const Edit &X, const Edit &Y)
                     {
                       if (X.Begin != Y.Begin)
                         return X.Begin < Y.Begin;
                       return X.End > Y.End;
                     });

    // Now that `Edits` ascends by start offset, an edit overlaps one
    // already kept exactly when it starts before the furthest point those
    // reach.
    std::vector<Edit> Kept;
    unsigned Reach = 0;
    for (const Edit &E : Edits)
      if (E.Begin >= Reach)
      {
        Reach = E.End;
        Kept.push_back(E);
      }

    // Original offsets and an explicit original length: the Rewriter
    // overloads taking a range would re-measure it against the rewrite
    // buffer, which is exactly the coordinate system these offsets are
    // not in. `tooling::Replacement` *is* that contract — a (file, offset,
    // length, text) triple whose `apply` rebuilds the location as
    // `getLocForStartOfFile(FID) + offset` and calls `ReplaceText` with
    // the original length — so both the arithmetic and the back-to-front
    // ordering are upstream's here rather than hand-rolled.
    //
    // What upstream will not do is resolve a conflict: `Replacements::add`
    // rejects an order-dependent overlap rather than choosing between the
    // pair, and a fold swallowing a subscript is exactly such an overlap.
    // That is why the outermost-wins pass above stays this tool's own —
    // what it hands over is already non-overlapping, and single-file
    // because `buildEdits` drops any plan that reaches outside `FID`.
    SourceManager &SM = Ctx.getSourceManager();
    const SourceLocation FileStart = SM.getLocForStartOfFile(FID);
    tooling::Replacements Replaces;
    for (const Edit &E : Kept)
      if (llvm::Error Err = Replaces.add(tooling::Replacement(
              SM, FileStart.getLocWithOffset(E.Begin), E.End - E.Begin,
              E.Text)))
      {
        // Unreachable unless the scan above let a conflict through. Half
        // an applied set is the one outcome worse than declining, so the
        // whole function goes untouched.
        llvm::errs() << "xj-prepare-baserewrite: conflicting edits, nothing "
                     << "rewritten in this function: "
                     << llvm::toString(std::move(Err)) << "\n";
        return;
      }

    tooling::applyAllReplacements(Replaces, Rewrite);
  }

} // namespace xj
