// FunctionAccessAnalyzer.cpp — see FunctionAccessAnalyzer.h for the
// high-level pipeline. Code below is grouped into:
//
//   1. Driver: constructor, run() (per-function analysis), and
//      onEndOfTranslationUnit() (the phase orchestrator).
//   2. transformAllFunctions — plain index rewriting.
//   3. Helpers — transformPointerVar, metadataRecordFor, applyEdits, etc.
//
// All actual source rewriting is deferred to onEndOfTranslationUnit so
// that every function in the TU has been analyzed before any edits are
// emitted.

#include "FunctionAccessAnalyzer.h"

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
void FunctionAccessAnalyzer::onEndOfTranslationUnit() {
    if (!StoredCtx)
        return;
    ASTContext &Ctx = *StoredCtx;

    // Rewrite individual local pointers function-by-function.
    transformAllFunctions(Ctx);

    // Rewrite file-scope pointer variables (they were collected once
    // during the first run() call).
    std::set<const VarDecl *> transformed_globals;
    for (auto &[VD, state] : g_global_pointer_map) {
        if (state.accesses.empty())
            continue;

        printAccesses(VD, state.accesses, Ctx);

        std::string error;
        if (!validatePointerCandidate(VD, state.candidate, state.accesses,
                                      Ctx, error)) {
            gLog.error = error;
            logFailedPointer(VD, Ctx, error);
            if (VERBOSE)
                llvm::outs() << "[Skip] global " << VD->getNameAsString()
                              << ": " << error << "\n";
            continue;
        }

        // The Rewriter cannot edit macro-expanded text, so a global
        // declared inside a macro would have its uses rewritten without
        // the index variable itself ever being introduced.
        if (VD->getBeginLoc().isMacroID()) {
            if (VERBOSE)
                llvm::outs() << "[Skip] global " << VD->getNameAsString()
                              << ": declaration in macro expansion\n";
            continue;
        }

        transformed_globals.insert(VD);
    }

    for (auto &[VD, state] : g_global_pointer_map) {
        for (auto &acc : state.accesses)
            if (acc.kind == PointerAccessKind::PairwiseRoot &&
                !transformed_globals.count(acc.pair_owner))
                acc.kind = PointerAccessKind::ValueUse;
    }

    // Reverse source order, for the same reason transformAllFunctions uses
    // it: two index declarations sharing an anchor are both InsertTextBefore
    // at one location, where a later insertion is placed ahead of an
    // earlier one.
    std::vector<const VarDecl *> globals;
    for (const auto &pair : g_global_pointer_map)
        globals.push_back(pair.first);
    std::sort(globals.begin(), globals.end(),
              [&](const VarDecl *A, const VarDecl *B) {
                  return Ctx.getSourceManager().isBeforeInTranslationUnit(
                      B->getLocation(), A->getLocation());
              });

    for (const VarDecl *VD : globals) {
        if (!transformed_globals.count(VD))
            continue;
        GlobalPointerState &state = g_global_pointer_map[VD];

        g_pointers_found++;

        if (generateTransformation(/*FD=*/nullptr, VD, state.candidate,
                                   state.accesses, transformed_globals, Ctx)) {
            gLog.replacedPointer = true;
            g_pointers_replaced++;
            SourceManager &SM = Ctx.getSourceManager();
            SourceLocation Loc = VD->getLocation();
            g_succeeded_pointers.push_back({
                VD->getNameAsString(),
                "(global)",
                SM.getSpellingLineNumber(Loc),
                SM.getSpellingColumnNumber(Loc)
            });
        }
    }
}

// ============================================================================
// transformAllFunctions — rewrite every local pointer that's safe to
// rewrite, function by function, in plain (base-param-relative) form.
// ============================================================================

void FunctionAccessAnalyzer::transformAllFunctions(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();

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

        // Validation is a pure function of a single pointer's own access
        // list, so one pass settles the whole function. Nothing here
        // depends on what any other pointer turns out to be.
        std::set<const VarDecl *> transformed;
        for (const VarDecl *PtrVar : ptrs) {
            std::string error;
            if (validatePointerCandidate(PtrVar, analysis.tracked_pointers[PtrVar],
                                         analysis.accesses[PtrVar], Ctx, error)) {
                transformed.insert(PtrVar);
            } else {
                gLog.error = error;
                logFailedPointer(PtrVar, Ctx, error);
                if (VERBOSE)
                    llvm::outs() << "[Skip] " << PtrVar->getNameAsString()
                                 << ": " << error << "\n";
            }
        }

        // A pairwise root is left bare because its owner's assignment
        // restores the position. If the owner is not rewritten, nothing
        // does, so the root has to be rebuilt like any other value read.
        for (const VarDecl *PtrVar : ptrs)
            for (auto &acc : analysis.accesses[PtrVar])
                if (acc.kind == PointerAccessKind::PairwiseRoot &&
                    !transformed.count(acc.pair_owner))
                    acc.kind = PointerAccessKind::ValueUse;

        // Reverse source order. Two index declarations sharing an anchor
        // are both InsertTextBefore at one location, where a later
        // insertion is placed ahead of an earlier one — so rewriting the
        // pointers backwards puts their declarations back in source order,
        // which is what a paired index needs to name the one before it.
        for (auto it = ptrs.rbegin(); it != ptrs.rend(); ++it) {
            if (!transformed.count(*it))
                continue;
            transformPointerVar(FD, *it, analysis.tracked_pointers[*it],
                                analysis.accesses[*it], transformed, Ctx);
        }
    }
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

// Validate-and-rewrite one local pointer. Bumps the per-file counters
// and emits the [REPLACED] / [FAILED] log entries.
void FunctionAccessAnalyzer::transformPointerVar(const FunctionDecl *FD,
                                                  const VarDecl *PtrVar,
                                                  PointerCandidate &candidate,
                                                  std::vector<PointerAccess> &accesses,
                                                  const std::set<const VarDecl *> &transformed,
                                                  ASTContext &Ctx) {
    printAccesses(PtrVar, accesses, Ctx);

    g_pointers_found++;

    if (!generateTransformation(FD, PtrVar, candidate, accesses, transformed, Ctx))
        return;

    gLog.replacedPointer = true;
    g_pointers_replaced++;
    SourceManager &SM = Ctx.getSourceManager();
    SourceLocation Loc = PtrVar->getLocation();
    g_succeeded_pointers.push_back({
        PtrVar->getNameAsString(),
        FD ? FD->getNameAsString() : "(global)",
        SM.getSpellingLineNumber(Loc),
        SM.getSpellingColumnNumber(Loc)
    });

    // Record the transformed pointer in the metadata side-file so the
    // slice pass knows which index variables exist. Identity only: nothing
    // about a base crosses this boundary, because this pass no longer has
    // an opinion about one.
    if (!FD)
        return;
    if (xj::PtrIndexFunctionRecord *fnRec = metadataRecordFor(FD, Ctx)) {
        xj::PtrIndexPointerRecord rec;
        rec.name = PtrVar->getNameAsString();
        rec.index_var = indexNameFor(PtrVar);
        rec.param_index = -1;
        if (const auto *PD = dyn_cast<ParmVarDecl>(PtrVar))
            rec.param_index = static_cast<int>(PD->getFunctionScopeIndex());
        fnRec->pointers.push_back(std::move(rec));
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

// Push a vector<Edit> through the Rewriter. Edits are applied
// highest-offset first so earlier offsets remain valid; any edit that
// overlaps an already-edited range is dropped to protect against the
// same span being rewritten twice by different phases.
void FunctionAccessAnalyzer::applyEdits(std::vector<Edit> &edits, SourceManager &SM) {
    std::sort(edits.begin(), edits.end(),
              [](const Edit &A, const Edit &B) { return A.offset > B.offset; });

    for (const auto &e : edits) {
        if (VERBOSE) {
            llvm::outs() << "[Edit] type=" << e.type
                         << " offset=" << e.offset
                         << " end_offset=" << SM.getFileOffset(e.end)
                         << " text=\"" << e.text << "\""
                         << " at " << e.start.printToString(SM) << "\n";
        }

        // Skip edits whose range overlaps with an already-applied edit.
        // This prevents garbled output when two transformed pointers
        // both try to rewrite the same comparison expression.
        if (e.type == Edit::Replace) {
            FileID eFile = SM.getFileID(e.start);
            unsigned eStart = e.offset;
            unsigned eEnd = SM.getFileOffset(e.end);
            bool overlaps = false;
            for (const auto &r : m_edited_ranges) {
                if (r.file == eFile && eStart < r.end && eEnd > r.begin) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps) {
                if (VERBOSE)
                    llvm::outs() << "[Edit] SKIPPED (overlapping range)\n";
                continue;
            }
        }

        switch (e.type) {
        case Edit::Replace: {
            // Use the (SourceLocation, unsigned, StringRef) overload to avoid
            // Rewriter's getRangeSize including prior InsertTextBefore at same offset
            unsigned origLen = SM.getFileOffset(e.end) - e.offset;
            TheRewriter.ReplaceText(e.start, origLen, e.text);
            m_edited_ranges.push_back(
                {SM.getFileID(e.start), e.offset, SM.getFileOffset(e.end)});
        }
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
