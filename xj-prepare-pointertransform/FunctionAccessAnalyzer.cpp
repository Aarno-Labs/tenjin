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
        state.candidate.base_array = nullptr;
        state.candidate.base_array_text = "";
        state.candidate.is_parameter = false;

        if (VD->hasInit()) {
            PointerAccessCollector tempCollector(Ctx);
            tempCollector.analyzePointerInit(VD->getInit(), VD, state.candidate, state.accesses);
        }

        g_global_pointer_map[VD] = state;
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

    // Roll any global-pointer accesses we just observed into the
    // shared g_global_pointer_map. Also opportunistically promote a
    // base array from the per-function candidate if the global one
    // didn't have one yet (e.g. a global pointer initialized by an
    // assignment in some function).
    for (auto &[GVD, state] : g_global_pointer_map) {
        auto it = V.accesses.find(GVD);
        if (it != V.accesses.end() && !it->second.empty()) {
            state.accesses.insert(state.accesses.end(),
                                  it->second.begin(), it->second.end());
        }
        auto cit = V.tracked_pointers.find(GVD);
        if (cit != V.tracked_pointers.end() &&
            state.candidate.base_array_text.empty() &&
            !cit->second.base_array_text.empty()) {
            state.candidate = cit->second;
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
    for (auto &[VD, state] : g_global_pointer_map) {
        if (state.accesses.empty())
            continue;

        printAccesses(VD, state.accesses, Ctx);

        std::string error;
        if (validatePointerCandidate(VD, state.candidate, state.accesses,
                                     Ctx, error) == TransformMode::Reject) {
            gLog.error = error;
            logFailedPointer(VD, Ctx, error);
            if (VERBOSE)
                llvm::outs() << "[Skip] global " << VD->getNameAsString()
                              << ": " << error << "\n";
            continue;
        }

        // The Rewriter cannot edit macro-expanded text, so a global
        // declared inside a macro would have its uses rewritten to
        // <name>_index without ever introducing the _index variable
        // itself. Skip the whole pointer in that case.
        if (VD->getBeginLoc().isMacroID()) {
            if (VERBOSE)
                llvm::outs() << "[Skip] global " << VD->getNameAsString()
                              << ": declaration in macro expansion\n";
            continue;
        }

        g_pointers_found++;

        if (generateGlobalTransformation(VD, state.candidate, state.accesses, Ctx)) {
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
// The per-function rewrite pipeline
// ============================================================================
//
// transformAllFunctions walks the analyzed functions; transformFunction runs
// the pipeline for one of them. The steps were previously one 200-line body;
// they are the same steps, in the same order, with names.
//
//   1. assignIndexNamesInSourceOrder  name every index before anything reads one
//   2. pointersInEditOrder            decide the order rewrites are emitted in
//   3. decideTransformModes           judge each pointer
//   4. resolveCrossPointerComparisons restate comparisons naming another pointer
//   5. rejectStaleOffsets             drop pointers whose pasted offset would rot
//   6. emitPointerRewrites            rewrite the survivors

// Step 1. Sorting by source location rather than iterating the map matters:
// the map is keyed by VarDecl address, which is allocation order and not a
// property of the source, so the names it hands out would not be reproducible.
static void assignIndexNamesInSourceOrder(const FunctionAnalysis &analysis,
                                          ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    std::vector<const VarDecl *> ptrs;
    for (const auto &pair : analysis.accesses)
        ptrs.push_back(pair.first);
    std::sort(ptrs.begin(), ptrs.end(),
              [&](const VarDecl *A, const VarDecl *B) {
                  return SM.isBeforeInTranslationUnit(A->getLocation(),
                                                      B->getLocation());
              });
    assignIndexNames(ptrs);
}

// True when the pointer indexes into one of the function's own pointer
// parameters and has a comparison that can be resolved against it. Those are
// the pointers whose rewrite the slice pass can anchor on, so they win any
// overlap against a pointer rewritten later.
static bool isParamBounded(const FunctionDecl *FD,
                           const PointerCandidate &candidate,
                           const std::vector<PointerAccess> &accesses) {
    if (candidate.is_parameter || FD->getNumParams() == 0)
        return false;

    bool base_is_param = false;
    for (unsigned i = 0; i < FD->getNumParams(); i++) {
        if (FD->getParamDecl(i)->getNameAsString() == candidate.base_array_text &&
            FD->getParamDecl(i)->getType()->isPointerType()) {
            base_is_param = true;
            break;
        }
    }
    if (!base_is_param)
        return false;

    for (const auto &acc : accesses) {
        if (acc.kind == PointerAccessKind::ComparisonExpr)
            return true;
    }
    return false;
}

// Step 2. See EditOrder for why the two groups stay apart.
static EditOrder pointersInEditOrder(const FunctionDecl *FD,
                                     FunctionAnalysis &analysis) {
    EditOrder order;
    for (auto &pair : analysis.accesses) {
        const VarDecl *PtrVar = pair.first;
        if (isParamBounded(FD, analysis.tracked_pointers[PtrVar], pair.second))
            order.param_bounded.push_back(PtrVar);
        else
            order.rest.push_back(PtrVar);
    }
    return order;
}

// Step 3.
TransformModeMap
FunctionAccessAnalyzer::decideTransformModes(FunctionAnalysis &analysis,
                                             ASTContext &Ctx) {
    TransformModeMap modes;
    for (auto &pair : analysis.accesses) {
        const VarDecl *PtrVar = pair.first;
        auto &candidate = analysis.tracked_pointers[PtrVar];
        auto &access_list = pair.second;

        std::string error;
        TransformMode mode =
            validatePointerCandidate(PtrVar, candidate, access_list, Ctx, error);
        if (mode != TransformMode::Reject)
            modes[PtrVar] = mode;
    }
    return modes;
}

// Step 4. A comparison against another pointer that is *also* being rewritten
// cannot keep naming that pointer — it is about to be deleted. Restate the
// operand in reconstructed form, `other_base + other_index`.
static void resolveCrossPointerComparisons(FunctionAnalysis &analysis,
                                           const TransformModeMap &modes,
                                           ASTContext &Ctx) {
    for (auto &pair : analysis.accesses) {
        const VarDecl *PtrVar = pair.first;
        auto &candidate = analysis.tracked_pointers[PtrVar];
        if (modes.find(PtrVar) == modes.end())
            continue;
        for (auto &acc : pair.second) {
            if (acc.kind != PointerAccessKind::ComparisonExpr)
                continue;
            // Check if operand_text is "(other - base)" pattern
            // and the other is also being transformed
            if (!acc.field_name.empty() &&
                acc.field_name != candidate.base_array_text)
                continue; // shape-5 param reconstruction; leave alone
            // (pointer-form equality records — field_name == base —
            // fall through: their operand still names the other
            // pointer and must be reconstructed below if that
            // pointer is transformed too)
            // Look for the other pointer in the comparison's parent
            const Stmt *P = skipTransparentParents(acc.expr, Ctx);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) continue;
            const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
            const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
            const Expr *OtherSide = nullptr;
            if (const DeclRefExpr *LDRE = dyn_cast<DeclRefExpr>(LHS)) {
                if (LDRE->getDecl() == PtrVar) OtherSide = RHS;
            }
            if (!OtherSide) {
                if (const DeclRefExpr *RDRE = dyn_cast<DeclRefExpr>(RHS)) {
                    if (RDRE->getDecl() == PtrVar) OtherSide = LHS;
                }
            }
            if (!OtherSide) continue;
            // Walk through OtherSide to find a DeclRefExpr to a transformed pointer
            // Handle both direct refs (e.g., `e`) and expressions (e.g., `buf + len`)
            const DeclRefExpr *OtherDRE = dyn_cast<DeclRefExpr>(OtherSide);
            if (!OtherDRE) {
                // Try to find the pointer in a BinaryOperator (e.g., arr + n)
                if (const BinaryOperator *AddBO = dyn_cast<BinaryOperator>(OtherSide)) {
                    OtherDRE = dyn_cast<DeclRefExpr>(AddBO->getLHS()->IgnoreParenImpCasts());
                }
            }
            if (!OtherDRE) continue;
            const VarDecl *OtherVD = dyn_cast<VarDecl>(OtherDRE->getDecl());
            if (!OtherVD || modes.find(OtherVD) == modes.end())
                continue;
            // Both pointers will be transformed. Use pointer reconstruction:
            // base + index <op> other_base + other_index
            auto &other_cand = analysis.tracked_pointers[OtherVD];
            std::string other_base = other_cand.base_array_text;
            std::string other_idx = indexNameFor(OtherVD);
            std::string rhs = other_base.empty() ?
                other_idx : other_base + " + " + other_idx;
            acc.field_name = candidate.base_array_text;
            acc.operand_text = rhs;
        }
    }
}

// Step 5. An offset is source text snapshotted at collect time and pasted
// back out. If it names another pointer that is also being rewritten, that
// text describes a variable which will not exist, so this pointer has to be
// dropped rather than emitted against a stale name.
void FunctionAccessAnalyzer::rejectStaleOffsets(FunctionAnalysis &analysis,
                                                TransformModeMap &modes,
                                                ASTContext &Ctx) {
    for (auto &pair : analysis.accesses) {
        const VarDecl *PtrVar = pair.first;
        if (modes.find(PtrVar) == modes.end())
            continue;
        for (const auto &acc : pair.second) {
            if (acc.kind != PointerAccessKind::InitArrayOffset &&
                acc.kind != PointerAccessKind::AssignAddrOf &&
                acc.kind != PointerAccessKind::AssignArrayOffset)
                continue;
            // Check if any other transformed pointer appears in the
            // init expression's subtree
            const Stmt *InitStmt = acc.enclosing_stmt;
            if (!InitStmt) {
                // For declarations, use the VarDecl's init
                if (PtrVar->hasInit())
                    InitStmt = PtrVar->getInit();
            }
            if (!InitStmt && acc.expr) {
                // For separate assignments (AssignAddrOf, AssignArrayOffset),
                // walk up from the DeclRefExpr to find the BinaryOperator
                // and check its RHS for references to other transformed ptrs
                const Stmt *P = skipTransparentParents(acc.expr, Ctx);
                if (const BinaryOperator *BO = dyn_cast_or_null<BinaryOperator>(P))
                    InitStmt = BO->getRHS();
            }
            if (!InitStmt) continue;
            bool has_conflict = false;
            // Walk the init to find DeclRefExprs to other transformed pointers
            std::function<void(const Stmt *)> checkRefs = [&](const Stmt *S) {
                if (has_conflict || !S) return;
                if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(S)) {
                    if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                        if (VD != PtrVar && modes.count(VD))
                            has_conflict = true;
                    }
                }
                for (const Stmt *Child : S->children())
                    checkRefs(Child);
            };
            checkRefs(InitStmt);
            if (has_conflict) {
                modes.erase(PtrVar);
                break;
            }
        }
    }
}

// Step 6. Both groups consult the verdict. transformPointerVar re-validates,
// which catches a pointer validation itself rejected — but not one that
// rejectStaleOffsets dropped, because that step's reason (a conflict with
// another pointer's rewrite) is not a fact validation can see. The map is the
// only place that knowledge lives.
void FunctionAccessAnalyzer::emitPointerRewrites(
    const FunctionDecl *FD, FunctionAnalysis &analysis,
    const TransformModeMap &modes, const EditOrder &order, ASTContext &Ctx) {
    for (const VarDecl *PtrVar : order.param_bounded) {
        if (modes.find(PtrVar) == modes.end())
            continue;
        auto &access_list = analysis.accesses[PtrVar];
        auto &candidate = analysis.tracked_pointers[PtrVar];
        transformPointerVar(FD, PtrVar, candidate, access_list, Ctx);
    }

    for (const VarDecl *PtrVar : order.rest) {
        if (modes.find(PtrVar) == modes.end())
            continue;
        auto &access_list = analysis.accesses[PtrVar];
        auto &candidate = analysis.tracked_pointers[PtrVar];
        transformPointerVar(FD, PtrVar, candidate, access_list, Ctx);
    }
}

void FunctionAccessAnalyzer::transformFunction(const FunctionDecl *FD,
                                               FunctionAnalysis &analysis,
                                               ASTContext &Ctx) {
    assignIndexNamesInSourceOrder(analysis, Ctx);

    EditOrder order = pointersInEditOrder(FD, analysis);

    TransformModeMap modes = decideTransformModes(analysis, Ctx);

    resolveCrossPointerComparisons(analysis, modes, Ctx);

    rejectStaleOffsets(analysis, modes, Ctx);

    emitPointerRewrites(FD, analysis, modes, order, Ctx);
}

void FunctionAccessAnalyzer::transformAllFunctions(ASTContext &Ctx) {
    for (auto &[FDCanon, analysis] : g_function_analyses) {
        const FunctionDecl *FD = analysis.FD;
        if (!FD || !FD->hasBody())
            continue;

        // Overlap tracking is per function: the ranges recorded while
        // rewriting one function say nothing about the next.
        m_edited_ranges.clear();

        transformFunction(FD, analysis, Ctx);
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
                                                  ASTContext &Ctx) {
    if (accesses.empty())
        return;

    printAccesses(PtrVar, accesses, Ctx);

    std::string error;
    if (validatePointerCandidate(PtrVar, candidate, accesses, Ctx, error) ==
        TransformMode::Reject) {
        gLog.error = error;
        logFailedPointer(PtrVar, Ctx, error);
        if (VERBOSE)
            llvm::outs() << "[Skip] " << PtrVar->getNameAsString() << ": " << error << "\n";
        return;
    }

    g_pointers_found++;

    if (generateTransformation(FD, PtrVar, candidate, accesses, Ctx)) {
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

        // Record the transformed pointer in the metadata side-file so
        // the slice pass knows which index variables exist and what
        // they index into. Identity only — offset bounds are derived by
        // the slice pass from the rewritten AST.
        if (FD) {
            if (xj::PtrIndexFunctionRecord *fnRec = metadataRecordFor(FD, Ctx)) {
                xj::PtrIndexPointerRecord rec;
                rec.name = PtrVar->getNameAsString();
                rec.index_var = indexNameFor(PtrVar);
                rec.param_index = -1;
                if (const auto *PD = dyn_cast<ParmVarDecl>(PtrVar))
                    rec.param_index = static_cast<int>(PD->getFunctionScopeIndex());
                rec.base_text = candidate.base_array_text;
                fnRec->pointers.push_back(std::move(rec));
            }
        }
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
            unsigned eStart = e.offset;
            unsigned eEnd = SM.getFileOffset(e.end);
            bool overlaps = false;
            for (const auto &r : m_edited_ranges) {
                if (eStart < r.second && eEnd > r.first) {
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
            m_edited_ranges.push_back({e.offset, SM.getFileOffset(e.end)});
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
