// FunctionAccessAnalyzer.cpp — see FunctionAccessAnalyzer.h for the
// high-level pipeline. Code below is grouped into:
//
//   1. Driver: constructor, run() (per-function analysis), and
//      onEndOfTranslationUnit() (the phase orchestrator).
//   2. Per-function transform pipeline — the eight steps that turn one
//      function's analysis snapshot into index rewrites. See the comment
//      on transformFunction for the algorithm.
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
        // Globals are collapse-only: generateGlobalTransformation always
        // deletes the declaration and substitutes base_array_text at every
        // access, and has no case for AssignPtr. A Handle verdict is the
        // validator saying that substitution is *not* sound — an unstable
        // or type-punned base, or a reseat — so it has to skip here. Testing
        // for Reject instead would let exactly those candidates through to
        // collapse codegen with the base just judged unsafe. Handle mode for
        // file-scope pointers is deferred (they bypass transformPointerVar
        // and emit no metadata).
        if (validatePointerCandidate(VD, state.candidate, state.accesses,
                                     Ctx, error) != TransformMode::Collapse) {
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
// Per-function transform pipeline — rewrite every local pointer that's safe
// to rewrite, function by function, in plain (base-param-relative) form.
// ============================================================================
//
// The steps below run in the order they are defined. Those that need no
// member state are file-static and declared here as a table of contents;
// the rest are members (they call validatePointerCandidate,
// logFailedPointer or transformPointerVar).

// Step 1: hand out index names, in source order.
static void assignIndexNamesInSourceOrder(const FunctionAnalysis &analysis,
                                          ASTContext &Ctx);

// Step 2: split the pointers into the two edit-ordering passes.
static void partitionByEditPriority(const FunctionDecl *FD,
                                    FunctionAnalysis &analysis,
                                    std::vector<const VarDecl *> &param_bounded,
                                    std::vector<const VarDecl *> &other_pointers);

// Step 4: restate comparisons that name another transformed pointer.
static void resolveCrossPointerComparisons(FunctionAnalysis &analysis,
                                           const TransformModeMap &modes,
                                           ASTContext &Ctx);

// Step 6: turn suppressed references into explicit `(base + index)` reads.
static void materializeSuppressedRefs(const FunctionDecl *FD,
                                      FunctionAnalysis &analysis,
                                      TransformModeMap &modes,
                                      const std::set<const VarDecl *> &inherited);

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

// Rewrite one function's pointers, working from the analysis snapshot run()
// left in g_function_analyses.
//
// The pipeline:
//
//   1. Name indices — every tracked pointer gets its `_index_xj` name.
//   2. Partition by edit priority — param-bounded pointers first.
//   3. Decide modes — judge each pointer once into Collapse/Handle/Reject.
//   4. Resolve cross-pointer comparisons — restate an operand naming
//      another transformed pointer in index form.
//   5. Inherit indices — `T *p = q + x` with q collapsed: p adopts q's base
//      and picks up q's index.
//   6. Materialize suppressed references — a NoRewrite reference whose
//      owner did not inherit has to say `(base + index)` after all.
//   7. Reject stale offsets — drop pointers whose pasted offset text the
//      rewrite would invalidate.
//   8. Emit — rewrite the survivors, param-bounded pass first.
//
// Steps 4-7 mutate the access records and the mode map; 1-2 only read.
//
// Ordering constraint: step 2 must precede step 5. The partition reads
// `base_array_text` and index inheritance overwrites it, so partitioning
// afterwards would sort pointers into the wrong emit pass.
void FunctionAccessAnalyzer::transformFunction(const FunctionDecl *FD,
                                               FunctionAnalysis &analysis,
                                               ASTContext &Ctx) {
    assignIndexNamesInSourceOrder(analysis, Ctx);

    std::vector<const VarDecl *> param_bounded;
    std::vector<const VarDecl *> other_pointers;
    partitionByEditPriority(FD, analysis, param_bounded, other_pointers);

    TransformModeMap modes = decideTransformModes(analysis, Ctx);

    resolveCrossPointerComparisons(analysis, modes, Ctx);

    std::set<const VarDecl *> inherited = inheritIndices(analysis, modes, Ctx);

    materializeSuppressedRefs(FD, analysis, modes, inherited);

    rejectStaleOffsets(analysis, modes, Ctx);

    emitPointerRewrites(FD, analysis, modes, param_bounded, Ctx);
    emitPointerRewrites(FD, analysis, modes, other_pointers, Ctx);
}

// Step 1. Name every index up front, in source order so the assignment is
// reproducible. Sorting by location rather than iterating the map matters:
// the map is keyed by VarDecl address, which is allocation order and not a
// property of the source.
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

// Step 2. Two-pass edit ordering: pointers whose bound comparison resolves
// against a parameter are rewritten first, so that when two pointers'
// comparison rewrites overlap, the param-bounded form wins (overlapping
// later edits are dropped in applyEdits).
static void partitionByEditPriority(const FunctionDecl *FD,
                                    FunctionAnalysis &analysis,
                                    std::vector<const VarDecl *> &param_bounded,
                                    std::vector<const VarDecl *> &other_pointers) {
    for (auto &pair : analysis.accesses) {
        const VarDecl *PtrVar = pair.first;
        auto &candidate = analysis.tracked_pointers[PtrVar];
        auto &access_list = pair.second;

        bool is_param_bounded = false;
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
                        is_param_bounded = true;
                        break;
                    }
                }
            }
        }

        if (is_param_bounded)
            param_bounded.push_back(PtrVar);
        else
            other_pointers.push_back(PtrVar);
    }
}

// Step 3. Pre-validation: determine which pointers will actually be
// transformed, and in which mode, so that cross-pointer comparisons can be
// resolved.
//
// Each pointer is judged exactly once and the verdict carried to the
// rewrite. Re-validating later would re-judge a candidate the fixups
// downstream have since altered, and the modes are not interchangeable: a
// pointer demoted to Handle for an unstable base looks perfectly
// collapsible once its base has been rewritten to its own name, and
// collapse would then delete the declaration out from under its accesses.
//
// The two later re-validations (in inheritIndices) are the exception, and
// sound for the reason given there: the candidate changed because the
// fixup changed it.
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
        if (mode != TransformMode::Reject) {
            modes[PtrVar] = mode;
        } else {
            gLog.error = error;
            logFailedPointer(PtrVar, Ctx, error);
            if (VERBOSE)
                llvm::outs() << "[Skip] " << PtrVar->getNameAsString()
                             << ": " << error << "\n";
        }
    }
    return modes;
}

// The pointer on the far side of the comparison `acc` sits in, if that
// pointer is also being transformed. Null otherwise.
//
// Handles both a direct reference (`p < e`) and one inside an addition
// (`p < buf + len`).
static const VarDecl *otherTransformedPointerIn(const PointerAccess &acc,
                                                const VarDecl *Self,
                                                const TransformModeMap &modes,
                                                ASTContext &Ctx) {
    const Stmt *P = skipTransparentParents(acc.expr, Ctx);
    const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
    if (!BO)
        return nullptr;

    // Whichever side isn't `Self` is the side to inspect.
    const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
    const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
    const Expr *OtherSide = nullptr;
    if (const DeclRefExpr *LDRE = dyn_cast<DeclRefExpr>(LHS)) {
        if (LDRE->getDecl() == Self) OtherSide = RHS;
    }
    if (!OtherSide) {
        if (const DeclRefExpr *RDRE = dyn_cast<DeclRefExpr>(RHS)) {
            if (RDRE->getDecl() == Self) OtherSide = LHS;
        }
    }
    if (!OtherSide)
        return nullptr;

    const DeclRefExpr *OtherDRE = dyn_cast<DeclRefExpr>(OtherSide);
    if (!OtherDRE) {
        // Try to find the pointer in a BinaryOperator (e.g., arr + n)
        if (const BinaryOperator *AddBO = dyn_cast<BinaryOperator>(OtherSide)) {
            OtherDRE = dyn_cast<DeclRefExpr>(AddBO->getLHS()->IgnoreParenImpCasts());
        }
    }
    if (!OtherDRE)
        return nullptr;

    const VarDecl *OtherVD = dyn_cast<VarDecl>(OtherDRE->getDecl());
    if (!OtherVD || !modes.count(OtherVD))
        return nullptr;
    return OtherVD;
}

// Step 4. Fix up ComparisonExpr accesses that reference another pointer
// which will also be transformed. Replace operand_text with the
// reconstructed pointer form: other_base + other_name_index.
static void resolveCrossPointerComparisons(FunctionAnalysis &analysis,
                                           const TransformModeMap &modes,
                                           ASTContext &Ctx) {
    for (auto &pair : analysis.accesses) {
        const VarDecl *PtrVar = pair.first;
        auto &candidate = analysis.tracked_pointers[PtrVar];
        if (!modes.count(PtrVar))
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
            const VarDecl *OtherVD =
                otherTransformedPointerIn(acc, PtrVar, modes, Ctx);
            if (!OtherVD)
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
}

// The pointer `candidate`'s base is derived from, if that pointer is also
// being transformed. Null otherwise.
//
// Resolve the source from the base *expression*, not from its spelling.
// `base_array` is the node the base was extracted from — for `T *p = q + 1`
// the DeclRefExpr for q — so the decl is already in hand. Matching
// `base_array_text` against pointer names instead would pick whichever
// same-named pointer the map yields first, which for two `q`s in nested or
// sibling scopes is the wrong one: p would inherit an index belonging to a
// different variable, or one whose block has already closed.
//
// This is not a widening. An explicit cast in the base (`(int *)q + 1`)
// survives IgnoreParenImpCasts and fails the dyn_cast, exactly as its
// spelling failed the name comparison; and base_array is non-null whenever
// base_array_text is, since every write sets both.
static const VarDecl *inheritanceSourceFor(const PointerCandidate &candidate,
                                           const VarDecl *Self,
                                           const TransformModeMap &modes) {
    if (const Expr *Base = candidate.base_array) {
        if (const auto *DRE =
                dyn_cast<DeclRefExpr>(Base->IgnoreParenImpCasts())) {
            if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                if (VD != Self && VD->getType()->isPointerType() &&
                    modes.count(VD))
                    return VD;
            }
        }
    }
    return nullptr;
}

// Step 5. Index inheritance: `T *p = q + x;` where q is itself rewritten
// as an index. p's position is q's position plus x, so p takes over q's
// base and its initial index becomes `q_index_xj + (x)`.
//
// Which base p adopts depends on how q was rewritten: a collapsed q has
// been deleted, so p must name whatever q indexed into; a frozen q is
// still there and is its own base. Either way the offset carries q's
// index, which is the whole point — reading `q + x` verbatim after q stops
// moving would capture q's starting position rather than its current one.
//
// Re-validating p afterwards is sound here, unlike re-validating to
// *decide* a mode: the candidate changed because this fixup changed it,
// and p is now judged against its real base (`buf`, stable) instead of a
// moving pointer, which is what lets it collapse at all.
//
// Returns the pointers that actually inherited — step 6 needs to know,
// since inheriting is exactly what consumes the source's reference.
std::set<const VarDecl *>
FunctionAccessAnalyzer::inheritIndices(FunctionAnalysis &analysis,
                                       TransformModeMap &modes,
                                       ASTContext &Ctx) {
    std::set<const VarDecl *> inherited;
    for (auto &pair : analysis.accesses) {
        const VarDecl *PtrVar = pair.first;
        if (!modes.count(PtrVar))
            continue;
        auto &candidate = analysis.tracked_pointers[PtrVar];
        if (candidate.base_array_text.empty())
            continue;

        const VarDecl *Src = inheritanceSourceFor(candidate, PtrVar, modes);
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
        if (modes.at(Src) != TransformMode::Collapse) {
            // The owner must not collapse onto the source: the source
            // is frozen but still moves *logically*, so substituting
            // its name at each access would read a position that
            // drifts. Force the handle and re-judge. The source's
            // reference in this initializer is materialized by step 6,
            // which covers every reference no wholesale rewrite is
            // going to consume.
            candidate.collapse_ineligible = true;
            std::string error;
            TransformMode remode = validatePointerCandidate(
                PtrVar, candidate, pair.second, Ctx, error);
            if (remode == TransformMode::Reject) {
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
            modes.erase(PtrVar);
        } else {
            modes[PtrVar] = remode;
            inherited.insert(PtrVar);
        }
    }
    return inherited;
}

// Step 6. Materialize every reference that no wholesale rewrite is going
// to consume.
//
// A reference is suppressed as NoRewrite on the assumption that the
// pointer owning the enclosing declaration replaces that declaration
// outright, which happens exactly when the owner inherits this pointer's
// index. Suppression is only about edit overlap: an inner replacement
// inside text that is itself being replaced would be dropped by
// applyEdits, taking the wholesale rewrite with it.
//
// When the owner does not inherit, the declaration survives and the
// reference has to say what it always meant — the pointer's value,
// `(base + index)`. Materializing is always sound, for a collapsed pointer
// as much as a frozen one. The only reason this used to drop the pointer
// instead is that MaterializeUse did not exist yet; dropping cost every
// second link of a derivation chain its rewrite.
static void materializeSuppressedRefs(const FunctionDecl *FD,
                                      FunctionAnalysis &analysis,
                                      TransformModeMap &modes,
                                      const std::set<const VarDecl *> &inherited) {
    for (auto &pair : analysis.accesses) {
        const VarDecl *PtrVar = pair.first;
        if (!modes.count(PtrVar))
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
                modes.at(PtrVar) == TransformMode::Collapse) {
                modes.erase(PtrVar);
                break;
            }

            acc.kind = PointerAccessKind::MaterializeUse;
        }
    }
}

// Step 7. Reject pointers whose init/assign offset names another pointer
// that is also being transformed.
//
// The offset is source text snapshotted at collect time and pasted
// back out at edit time, so a name in it that the rewrite invalidates
// is pasted stale — and a collapsed pointer's declaration is deleted
// outright, leaving text that names nothing. The pointer is dropped
// rather than rewritten wrong.
//
// Only the *offset* is pasted, so only the offset is checked.
// `offset_expr` is the node the text came from, which is the whole
// question: which expression to look at is a property of the access,
// not of the pointer, and reconstructing it from the pointer got both
// available answers wrong. Checking the enclosing initializer instead
// also swept in the base, whose reference index inheritance already
// handles — the false positives that produced were then suppressed by
// an exemption that trusted an inner MaterializeUse rewrite to survive
// a wholesale replacement of the text containing it. It does not:
// applyEdits drops the overlapping inner edit. Narrowing to the offset
// retires that exemption and the `inherited` one together.
//
// A null offset_expr means the text was synthesized, not copied — the
// inheritance fixup's `q_index_xj + (...)`, which names an index
// variable and cannot go stale. Nothing to check.
void FunctionAccessAnalyzer::rejectStaleOffsets(FunctionAnalysis &analysis,
                                                TransformModeMap &modes,
                                                ASTContext &Ctx) {
    for (auto &pair : analysis.accesses) {
        const VarDecl *PtrVar = pair.first;
        if (!modes.count(PtrVar))
            continue;
        for (const auto &acc : pair.second) {
            if (acc.kind != PointerAccessKind::InitArrayOffset &&
                acc.kind != PointerAccessKind::AssignAddrOf &&
                acc.kind != PointerAccessKind::AssignArrayOffset)
                continue;
            if (!acc.offset_expr)
                continue;
            const DeclRefExpr *Conflict =
                findRefIf(acc.offset_expr, [&](const Decl *D) {
                    const auto *VD = dyn_cast<VarDecl>(D);
                    return VD && VD != PtrVar && modes.count(VD);
                });
            if (Conflict) {
                logFailedPointer(PtrVar, Ctx,
                                 "offset names '" +
                                     Conflict->getDecl()->getNameAsString() +
                                     "', which is also being rewritten, so "
                                     "the offset text would go stale");
                modes.erase(PtrVar);
                break;
            }
        }
    }
}

// Step 8. Rewrite the pointers in `order`, skipping those that did not
// survive into `modes`.
//
// Called once per edit-ordering pass. Absence from `modes` covers every
// way a pointer can have been dropped — rejection at step 3, re-judging
// during inheritance, and stale-offset detection alike — so this one
// guard is the whole eligibility test.
void FunctionAccessAnalyzer::emitPointerRewrites(
    const FunctionDecl *FD, FunctionAnalysis &analysis,
    const TransformModeMap &modes, const std::vector<const VarDecl *> &order,
    ASTContext &Ctx) {
    for (const VarDecl *PtrVar : order) {
        auto mit = modes.find(PtrVar);
        if (mit == modes.end())
            continue;
        auto &access_list = analysis.accesses[PtrVar];
        auto &candidate = analysis.tracked_pointers[PtrVar];
        transformPointerVar(FD, PtrVar, candidate, access_list, Ctx, mit->second);
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
