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
void FunctionAccessAnalyzer::collectGlobalPointers(ASTContext &Ctx) {
    TranslationUnitDecl *TU = Ctx.getTranslationUnitDecl();
    const SourceManager &SM = Ctx.getSourceManager();

    std::vector<VarDecl *> globals;
    for (auto *D : TU->decls()) {
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
        if (VD->isExternallyVisible()) {
            logFailedPointer(VD, Ctx,
                             "file-scope pointer has external linkage (visible to "
                             "other translation units)");
            if (VERBOSE)
                llvm::outs() << "[Skip] global " << VD->getNameAsString()
                             << ": external linkage\n";
            continue;
        }

        if (VERBOSE)
            llvm::outs() << "[Collect] Found global pointer: " << VD->getNameAsString() << "\n";

        GlobalPointerState state;
        state.candidate.ptr_var = VD;
        state.candidate.is_parameter = false;
        g_global_pointer_map[VD] = state;
        globals.push_back(VD);
    }

    // Splitting an initializer has to know which names are tracked
    // pointers, so it runs only once every global is registered.
    PointerAccessCollector splitter(Ctx);
    for (VarDecl *VD : globals)
        splitter.tracked_pointers[VD] = g_global_pointer_map[VD].candidate;

    for (VarDecl *VD : globals) {
        if (!VD->hasInit())
            continue;
        PointerAccess pa;
        pa.kind = PointerAccessKind::Init;
        pa.loc = VD->getInit()->getBeginLoc();
        pa.expr = VD->getInit();
        splitter.splitAssignedValue(VD->getInit(), pa, VD,
                                    /*owner_is_declared_here=*/true);
        g_global_pointer_map[VD].accesses.push_back(pa);
    }
}

// MatchFinder fires this once per function definition. We run the
// PointerAccessCollector over the body, merge any global-pointer
// accesses we saw into g_global_pointer_map, and snapshot the
// per-function results into g_function_analyses for the end-of-TU
// phases. No edits are emitted here — see onEndOfTranslationUnit.
void FunctionAccessAnalyzer::run(const MatchFinder::MatchResult &Result) {
    const FunctionDecl *FD = Result.Nodes.getNodeAs<FunctionDecl>("funcDecl");
    if (!FD || !FD->hasBody())
        return;

    ASTContext &Ctx = *Result.Context;
    StoredCtx = &Ctx;
    Stmt *Body = FD->getBody();

    if (Ctx.getSourceManager().isInSystemHeader(FD->getLocation()))
        return;

    if (!globals_collected) {
        collectGlobalPointers(Ctx);
        globals_collected = true;
    }

    if (VERBOSE)
        llvm::outs() << "[Info] Analyzing function: " << FD->getNameAsString() << "\n";

    PointerAccessCollector V(Ctx);

    // Seed the visitor with global pointers so VisitDeclRefExpr knows
    // they are tracked.
    for (auto &[GVD, state] : g_global_pointer_map) {
        V.tracked_pointers[GVD] = state.candidate;
        V.accesses[GVD] = {};
    }

    for (const ParmVarDecl *P : FD->parameters()) {
        V.VisitVarDecl(const_cast<VarDecl *>(static_cast<const VarDecl *>(P)));
    }

    traverseFunctionBody(Body, V);

    // Roll any global-pointer accesses we just observed into the shared
    // g_global_pointer_map.
    for (auto &[GVD, state] : g_global_pointer_map) {
        auto it = V.accesses.find(GVD);
        if (it != V.accesses.end() && !it->second.empty()) {
            state.accesses.insert(state.accesses.end(),
                                  it->second.begin(), it->second.end());
        }
    }

    // Snapshot per-function pointer data for the deferred phases.
    // Globals are stored separately, so exclude them here.
    FunctionAnalysis fa;
    fa.FD = FD;
    for (auto &pair : V.accesses) {
        if (g_global_pointer_map.count(pair.first))
            continue;
        fa.tracked_pointers[pair.first] = V.tracked_pointers[pair.first];
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
// is settled first, because a pointer's index may name another's. Every
// rewrite is then planned across the entire TU before any of them is
// written, because two rewrites can nest — an offset that reads through a
// second pointer, say — and only a plan that sees both can fold one into
// the other instead of losing it.
void FunctionAccessAnalyzer::onEndOfTranslationUnit() {
    if (!StoredCtx)
        return;
    ASTContext &Ctx = *StoredCtx;
    SourceManager &SM = Ctx.getSourceManager();

    // ---- 1. Who is rewritten, and where each index lives --------------
    std::vector<PointerPlan> plans;
    std::set<const VarDecl *> transformed;
    collectCandidates(Ctx, plans, transformed);

    // ---- 2. A pairwise root is left bare because its owner's assignment
    // restores the position. If the owner is not rewritten, nothing does,
    // so the root has to be rebuilt like any other value read.
    for (PointerPlan &P : plans)
        for (PointerAccess &acc : *P.accesses)
            if (acc.kind == PointerAccessKind::PairwiseRoot &&
                !transformed.count(acc.pair_owner)) {
                // A bare root is just a value read once its owner is out. A
                // *stepped* root still has to move: `q = p++` with q not
                // rewritten is p's own increment again, rendered in value
                // position because the initializer still wants a pointer.
                switch (acc.root_adjust) {
                case RootAdjust::PostInc:
                case RootAdjust::PreInc:
                    acc.kind = PointerAccessKind::Increment;
                    break;
                case RootAdjust::PostDec:
                case RootAdjust::PreDec:
                    acc.kind = PointerAccessKind::Decrement;
                    break;
                case RootAdjust::None:
                    acc.kind = PointerAccessKind::ValueUse;
                    break;
                }
            }

    // ---- 3. Plan every access rewrite in the TU at once ---------------
    EditPlan plan(Ctx, transformed);
    for (PointerPlan &P : plans)
        plan.add(P.FD, P.ptr, *P.accesses);
    plan.build();

    // ---- 4. Write ------------------------------------------------------
    // Declarations first, in the order the pointers were planned, so that
    // two sharing an anchor stack back into source order; then one
    // replacement per outermost access rewrite.
    std::vector<Edit> edits;
    for (PointerPlan &P : plans)
        emitIndexDecl(P.site, P.ptr, plan.indexDeclInit(P.ptr, *P.accesses), SM,
                      edits);
    plan.appendRootEdits(edits);

    // A failure here is a bug in this tool. Leaving the file untouched and
    // failing the run is the only honest response: the alternative is C
    // that compiles and means something else.
    if (!plan.verify(edits))
        return;

    applyEdits(edits, SM);

    // ---- 5. Record what was done --------------------------------------
    for (const PointerPlan &P : plans)
        recordTransformed(P, Ctx);
}

// ============================================================================
// collectCandidates — decide the rewritten set, TU-wide
// ============================================================================
//
// Validation is a pure function of one pointer's own access list, and
// placement is a pure function of its declaration, so a single pass
// settles both. Nothing here depends on what any other pointer turns out
// to be — which is what lets step 2 above run once, on a final answer.
//
// Pointers are appended in reverse source order within each scope. Two
// index declarations sharing an anchor are both InsertTextBefore at one
// location, where a later insertion is placed ahead of an earlier one, so
// planning them backwards puts their declarations back in source order —
// which is what a paired index needs to name the one before it.
void FunctionAccessAnalyzer::collectCandidates(ASTContext &Ctx,
                                               std::vector<PointerPlan> &plans,
                                               std::set<const VarDecl *> &transformed) {
    SourceManager &SM = Ctx.getSourceManager();

    auto consider = [&](const FunctionDecl *FD, const VarDecl *PtrVar,
                        PointerCandidate &candidate,
                        std::vector<PointerAccess> &accesses) {
        printAccesses(PtrVar, accesses, Ctx);

        std::string error;
        if (!validatePointerCandidate(PtrVar, candidate, accesses, Ctx, error)) {
            gLog.error = error;
            logFailedPointer(PtrVar, Ctx, error);
            if (VERBOSE)
                llvm::outs() << "[Skip] " << PtrVar->getNameAsString() << ": "
                             << error << "\n";
            return;
        }

        g_pointers_found++;

        PointerPlan P;
        P.FD = FD;
        P.ptr = PtrVar;
        P.accesses = &accesses;
        if (!findIndexDeclSite(FD, PtrVar, candidate, Ctx, P.site)) {
            error = "No position for the index declaration";
            gLog.error = error;
            logFailedPointer(PtrVar, Ctx, error);
            return;
        }

        plans.push_back(std::move(P));
        transformed.insert(PtrVar);
    };

    for (auto &[FDCanon, analysis] : g_function_analyses) {
        const FunctionDecl *FD = analysis.FD;
        if (!FD || !FD->hasBody())
            continue;

        // Name every index up front, in source order so the assignment is
        // reproducible. Sorting by location rather than iterating the map
        // matters: the map is keyed by VarDecl address, which is
        // allocation order and not a property of the source.
        std::vector<const VarDecl *> ptrs;
        for (const auto &pair : analysis.accesses)
            ptrs.push_back(pair.first);
        std::sort(ptrs.begin(), ptrs.end(),
                  [&](const VarDecl *A, const VarDecl *B) {
                      return SM.isBeforeInTranslationUnit(A->getLocation(),
                                                          B->getLocation());
                  });
        assignIndexNames(ptrs);

        for (auto it = ptrs.rbegin(); it != ptrs.rend(); ++it)
            consider(FD, *it, analysis.tracked_pointers[*it], analysis.accesses[*it]);
    }

    // File-scope pointers, collected once during the first run() call.
    std::vector<const VarDecl *> globals;
    for (const auto &pair : g_global_pointer_map)
        globals.push_back(pair.first);
    std::sort(globals.begin(), globals.end(),
              [&](const VarDecl *A, const VarDecl *B) {
                  return SM.isBeforeInTranslationUnit(B->getLocation(),
                                                      A->getLocation());
              });

    for (const VarDecl *VD : globals) {
        GlobalPointerState &state = g_global_pointer_map[VD];
        if (state.accesses.empty())
            continue;
        // The Rewriter cannot edit macro-expanded text, so a global
        // declared inside a macro would have its uses rewritten without
        // the index variable itself ever being introduced.
        if (VD->getBeginLoc().isMacroID()) {
            if (VERBOSE)
                llvm::outs() << "[Skip] global " << VD->getNameAsString()
                             << ": declaration in macro expansion\n";
            continue;
        }
        consider(/*FD=*/nullptr, VD, state.candidate, state.accesses);
    }
}

// ============================================================================
// recordTransformed — logs and the metadata side-file
// ============================================================================

void FunctionAccessAnalyzer::recordTransformed(const PointerPlan &P, ASTContext &Ctx) {
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
    // nothing about a base crosses this boundary, because this pass no
    // longer has an opinion about one — xj-prepare-baserewrite fills in
    // base_text once it has proved a base.
    if (!P.FD)
        return;
    xj::PtrIndexFunctionRecord *fnRec = metadataRecordFor(P.FD, Ctx);
    if (!fnRec)
        return;

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
                                                   PointerAccessCollector &V) {
    if (VERBOSE)
        llvm::outs() << "[Debug] Traversing Function Body for pointer accesses\n";
    V.TraverseStmt(Body);
    if (VERBOSE)
        llvm::outs() << "[Debug] Done traversing Function Body for pointer accesses\n";
}

// Append a [FAILED] log entry for `VD` with `error` as the reason.
void FunctionAccessAnalyzer::logFailedPointer(const VarDecl *VD, ASTContext &Ctx,
                                               const std::string &error) {
    SourceManager &SM = Ctx.getSourceManager();
    FailedPointerLog entry;
    entry.varName = VD->getNameAsString();
    entry.line = SM.getSpellingLineNumber(VD->getLocation());
    entry.col = SM.getSpellingColumnNumber(VD->getLocation());
    entry.error = error;
    g_failed_pointers.push_back(entry);
}

// Verbose-mode debug dump of an access list — useful when chasing down
// why a pointer was misclassified or rejected.
void FunctionAccessAnalyzer::printAccesses(const VarDecl *VD,
                                            const std::vector<PointerAccess> &seq,
                                            ASTContext &Ctx) {
    if (!VERBOSE)
        return;
    SourceManager &SM = Ctx.getSourceManager();
    llvm::outs() << "[Debug] Accesses for pointer '" << VD->getNameAsString() << "':\n";
    for (const auto &access : seq) {
        llvm::outs() << "  " << pointerAccessKindToString(access.kind)
                      << " at " << access.loc.printToString(SM);
        if (!access.offset_text.empty())
            llvm::outs() << " offset=" << access.offset_text;
        if (!access.field_name.empty())
            llvm::outs() << " field=" << access.field_name;
        if (!access.subscript_text.empty())
            llvm::outs() << " subscript=" << access.subscript_text;
        if (!access.operand_text.empty())
            llvm::outs() << " operand=" << access.operand_text;
        llvm::outs() << "\n";
    }
}

// Return the metadata record for `FD`, creating it (with the right
// source file stamped) on first use. Keying by xj::functionKey rather
// than by bare name is what keeps distinct same-named statics apart:
// uniquify_statics runs after this pass, so the names have not been
// made unique yet.
xj::PtrIndexFunctionRecord *
FunctionAccessAnalyzer::metadataRecordFor(const FunctionDecl *FD, ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    std::string key = xj::functionKey(FD, SM);

    auto it = g_metadata.functions.find(key);
    if (it == g_metadata.functions.end()) {
        xj::PtrIndexFunctionRecord rec;
        rec.file = xj::functionFilePath(FD, SM);
        it = g_metadata.functions.emplace(std::move(key), std::move(rec)).first;
    }
    return &it->second;
}

// Push a vector<Edit> through the Rewriter, highest offset first so the
// offsets of the edits still to come stay valid.
//
// Nothing is dropped here. Overlap used to be discovered at this point and
// resolved by skipping the later edit, which is silent and — once every
// pointer reference has to be rewritten — wrong: the skipped rewrite is a
// reference left with its old meaning. EditPlan now settles overlap before
// anything reaches the Rewriter, so an edit that arrives here is applied.
//
// The sort is stable because insertions at one location stack in the order
// they are applied, and that order is how two index declarations sharing an
// anchor end up in source order.
void FunctionAccessAnalyzer::applyEdits(std::vector<Edit> &edits, SourceManager &SM) {
    std::stable_sort(edits.begin(), edits.end(),
                     [](const Edit &A, const Edit &B) { return A.offset > B.offset; });

    for (const auto &e : edits) {
        if (VERBOSE) {
            llvm::outs() << "[Edit] type=" << e.type
                         << " offset=" << e.offset
                         << " text=\"" << e.text << "\""
                         << " at " << e.start.printToString(SM) << "\n";
        }

        switch (e.type) {
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

