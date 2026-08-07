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
        // Globals stay on the collapse path for now; handle mode for
        // file-scope pointers is deferred (they bypass transformPointerVar
        // and emit no metadata).
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
// transformAllFunctions — rewrite every local pointer that's safe to
// rewrite, function by function, in plain (base-param-relative) form.
// ============================================================================

void FunctionAccessAnalyzer::transformAllFunctions(ASTContext &Ctx) {
    for (auto &[FDCanon, analysis] : g_function_analyses) {
        const FunctionDecl *FD = analysis.FD;
        if (!FD || !FD->hasBody())
            continue;

        m_edited_ranges.clear();

        // Name every index up front, in source order so the assignment is
        // reproducible. Sorting by location rather than iterating the map
        // matters: the map is keyed by VarDecl address, which is
        // allocation order and not a property of the source.
        {
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
        // transformed, and in which mode, so that cross-pointer
        // comparisons can be resolved.
        //
        // Each pointer is judged exactly once and the verdict carried to
        // the rewrite. Re-validating later would re-judge a candidate the
        // fixups below have since altered, and the modes are not
        // interchangeable: a pointer demoted to Handle for an unstable
        // base looks perfectly collapsible once its base has been
        // rewritten to its own name, and collapse would then delete the
        // declaration out from under its accesses.
        std::set<const VarDecl *> will_transform;
        std::map<const VarDecl *, TransformMode> modes;
        for (auto &pair : analysis.accesses) {
            const VarDecl *PtrVar = pair.first;
            auto &candidate = analysis.tracked_pointers[PtrVar];
            auto &access_list = pair.second;
            std::string error;
            TransformMode mode =
                validatePointerCandidate(PtrVar, candidate, access_list, Ctx, error);
            if (mode != TransformMode::Reject) {
                will_transform.insert(PtrVar);
                modes[PtrVar] = mode;
            } else {
                gLog.error = error;
                logFailedPointer(PtrVar, Ctx, error);
                if (VERBOSE)
                    llvm::outs() << "[Skip] " << PtrVar->getNameAsString()
                                 << ": " << error << "\n";
            }
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
                std::string other_idx = indexNameFor(OtherVD);

                // Same base on both sides: the pointers cancel and the
                // comparison is between the two indices. Leaving
                // field_name empty selects the index form downstream.
                if (!other_base.empty() &&
                    other_base == candidate.base_array_text) {
                    acc.field_name.clear();
                    acc.operand_text = other_idx;
                    continue;
                }

                std::string rhs = other_base.empty() ?
                    other_idx : other_base + " + " + other_idx;
                acc.field_name = candidate.base_array_text;
                acc.operand_text = rhs;
            }
        }

        // Index inheritance: `T *p = q + x;` where q is itself rewritten
        // as an index. p's position is q's position plus x, so p takes
        // over q's base and its initial index becomes `q_index_xj + (x)`.
        //
        // Which base p adopts depends on how q was rewritten: a collapsed
        // q has been deleted, so p must name whatever q indexed into; a
        // frozen q is still there and is its own base. Either way the
        // offset carries q's index, which is the whole point — reading
        // `q + x` verbatim after q stops moving would capture q's
        // starting position rather than its current one.
        //
        // Re-validating p afterwards is sound here, unlike re-validating
        // to *decide* a mode: the candidate changed because this fixup
        // changed it, and p is now judged against its real base (`buf`,
        // stable) instead of a moving pointer, which is what lets it
        // collapse at all.
        std::set<const VarDecl *> inherited;
        for (auto &pair : analysis.accesses) {
            const VarDecl *PtrVar = pair.first;
            if (!will_transform.count(PtrVar))
                continue;
            auto &candidate = analysis.tracked_pointers[PtrVar];
            if (candidate.base_array_text.empty())
                continue;

            // Resolve the source from the base *expression*, not from its
            // spelling. `base_array` is the node the base was extracted
            // from — for `T *p = q + 1` the DeclRefExpr for q — so the
            // decl is already in hand. Matching `base_array_text` against
            // pointer names instead would pick whichever same-named
            // pointer the map yields first, which for two `q`s in nested
            // or sibling scopes is the wrong one: p would inherit an index
            // belonging to a different variable, or one whose block has
            // already closed.
            //
            // This is not a widening. An explicit cast in the base
            // (`(int *)q + 1`) survives IgnoreParenImpCasts and fails the
            // dyn_cast, exactly as its spelling failed the name
            // comparison; and base_array is non-null whenever
            // base_array_text is, since every write sets both.
            const VarDecl *Src = nullptr;
            if (const Expr *Base = candidate.base_array) {
                if (const auto *DRE =
                        dyn_cast<DeclRefExpr>(Base->IgnoreParenImpCasts())) {
                    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                        if (VD != PtrVar && VD->getType()->isPointerType() &&
                            will_transform.count(VD))
                            Src = VD;
                    }
                }
            }
            if (!Src)
                continue;

            // Only a collapsed source can be *inherited* from. A frozen
            // source keeps its declaration and its value is its base — its
            // position lives in its index — so `p1 = p0 + 1` read verbatim
            // means base + 1 rather than base + p0_index_xj + 1, placing
            // p1 wherever p0 started instead of where it had reached.
            //
            // Such a source is materialized instead: its reference in the
            // initializer becomes `(p0 + p0_index_xj)`, which is what
            // every other context reading a frozen pointer's value already
            // does — call arguments via PassedToFunc, comparisons via
            // ComparisonExpr, returns via ReturnPtr. This position is the
            // odd one out only because the reference was suppressed as
            // NoRewrite, on the assumption that the enclosing declaration
            // is rewritten wholesale; true of a collapsed source, false of
            // a frozen one. Both pointers then transform independently:
            // there is no index to inherit, because the owner starts
            // wherever that materialized expression points.
            if (modes[Src] != TransformMode::Collapse) {
                // The owner must not collapse onto the source: the source
                // is frozen but still moves *logically*, so substituting
                // its name at each access would read a position that
                // drifts. Force the handle and re-judge. The source's
                // reference in this initializer is materialized by the
                // sweep below, which covers every reference no wholesale
                // rewrite is going to consume.
                candidate.collapse_ineligible = true;
                std::string error;
                TransformMode remode = validatePointerCandidate(
                    PtrVar, candidate, pair.second, Ctx, error);
                if (remode == TransformMode::Reject) {
                    will_transform.erase(PtrVar);
                    modes.erase(PtrVar);
                } else {
                    modes[PtrVar] = remode;
                }
                continue;
            }

            const std::string src_index = indexNameFor(Src);
            auto &src_cand = analysis.tracked_pointers[Src];

            bool changed = false;
            for (auto &acc : pair.second) {
                switch (acc.kind) {
                case PointerAccessKind::InitArray:
                case PointerAccessKind::AssignArray:
                    // `p = q` — p starts exactly where q is.
                    acc.kind = (acc.kind == PointerAccessKind::InitArray)
                                   ? PointerAccessKind::InitArrayOffset
                                   : PointerAccessKind::AssignArrayOffset;
                    acc.offset_text = src_index;
                    changed = true;
                    break;
                case PointerAccessKind::InitArrayOffset:
                case PointerAccessKind::AssignArrayOffset:
                    acc.offset_text = src_index + " + (" + acc.offset_text + ")";
                    changed = true;
                    break;
                default:
                    break;
                }
            }
            if (!changed)
                continue;

            candidate.base_array_text = src_cand.base_array_text;
            candidate.base_array = src_cand.base_array;

            std::string error;
            TransformMode remode = validatePointerCandidate(
                PtrVar, candidate, pair.second, Ctx, error);
            if (remode == TransformMode::Reject) {
                will_transform.erase(PtrVar);
                modes.erase(PtrVar);
            } else {
                modes[PtrVar] = remode;
                inherited.insert(PtrVar);
            }
        }

        // Materialize every reference that no wholesale rewrite is going
        // to consume.
        //
        // A reference is suppressed as NoRewrite on the assumption that
        // the pointer owning the enclosing declaration replaces that
        // declaration outright, which happens exactly when the owner
        // inherits this pointer's index. Suppression is only about edit
        // overlap: an inner replacement inside text that is itself being
        // replaced would be dropped by applyEdits, taking the wholesale
        // rewrite with it.
        //
        // When the owner does not inherit, the declaration survives and
        // the reference has to say what it always meant — the pointer's
        // value, `(base + index)`. Materializing is always sound, for a
        // collapsed pointer as much as a frozen one. The only reason this
        // used to drop the pointer instead is that MaterializeUse did not
        // exist yet; dropping cost every second link of a derivation
        // chain its rewrite.
        for (auto &pair : analysis.accesses) {
            const VarDecl *PtrVar = pair.first;
            if (!will_transform.count(PtrVar))
                continue;
            for (auto &acc : pair.second) {
                if (acc.kind != PointerAccessKind::NoRewrite)
                    continue;

                // The owner travels on the record, so both questions —
                // did it inherit, and which DeclStmt is it in — are
                // answered without searching by name.
                const VarDecl *Owner = acc.owner_ptr;
                if (Owner && inherited.count(Owner))
                    continue;

                // A collapsed pointer sharing a DeclStmt with its owner is
                // the one case materialization cannot serve: its index is
                // declared *after* the whole statement (the initializer
                // may depend on names bound within it, so it cannot move),
                // and the owner's initializer would reach it too early. A
                // frozen pointer's index now precedes the statement, so it
                // is fine.
                const DeclStmt *OwnDS =
                    Owner ? findDeclStmtForVar(Owner, FD->getBody()) : nullptr;
                const DeclStmt *SelfDS = findDeclStmtForVar(PtrVar, FD->getBody());
                if (OwnDS && OwnDS == SelfDS &&
                    modes[PtrVar] == TransformMode::Collapse) {
                    will_transform.erase(PtrVar);
                    modes.erase(PtrVar);
                    break;
                }

                acc.kind = PointerAccessKind::MaterializeUse;
            }
        }

        // Reject pointers whose init/assign offset references another
        // pointer that will also be transformed. The init edit would use
        // stale source text for the offset, conflicting with the inner
        // pointer's transformation. Pointers handled by the inheritance
        // fixup above are exempt: their offset no longer names the other
        // pointer, it names that pointer's index.
        for (auto &pair : analysis.accesses) {
            const VarDecl *PtrVar = pair.first;
            if (will_transform.find(PtrVar) == will_transform.end())
                continue;
            if (inherited.count(PtrVar))
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
                            if (VD != PtrVar && will_transform.count(VD)) {
                                // Not stale if that pointer's reference in
                                // this very initializer is being
                                // materialized: the text is rewritten to
                                // `(base + index)`, which is exactly the
                                // value the offset needs. Keyed on the
                                // owning decl — a same-named pointer's
                                // materialized reference is a different
                                // initializer and says nothing about this
                                // one.
                                bool materialized = false;
                                for (const auto &oacc : analysis.accesses[VD]) {
                                    if (oacc.kind == PointerAccessKind::MaterializeUse &&
                                        oacc.owner_ptr == PtrVar) {
                                        materialized = true;
                                        break;
                                    }
                                }
                                if (!materialized)
                                    has_conflict = true;
                            }
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
            auto mit = modes.find(PtrVar);
            if (mit == modes.end())
                continue;
            auto &access_list = analysis.accesses[PtrVar];
            auto &candidate = analysis.tracked_pointers[PtrVar];
            transformPointerVar(FD, PtrVar, candidate, access_list, Ctx, mit->second);
        }

        // Second pass: remaining pointers. Skip pointers removed from
        // will_transform by init conflict detection.
        for (const VarDecl *PtrVar : other_pointers) {
            auto mit = modes.find(PtrVar);
            if (mit == modes.end())
                continue;
            if (will_transform.find(PtrVar) == will_transform.end())
                continue;
            auto &access_list = analysis.accesses[PtrVar];
            auto &candidate = analysis.tracked_pointers[PtrVar];
            transformPointerVar(FD, PtrVar, candidate, access_list, Ctx, mit->second);
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
        if (access.owner_ptr)
            llvm::outs() << " owner=" << access.owner_ptr->getNameAsString();
        if (!access.subscript_text.empty())
            llvm::outs() << " subscript=" << access.subscript_text;
        if (!access.operand_text.empty())
            llvm::outs() << " operand=" << access.operand_text;
        llvm::outs() << "\n";
    }
}

// True if any access assigns to the pointer after its declaration.
//
// Under handle mode the pointer *is* the handle, so such an assignment
// survives verbatim into the output as a write to the handle itself.
// Under collapse mode it does not — the pointer is gone and the
// assignment has become an index update — which is why this only
// distinguishes the two handle modes. Initializers are not assignments:
// they establish the handle rather than move it.
static bool assignsPointerAfterDecl(const std::vector<PointerAccess> &accesses) {
    for (const auto &access : accesses) {
        switch (access.kind) {
        case PointerAccessKind::AssignPtr:
        case PointerAccessKind::AssignArray:
        case PointerAccessKind::AssignArrayOffset:
        case PointerAccessKind::AssignAddrOf:
        case PointerAccessKind::AssignNull:
        case PointerAccessKind::AssignFromAllowedFunc:
            return true;
        default:
            break;
        }
    }
    return false;
}

// Validate-and-rewrite one local pointer. Bumps the per-file counters
// and emits the [REPLACED] / [FAILED] log entries.
void FunctionAccessAnalyzer::transformPointerVar(const FunctionDecl *FD,
                                                  const VarDecl *PtrVar,
                                                  PointerCandidate &candidate,
                                                  std::vector<PointerAccess> &accesses,
                                                  ASTContext &Ctx,
                                                  TransformMode mode) {
    if (accesses.empty() || mode == TransformMode::Reject)
        return;

    printAccesses(PtrVar, accesses, Ctx);

    g_pointers_found++;

    if (generateTransformation(FD, PtrVar, candidate, accesses, Ctx, mode)) {
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
                // A frozen handle is its own base, and that is how the
                // rewritten source spells every access. Recording the
                // pre-transform base text would describe code that no
                // longer exists — the slice pass matches subscript bases
                // against this string.
                rec.base_text = (mode == TransformMode::Handle)
                                    ? rec.name
                                    : candidate.base_array_text;
                if (mode != TransformMode::Handle)
                    rec.mode = xj::PtrIndexMode::Collapse;
                else if (assignsPointerAfterDecl(accesses))
                    rec.mode = xj::PtrIndexMode::Reseated;
                else
                    rec.mode = xj::PtrIndexMode::Handle;
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
