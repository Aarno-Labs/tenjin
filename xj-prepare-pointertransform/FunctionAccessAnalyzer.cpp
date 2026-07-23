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

#include "llvm/Support/Path.h"

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

            xj::PtrIndexPointerRecord rec;
            rec.name = VD->getNameAsString();
            rec.index_var = rec.name + "_index_xj";
            rec.param_index = -1;
            rec.moved = true;
            rec.base_text = state.candidate.base_array_text;
            rec.min_offset = state.candidate.min_relative_offset;
            rec.max_offset = state.candidate.max_relative_offset;
            rec.variable_offsets = !state.candidate.constant_offsets;
            g_metadata.globals.push_back(std::move(rec));
        }
    }
}

// ============================================================================
// transformAllFunctions — rewrite every local pointer that's safe to
// rewrite, function by function, in plain (base-param-relative) form.
// ============================================================================

void FunctionAccessAnalyzer::transformAllFunctions(ASTContext &Ctx) {
    for (auto &[FDCanon, analysis] : g_function_analyses) {
        const FunctionDecl *FD = analysis.FD;
        if (!FD || !FD->hasBody())
            continue;

        m_edited_ranges.clear();

        // Two-pass edit ordering: pointers whose bound comparison
        // resolves against a parameter are rewritten first, so that when
        // two pointers' comparison rewrites overlap, the param-bounded
        // form wins (overlapping later edits are dropped in applyEdits).
        std::vector<const VarDecl *> rust_slice_candidates;
        std::vector<const VarDecl *> other_pointers;

        for (auto &pair : analysis.accesses) {
            const VarDecl *PtrVar = pair.first;
            auto &candidate = analysis.tracked_pointers[PtrVar];
            auto &access_list = pair.second;

            bool is_rs_candidate = false;
            if (!candidate.is_parameter && FD->getNumParams() > 0) {
                bool base_is_param = false;
                for (unsigned i = 0; i < FD->getNumParams(); i++) {
                    if (FD->getParamDecl(i)->getNameAsString() == candidate.base_array_text &&
                        FD->getParamDecl(i)->getType()->isPointerType()) {
                        base_is_param = true;
                        break;
                    }
                }
                if (base_is_param) {
                    for (const auto &acc : access_list) {
                        if (acc.kind == PointerAccessKind::ComparisonExpr) {
                            is_rs_candidate = true;
                            break;
                        }
                    }
                }
            }

            if (is_rs_candidate)
                rust_slice_candidates.push_back(PtrVar);
            else
                other_pointers.push_back(PtrVar);
        }

        // Pre-validation: determine which pointers will actually be
        // transformed so that cross-pointer comparisons can be resolved.
        std::set<const VarDecl *> will_transform;
        for (auto &pair : analysis.accesses) {
            const VarDecl *PtrVar = pair.first;
            auto &candidate = analysis.tracked_pointers[PtrVar];
            auto &access_list = pair.second;
            std::string error;
            if (validatePointerCandidate(PtrVar, candidate, access_list, Ctx, error))
                will_transform.insert(PtrVar);
        }

        // Fix up ComparisonExpr accesses that reference another pointer
        // which will also be transformed. Replace operand_text with the
        // reconstructed pointer form: other_base + other_name_index.
        for (auto &pair : analysis.accesses) {
            const VarDecl *PtrVar = pair.first;
            auto &candidate = analysis.tracked_pointers[PtrVar];
            if (will_transform.find(PtrVar) == will_transform.end())
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
                if (!OtherVD || will_transform.find(OtherVD) == will_transform.end())
                    continue;
                // Both pointers will be transformed. Use pointer reconstruction:
                // base + index <op> other_base + other_index
                auto &other_cand = analysis.tracked_pointers[OtherVD];
                std::string other_base = other_cand.base_array_text;
                std::string other_idx = OtherVD->getNameAsString() + "_index_xj";
                std::string rhs = other_base.empty() ?
                    other_idx : other_base + " + " + other_idx;
                acc.field_name = candidate.base_array_text;
                acc.operand_text = rhs;
            }
        }

        // Reject pointers whose init/assign offset references another
        // pointer that will also be transformed. The init edit would use
        // stale source text for the offset, conflicting with the inner
        // pointer's transformation.
        for (auto &pair : analysis.accesses) {
            const VarDecl *PtrVar = pair.first;
            if (will_transform.find(PtrVar) == will_transform.end())
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
                            if (VD != PtrVar && will_transform.count(VD))
                                has_conflict = true;
                        }
                    }
                    for (const Stmt *Child : S->children())
                        checkRefs(Child);
                };
                checkRefs(InitStmt);
                if (has_conflict) {
                    will_transform.erase(PtrVar);
                    break;
                }
            }
        }

        // First pass: param-bounded pointers
        for (const VarDecl *PtrVar : rust_slice_candidates) {
            auto &access_list = analysis.accesses[PtrVar];
            auto &candidate = analysis.tracked_pointers[PtrVar];
            transformPointerVar(FD, PtrVar, candidate, access_list, Ctx);
        }

        // Second pass: remaining pointers. Skip pointers removed from
        // will_transform by init conflict detection.
        for (const VarDecl *PtrVar : other_pointers) {
            auto &access_list = analysis.accesses[PtrVar];
            auto &candidate = analysis.tracked_pointers[PtrVar];
            if (will_transform.find(PtrVar) == will_transform.end())
                continue;
            transformPointerVar(FD, PtrVar, candidate, access_list, Ctx);
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
                                                  ASTContext &Ctx) {
    if (accesses.empty())
        return;

    printAccesses(PtrVar, accesses, Ctx);

    std::string error;
    if (!validatePointerCandidate(PtrVar, candidate, accesses, Ctx, error)) {
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
        // they index into.
        if (FD) {
            if (xj::PtrIndexFunctionRecord *fnRec = metadataRecordFor(FD, Ctx)) {
                xj::PtrIndexPointerRecord rec;
                rec.name = PtrVar->getNameAsString();
                rec.index_var = rec.name + "_index_xj";
                rec.param_index = -1;
                if (const auto *PD = dyn_cast<ParmVarDecl>(PtrVar))
                    rec.param_index = static_cast<int>(PD->getFunctionScopeIndex());
                rec.moved = true;
                rec.base_text = candidate.base_array_text;
                rec.min_offset = candidate.min_relative_offset;
                rec.max_offset = candidate.max_relative_offset;
                rec.variable_offsets = !candidate.constant_offsets;
                fnRec->pointers.push_back(std::move(rec));
            }
        }
    }
}

// Return the metadata record for `FD`, creating it (with the right
// source file stamped) on first use. Returns nullptr when a function of
// the same name from a *different* file already claimed the record —
// uniquify_statics runs after this pass, so distinct static functions
// can still share a name here.
xj::PtrIndexFunctionRecord *
FunctionAccessAnalyzer::metadataRecordFor(const FunctionDecl *FD, ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    std::string name = FD->getNameAsString();
    std::string file;
    if (auto FE = SM.getFileEntryRefForID(
            SM.getFileID(SM.getSpellingLoc(FD->getLocation()))))
        file = llvm::sys::path::filename(FE->getName()).str();

    auto it = g_metadata.functions.find(name);
    if (it == g_metadata.functions.end()) {
        xj::PtrIndexFunctionRecord rec;
        rec.file = file;
        it = g_metadata.functions.emplace(name, std::move(rec)).first;
    } else if (it->second.file != file) {
        return nullptr;
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
