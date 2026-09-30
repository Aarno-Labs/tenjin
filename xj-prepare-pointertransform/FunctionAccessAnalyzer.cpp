// FunctionAccessAnalyzer.cpp — see FunctionAccessAnalyzer.h for the
// high-level pipeline. Code below is grouped into:
//
//   1. Driver: constructor, run() (per-function analysis), and
//      onEndOfTranslationUnit() (the phase orchestrator).
//   2. collectCandidates — which pointers are rewritten, and where
//      each companion index is declared.
//   3. Helpers — recordTransformed, metadataRecordFor, applyEdits, etc.
//
// All actual source rewriting is deferred to onEndOfTranslationUnit so
// that every function in the TU has been analyzed before any edits are
// emitted.

#include "FunctionAccessAnalyzer.h"

#include "EditPlan.h"
#include "FunctionKey.h"

// ============================================================================
// Driver
// ============================================================================

FunctionAccessAnalyzer::FunctionAccessAnalyzer(Rewriter &R) : TheRewriter(R) {}

// One-time scan of the TU for file-scope pointer variables. The
// per-function visitor uses this map so that uses of globals inside
// functions get classified alongside locals.
void FunctionAccessAnalyzer::collectGlobalPointers(ASTContext &Ctx)
{
    TranslationUnitDecl *TU = Ctx.getTranslationUnitDecl();
    const SourceManager &SM = Ctx.getSourceManager();

    std::vector<VarDecl *> globals;
    for (auto *D : TU->decls())
    {
        auto *VD = dyn_cast<VarDecl>(D);
        if (!VD)
            continue;
        if (!VD->getType()->isPointerType())
            continue;
        if (SM.isInSystemHeader(VD->getLocation()))
            continue;
        if (VD->hasExternalStorage())
            continue;
        // A file-scope pointer with external linkage is read from
        // translation units this per-TU pass never rewrites: they hold
        // `extern T *p;` and keep spelling `*p`, with no `p_index_xj` to
        // advance. Splitting it here pins every one of those uses at
        // index 0, and because the base pointer still exists the program
        // links and silently computes the wrong thing.
        if (VD->isExternallyVisible())
        {
            logFailedPointer(VD, Ctx,
                             "file-scope pointer has external linkage (visible to "
                             "other translation units)");
            if (VERBOSE)
                llvm::outs() << "[Skip] global " << VD->getNameAsString()
                             << ": external linkage\n";
            continue;
        }
        // Declared more than once — tentatively, or again by a block-scope
        // `extern` — its references are spread over declarations that are
        // tracked one at a time, so no single access list covers them.
        if (VD->getFirstDecl() != VD->getMostRecentDecl())
        {
            logFailedPointer(VD, Ctx, "file-scope pointer is declared more than once");
            continue;
        }

        globals.push_back(VD);
    }

    // Splitting an initializer has to know which names are tracked
    // pointers, so every global is registered before any is visited.
    PointerAccessCollector collector(Ctx);
    collector.tracked_pointers.insert(globals.begin(), globals.end());
    for (VarDecl *VD : globals)
    {
        collector.VisitVarDecl(VD);
        g_global_pointer_map[VD] = collector.accesses[VD];
    }
}

// MatchFinder fires this once per function definition. We run the
// PointerAccessCollector over the body, merge any global-pointer
// accesses we saw into g_global_pointer_map, and snapshot the
// per-function results into g_function_analyses for the end-of-TU
// phases. No edits are emitted here — see onEndOfTranslationUnit.
void FunctionAccessAnalyzer::run(const MatchFinder::MatchResult &Result)
{
    const FunctionDecl *FD = Result.Nodes.getNodeAs<FunctionDecl>("funcDecl");
    if (!FD || !FD->hasBody())
        return;

    ASTContext &Ctx = *Result.Context;
    StoredCtx = &Ctx;
    Stmt *Body = FD->getBody();

    if (Ctx.getSourceManager().isInSystemHeader(FD->getLocation()))
        return;

    if (!globals_collected)
    {
        collectGlobalPointers(Ctx);
        globals_collected = true;
    }

    if (VERBOSE)
        llvm::outs() << "[Info] Analyzing function: " << FD->getNameAsString() << "\n";

    PointerAccessCollector V(Ctx);

    // Seed the visitor with global pointers so VisitDeclRefExpr knows
    // they are tracked.
    for (auto &[GVD, global_accesses] : g_global_pointer_map)
    {
        V.tracked_pointers.insert(GVD);
        V.accesses[GVD] = {};
    }

    for (const ParmVarDecl *P : FD->parameters())
    {
        V.VisitVarDecl(const_cast<VarDecl *>(static_cast<const VarDecl *>(P)));
    }

    traverseFunctionBody(Body, V);

    // Roll any global-pointer accesses we just observed into the shared
    // g_global_pointer_map.
    for (auto &[GVD, global_accesses] : g_global_pointer_map)
    {
        auto it = V.accesses.find(GVD);
        if (it != V.accesses.end() && !it->second.empty())
        {
            global_accesses.insert(global_accesses.end(),
                                   it->second.begin(), it->second.end());
        }
    }

    // Snapshot per-function pointer data for the deferred phases.
    // Globals are stored separately, so exclude them here.
    FunctionAnalysis fa;
    fa.FD = FD;
    for (auto &pair : V.accesses)
    {
        if (g_global_pointer_map.count(pair.first))
            continue;
        fa.accesses[pair.first] = pair.second;
    }
    g_function_analyses[FD->getCanonicalDecl()] = std::move(fa);
}

// All actual source rewriting happens here, once every function in the
// TU has been analyzed. This tool knows nothing about RustSlice
// reshaping: it rewrites moving pointers as indices and records each
// rewritten pointer's facts in the metadata side-file. All slice
// candidate detection happens downstream, in xj-prepare-slicetransform.
//
// The order of the phases is the whole design. Which pointers are rewritten
// is settled first, because a pointer's index may name another's — and only
// the plan, which is handed that answer, decides that one does. Every
// rewrite is then planned across the entire TU before any of them is
// written, because two rewrites can nest — an offset that reads through a
// second pointer, say — and only a plan that sees both can fold one into
// the other instead of losing it.
void FunctionAccessAnalyzer::onEndOfTranslationUnit()
{
    if (!StoredCtx)
        return;
    ASTContext &Ctx = *StoredCtx;
    SourceManager &SM = Ctx.getSourceManager();

    // ---- 1. Who is rewritten, and where each index lives --------------
    std::vector<PointerPlan> plans;
    std::set<const VarDecl *> transformed;
    std::map<const VarDecl *, PointerFacts> facts;
    collectCandidates(Ctx, plans, transformed, facts);

    // ---- 2. Plan every access rewrite in the TU at once ---------------
    EditPlan plan(Ctx, transformed, facts);
    for (const PointerPlan &P : plans)
        plan.add(P.ptr, *P.accesses);
    plan.build();

    // ---- 3. Write ------------------------------------------------------
    // Declarations first, in the order the pointers were planned, so that
    // two sharing an anchor stack back into source order; then one
    // replacement per outermost access rewrite.
    std::vector<Edit> edits;
    for (const PointerPlan &P : plans)
        emitIndexDecl(P.site, P.ptr, plan.indexDeclInit(P.ptr, *P.accesses), SM,
                      edits);
    plan.appendRootEdits(edits);

    // A failure here is a bug in this tool. Leaving the file untouched and
    // failing the run is the only honest response: the alternative is C
    // that compiles and means something else.
    if (!plan.verify(edits))
        return;

    applyEdits(edits, SM);

    // ---- 4. Record what was done --------------------------------------
    for (const PointerPlan &P : plans)
        recordTransformed(P, Ctx);
}

// ============================================================================
// collectCandidates — decide the rewritten set, TU-wide
// ============================================================================
//
// Candidacy is a function of one pointer's own access list and of which
// references are roots, and placement is a function of its declaration, so
// a single pass settles both. Whether a candidate can be edited is the one
// question that depends on what the others turn out to be, and it is asked
// once they are all known.
//
// Pointers are appended in reverse source order within each scope. Two
// index declarations sharing an anchor are both InsertTextBefore at one
// location, where a later insertion is placed ahead of an earlier one, so
// planning them backwards puts their declarations back in source order —
// which is what a paired index needs to name the one before it.
void FunctionAccessAnalyzer::collectCandidates(
    ASTContext &Ctx, std::vector<PointerPlan> &plans,
    std::set<const VarDecl *> &transformed,
    std::map<const VarDecl *, PointerFacts> &facts)
{
    SourceManager &SM = Ctx.getSourceManager();

    // Every reference that some assignment takes as its root, in the whole
    // TU. Candidacy asks for them; see isCandidate.
    std::set<const Expr *> roots;
    auto noteRoots = [&](const std::vector<PointerAccess> &accesses)
    {
        for (const PointerAccess &a : accesses)
            if (a.root_expr)
                roots.insert(a.root_expr);
    };
    for (const auto &[FDCanon, analysis] : g_function_analyses)
        for (const auto &[VD, accesses] : analysis.accesses)
            noteRoots(accesses);
    for (const auto &[VD, accesses] : g_global_pointer_map)
        noteRoots(accesses);

    auto decline = [&](const VarDecl *PtrVar, const std::string &error)
    {
        gLog.error = error;
        logFailedPointer(PtrVar, Ctx, error);
        if (VERBOSE)
            llvm::outs() << "[Skip] " << PtrVar->getNameAsString() << ": "
                         << error << "\n";
    };

    auto consider = [&](const FunctionDecl *FD, const VarDecl *PtrVar,
                        const std::vector<PointerAccess> &accesses)
    {
        printAccesses(PtrVar, accesses, Ctx);

        std::string error;
        if (!isCandidate(accesses, roots, Ctx, error))
        {
            decline(PtrVar, error);
            return;
        }

        gLog.foundPointer = true;
        g_pointers_found++;

        PointerPlan P;
        P.FD = FD;
        P.ptr = PtrVar;
        P.accesses = &accesses;
        if (!findIndexDeclSite(FD, PtrVar, Ctx, P.site))
        {
            decline(PtrVar, "No position for the index declaration");
            return;
        }

        plans.push_back(std::move(P));
        transformed.insert(PtrVar);
    };

    for (auto &[FDCanon, analysis] : g_function_analyses)
    {
        const FunctionDecl *FD = analysis.FD;
        if (!FD || !FD->hasBody())
            continue;

        // Name and process pointers in source order. The map is keyed by
        // VarDecl address, so iterating it would make both index naming and
        // metadata order depend on allocation order.
        std::vector<const VarDecl *> ptrs;
        for (const auto &pair : analysis.accesses)
            ptrs.push_back(pair.first);
        std::sort(ptrs.begin(), ptrs.end(),
                  [&](const VarDecl *A, const VarDecl *B)
                  {
                      return SM.isBeforeInTranslationUnit(A->getLocation(),
                                                          B->getLocation());
                  });
        assignIndexNames(ptrs);

        for (auto it = ptrs.rbegin(); it != ptrs.rend(); ++it)
            consider(FD, *it, analysis.accesses[*it]);
    }

    // File-scope pointers, collected once during the first run() call.
    std::vector<const VarDecl *> globals;
    for (const auto &pair : g_global_pointer_map)
        globals.push_back(pair.first);
    std::sort(globals.begin(), globals.end(),
              [&](const VarDecl *A, const VarDecl *B)
              {
                  return SM.isBeforeInTranslationUnit(B->getLocation(),
                                                      A->getLocation());
              });

    for (const VarDecl *VD : globals)
    {
        const std::vector<PointerAccess> &accesses = g_global_pointer_map[VD];
        if (accesses.empty())
            continue;
        // The Rewriter cannot edit macro-expanded text, so a global
        // declared inside a macro would have its uses rewritten without
        // the index variable itself ever being introduced.
        if (VD->getBeginLoc().isMacroID())
        {
            if (VERBOSE)
                llvm::outs() << "[Skip] global " << VD->getNameAsString()
                             << ": declaration in macro expansion\n";
            continue;
        }
        consider(/*FD=*/nullptr, VD, accesses);
    }

    // Whether the candidates can be edited is asked last, of all of them
    // together. What is edited depends on a pointer's facts, and those
    // follow its pairings — so dropping one pointer can change another's.
    // It can only take facts away, so going round again settles.
    for (bool settled = false; !settled;)
    {
        settled = true;
        RewrittenPointers rewritten;
        for (const PointerPlan &P : plans)
            rewritten[P.ptr] = P.accesses;
        facts = pointerFacts(rewritten);

        for (auto it = plans.begin(); it != plans.end(); ++it)
        {
            std::string error;
            if (isEditable(*it->accesses, facts.at(it->ptr), Ctx, error))
                continue;
            decline(it->ptr, error);
            transformed.erase(it->ptr);
            plans.erase(it);
            settled = false;
            break;
        }
    }
}

// ============================================================================
// recordTransformed — logs and the metadata side-file
// ============================================================================

void FunctionAccessAnalyzer::recordTransformed(const PointerPlan &P, ASTContext &Ctx)
{
    SourceManager &SM = Ctx.getSourceManager();
    SourceLocation Loc = P.ptr->getLocation();

    gLog.replacedPointer = true;
    g_pointers_replaced++;
    g_succeeded_pointers.push_back({P.ptr->getNameAsString(),
                                    P.FD ? P.FD->getNameAsString() : "(global)",
                                    SM.getSpellingLineNumber(Loc),
                                    SM.getSpellingColumnNumber(Loc)});

    // Record the transformed pointer in the metadata side-file so the
    // downstream tools know which index variables exist. Identity only:
    // nothing about a base crosses this boundary, because this pass has
    // no opinion about one — xj-prepare-baserewrite fills in base_text
    // once it has proved a base.
    if (!P.FD)
        return;
    xj::PtrIndexFunctionRecord *fnRec = metadataRecordFor(P.FD, Ctx);

    xj::PtrIndexPointerRecord rec;
    rec.name = P.ptr->getNameAsString();
    rec.index_var = indexNameFor(P.ptr);
    rec.param_index = -1;
    if (const auto *PD = dyn_cast<ParmVarDecl>(P.ptr))
        rec.param_index = static_cast<int>(PD->getFunctionScopeIndex());
    fnRec->pointers.push_back(std::move(rec));

    // Note the *pre-rewrite* position of the declaring identifier and
    // defer translating it, because pointers earlier in the function
    // have not been rewritten yet. The identifier token itself is never
    // rewritten, so it maps to itself and end-of-TU is free to look it
    // up by offset. See PendingDeclLoc.
    auto [FID, Off] = SM.getDecomposedLoc(SM.getSpellingLoc(Loc));
    if (FID.isValid())
        g_pending_decl_locs.push_back(
            {xj::functionKey(P.FD, SM), fnRec->pointers.size() - 1, FID, Off});
}

// ============================================================================
// Small helpers
// ============================================================================

// Drive the per-function visitor over the body. Pulled out so the
// trace points have a single home.
void FunctionAccessAnalyzer::traverseFunctionBody(Stmt *Body,
                                                  PointerAccessCollector &V)
{
    if (VERBOSE)
        llvm::outs() << "[Debug] Traversing Function Body for pointer accesses\n";
    V.TraverseStmt(Body);
    if (VERBOSE)
        llvm::outs() << "[Debug] Done traversing Function Body for pointer accesses\n";
}

// Append a [FAILED] log entry for `VD` with `error` as the reason.
void FunctionAccessAnalyzer::logFailedPointer(const VarDecl *VD, ASTContext &Ctx,
                                              const std::string &error)
{
    SourceManager &SM = Ctx.getSourceManager();
    FailedPointerLog entry;
    entry.varName = VD->getNameAsString();
    entry.line = SM.getSpellingLineNumber(VD->getLocation());
    entry.col = SM.getSpellingColumnNumber(VD->getLocation());
    entry.error = error;
    g_failed_pointers.push_back(entry);
}

// Verbose-mode debug dump of an access list — useful when chasing down
// why a pointer was misclassified or rejected. Spellings are lexed here,
// from the expressions each access carries.
void FunctionAccessAnalyzer::printAccesses(const VarDecl *VD,
                                           const std::vector<PointerAccess> &seq,
                                           ASTContext &Ctx)
{
    if (!VERBOSE)
        return;
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();
    auto spell = [&](const std::vector<OffsetTerm> &terms)
    {
        std::string text;
        for (const OffsetTerm &t : terms)
            text += (t.minus ? " - " : " + ") + getSourceText(t.expr, SM, LO);
        return text;
    };

    llvm::outs() << "[Debug] Accesses for pointer '" << VD->getNameAsString() << "':\n";
    for (const auto &access : seq)
    {
        llvm::outs() << "  " << pointerAccessKindToString(access.kind)
                     << " at " << access.loc.printToString(SM);
        if (!access.offset_terms.empty())
            llvm::outs() << " offset=" << spell(access.offset_terms);
        if (access.isSplit())
            llvm::outs() << " root="
                         << applyStep(access.step,
                                      getSourceText(access.root_expr, SM, LO));
        if (!access.index_terms.empty())
            llvm::outs() << " index=" << spell(access.index_terms);
        if (!access.field_name.empty())
            llvm::outs() << " field=" << access.field_name;
        if (access.subscript_expr)
            llvm::outs() << " subscript="
                         << getSourceText(access.subscript_expr, SM, LO);
        if (access.kind == PointerAccessKind::Move)
            if (const auto *BO =
                    dyn_cast_or_null<BinaryOperator>(access.enclosing_stmt))
                llvm::outs() << " operand=" << getSourceText(BO->getRHS(), SM, LO);
        llvm::outs() << "\n";
    }
}

// Return the metadata record for `FD`, creating it (with the right
// source file stamped) on first use. Keying by xj::functionKey rather
// than by bare name is what keeps distinct same-named statics apart:
// uniquify_statics runs after this pass, so the names have not been
// made unique yet.
xj::PtrIndexFunctionRecord *
FunctionAccessAnalyzer::metadataRecordFor(const FunctionDecl *FD, ASTContext &Ctx)
{
    SourceManager &SM = Ctx.getSourceManager();
    std::string key = xj::functionKey(FD, SM);

    auto it = g_metadata.functions.find(key);
    if (it == g_metadata.functions.end())
    {
        xj::PtrIndexFunctionRecord rec;
        rec.file = xj::functionFilePath(FD, SM);
        it = g_metadata.functions.emplace(std::move(key), std::move(rec)).first;
    }
    return &it->second;
}

// Push a vector<Edit> through the Rewriter, highest offset first so the
// offsets of the edits still to come stay valid.
//
// Every edit is applied. EditPlan has settled overlap before anything
// reaches the Rewriter, and a skipped edit would be a reference left with
// its old meaning.
//
// The sort is stable because insertions at one location stack in the order
// they are applied, and that order is how two index declarations sharing an
// anchor end up in source order.
void FunctionAccessAnalyzer::applyEdits(std::vector<Edit> &edits, SourceManager &SM)
{
    std::stable_sort(edits.begin(), edits.end(),
                     [](const Edit &A, const Edit &B)
                     { return A.offset > B.offset; });

    for (const auto &e : edits)
    {
        if (VERBOSE)
        {
            llvm::outs() << "[Edit] type=" << e.type
                         << " offset=" << e.offset
                         << " text=\"" << e.text << "\""
                         << " at " << e.start.printToString(SM) << "\n";
        }

        switch (e.type)
        {
        case Edit::Replace:
            // The (SourceLocation, unsigned, StringRef) overload avoids
            // Rewriter's getRangeSize including a prior InsertTextBefore at
            // the same offset.
            TheRewriter.ReplaceText(e.start, SM.getFileOffset(e.end) - e.offset, e.text);
            break;
        case Edit::InsertBefore:
            TheRewriter.InsertTextBefore(e.start, e.text);
            break;
        case Edit::InsertAfterToken:
            TheRewriter.InsertTextAfterToken(e.start, e.text);
            break;
        }
    }
}
