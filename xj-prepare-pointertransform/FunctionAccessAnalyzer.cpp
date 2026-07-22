// FunctionAccessAnalyzer.cpp — see FunctionAccessAnalyzer.h for the
// high-level pipeline. Code below is grouped into:
//
//   1. Driver: constructor, run() (per-function analysis), and
//      onEndOfTranslationUnit() (the phase orchestrator).
//   2. Phase 0  — detectAllTransformations (RustSlice detection; results
//      are exported to metadata for xj-prepare-slicetransform).
//   3. Phase 1  — transformAllFunctions (plain index rewriting).
//   4. Helpers  — exportMetadata, applyEdits, etc.
//
// All actual source rewriting is deferred to onEndOfTranslationUnit so
// that detection (which depends on which other functions get rewritten)
// can run a fixpoint without being interleaved with edits.

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

// All actual source rewriting happens here. The phases are ordered so
// that detection (which can grow as functions become eligible
// transitively) finishes before any edits, and so that signature
// changes are applied before call-site patching.
void FunctionAccessAnalyzer::onEndOfTranslationUnit() {
    if (!StoredCtx)
        return;
    ASTContext &Ctx = *StoredCtx;

    // Detect every RustSlice candidate (root, singleton, pointer-pair,
    // recursive). No edits emitted yet. The detection results are not
    // applied here — they are exported to the metadata side-file for
    // xj-prepare-slicetransform, which performs all signature-level
    // reshaping.
    detectAllTransformations(Ctx);

    // Rewrite individual local pointers function-by-function. May
    // un-mark a function for RustSlice if its iterating pointer fails
    // validation.
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

    // Export this TU's detection results for the slice pass.
    exportMetadata(Ctx);
}

// ============================================================================
// detectAllTransformations — populate g_transformed_functions
// ============================================================================
//
// Runs three sub-phases:
//
//   A. Root candidates: a function whose body contains an iterating
//      local pointer initialized from a parameter and bounded by
//      another parameter (length or end pointer). These are the
//      "obvious" RustSlice functions.
//
//   B. Singleton callees: a non-RustSlice function called from a
//      RustSlice function whose pointer params are only dereferenced
//      (no iteration). e.g. swap(int *a, int *b). Becomes
//      swap(RustSlice arr, int a, int b).
//
//   C. Pointer-pair propagation (fixpoint): a function that forwards
//      a pointer pair to an already-RustSlice function (or recurses
//      into itself with a pointer pair) is itself RustSlice. Loop
//      until no new functions are discovered.

void FunctionAccessAnalyzer::detectAllTransformations(ASTContext &Ctx) {
    // ---- Sub-phase A: root RustSlice candidates -----------------------
    for (auto &[FDCanon, analysis] : g_function_analyses) {
        const FunctionDecl *FD = analysis.FD;
        if (!FD || !FD->hasBody() || FD->getNumParams() == 0)
            continue;

        for (auto &[PtrVar, accesses] : analysis.accesses) {
            auto &candidate = analysis.tracked_pointers[PtrVar];

            // Same detection logic as generateTransformation
            if (candidate.is_parameter || FD->getNumParams() == 0)
                continue;

            // Check if base_array matches a function parameter
            int base_param_idx = -1;
            std::string pointee_type;
            for (unsigned i = 0; i < FD->getNumParams(); i++) {
                const ParmVarDecl *P = FD->getParamDecl(i);
                if (P->getNameAsString() == candidate.base_array_text &&
                    P->getType()->isPointerType()) {
                    base_param_idx = i;
                    QualType pt = P->getType()->getPointeeType();
                    pointee_type = pt.getUnqualifiedType().getAsString();
                    break;
                }
            }
            if (base_param_idx < 0)
                continue;

            // Need ComparisonExpr to determine length source
            bool found_rs = false;
            RustSliceInfo info;
            info.base_param_index = base_param_idx;
            info.pointee_type = pointee_type;
            info.slice_type = makeSliceTypeName(pointee_type);
            info.slice_param_name = "arr";
            info.lookback = -candidate.min_relative_offset;
            info.lookahead = candidate.max_relative_offset;

            for (const auto &access : accesses) {
                if (access.kind == PointerAccessKind::ComparisonExpr) {
                    std::string op_text = access.operand_text;

                    // Case 0: pointer-form equality "base + idx != (end)"
                    // (field_name carries the base; see the collector's
                    // equality handling).
                    if (!access.field_name.empty()) {
                        if (op_text.size() > 2 && op_text.front() == '(' &&
                            op_text.back() == ')') {
                            std::string end_name =
                                op_text.substr(1, op_text.size() - 2);
                            for (unsigned i = 0; i < FD->getNumParams(); i++) {
                                if (FD->getParamDecl(i)->getNameAsString() == end_name &&
                                    FD->getParamDecl(i)->getType()->isPointerType()) {
                                    info.end_param_index = i;
                                    found_rs = true;
                                    break;
                                }
                            }
                        }
                        break;
                    }

                    // Case 1: pointer pair "(end - base)"
                    std::string pair_suffix = " - " + candidate.base_array_text + ")";
                    if (op_text.size() > pair_suffix.size() &&
                        op_text.front() == '(' &&
                        op_text.substr(op_text.size() - pair_suffix.size()) == pair_suffix) {
                        std::string end_name = op_text.substr(1, op_text.size() - pair_suffix.size() - 1);
                        for (unsigned i = 0; i < FD->getNumParams(); i++) {
                            if (FD->getParamDecl(i)->getNameAsString() == end_name) {
                                info.end_param_index = i;
                                found_rs = true;
                                break;
                            }
                        }
                    }
                    // Case 2: simple param name (e.g., "n")
                    else {
                        for (unsigned i = 0; i < FD->getNumParams(); i++) {
                            if (FD->getParamDecl(i)->getNameAsString() == op_text &&
                                !FD->getParamDecl(i)->getType()->isPointerType()) {
                                info.len_param_index = i;
                                found_rs = true;
                                break;
                            }
                        }
                    }
                    break;
                }
            }

            if (!found_rs)
                continue;

            // Validate the pointer before committing to RustSlice.
            // A linked-list walk (p = p->next) passes the ComparisonExpr
            // check but fails validation due to base mismatch → Unknown.
            {
                std::string val_error;
                if (!validatePointerCandidate(PtrVar, candidate, accesses,
                                              Ctx, val_error)) {
                    if (VERBOSE)
                        llvm::outs() << "[Detect] Skipping RustSlice in "
                                      << FD->getNameAsString()
                                      << ": " << val_error << "\n";
                    continue;
                }
            }

            // Detect return type change.
            //
            // The return type can only become `int` if every return
            // statement either returns NULL or returns a tracked
            // pointer whose base array is the slice's base — i.e. one
            // we can render as an integer index into the slice. A
            // function that returns an unrelated pointer (e.g.
            // `return malloc(...)`, `return p->next`) would silently
            // become an integer-returning function whose returned
            // value the caller still treats as a pointer. We refuse
            // the rewrite in that case.
            if (FD->getReturnType()->isPointerType() && FD->hasBody()) {
                const std::string &slice_base = candidate.base_array_text;

                class ReturnSafetyChecker : public RecursiveASTVisitor<ReturnSafetyChecker> {
                public:
                    const std::string &slice_base;
                    const std::map<const VarDecl *, PointerCandidate> &tracked;
                    bool any_return = false;
                    bool all_safe = true;

                    ReturnSafetyChecker(const std::string &sb,
                                        const std::map<const VarDecl *, PointerCandidate> &t)
                        : slice_base(sb), tracked(t) {}

                    static bool isNullLike(const Expr *E) {
                        E = E->IgnoreParenImpCasts();
                        if (const auto *IL = dyn_cast<IntegerLiteral>(E))
                            return IL->getValue() == 0;
                        if (isa<GNUNullExpr>(E))
                            return true;
                        if (const auto *CE = dyn_cast<CStyleCastExpr>(E))
                            return isNullLike(CE->getSubExpr());
                        return false;
                    }

                    bool VisitReturnStmt(ReturnStmt *RS) {
                        const Expr *V = RS->getRetValue();
                        if (!V)
                            return true;  // void return - shouldn't happen for a pointer-returning function
                        any_return = true;

                        const Expr *Stripped = V->IgnoreParenImpCasts();
                        while (const auto *C = dyn_cast<CStyleCastExpr>(Stripped))
                            Stripped = C->getSubExpr()->IgnoreParenImpCasts();

                        if (isNullLike(Stripped))
                            return true;

                        // Accept only DeclRefs to tracked pointers
                        // whose base matches the slice's base.
                        if (const auto *DRE = dyn_cast<DeclRefExpr>(Stripped)) {
                            if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                                auto it = tracked.find(VD);
                                if (it != tracked.end() &&
                                    it->second.base_array_text == slice_base)
                                    return true;
                            }
                        }

                        all_safe = false;
                        return false;
                    }
                };

                ReturnSafetyChecker checker(slice_base, analysis.tracked_pointers);
                checker.TraverseStmt(FD->getBody());
                if (checker.any_return && checker.all_safe)
                    info.return_type_changed = true;
            }

            // Detect inclusive end
            if (info.end_param_index >= 0) {
                const ParmVarDecl *EndP = FD->getParamDecl(info.end_param_index);
                auto ep_it = analysis.accesses.find(EndP);
                if (ep_it != analysis.accesses.end()) {
                    for (const auto &acc : ep_it->second) {
                        if (acc.kind == PointerAccessKind::Deref ||
                            acc.kind == PointerAccessKind::DerefWrite) {
                            info.inclusive_end = true;
                            break;
                        }
                    }
                }
            }

            // Safety: refuse the rewrite if any *non-rewriteable*
            // pointer in the body references the base/end/len params.
            //
            // After the signature change, the slice pass rewrites the
            // remaining references to those params to arr.ptr/arr.len.
            // That's correct for pointers we're also rewriting to
            // indices — `q_index < arr.len` makes sense. But for a
            // pointer that stays a real pointer (because it was
            // reseated from an opaque expression like
            // `q = find_comma(q) + 1`), the rewrite turns
            // `q < end` into `char *q < size_t arr.len`, which compiles
            // but is meaningless and silently breaks the loop.
            {
                std::string base_name = FD->getParamDecl(info.base_param_index)->getNameAsString();
                std::string end_name = info.end_param_index >= 0
                    ? FD->getParamDecl(info.end_param_index)->getNameAsString()
                    : std::string();
                std::string len_name = info.len_param_index >= 0
                    ? FD->getParamDecl(info.len_param_index)->getNameAsString()
                    : std::string();

                auto containsWord = [](const std::string &s, const std::string &w) {
                    if (w.empty()) return false;
                    size_t pos = 0;
                    while ((pos = s.find(w, pos)) != std::string::npos) {
                        bool before_ok = pos == 0 ||
                            (!isalnum((unsigned char)s[pos - 1]) && s[pos - 1] != '_');
                        bool after_ok = pos + w.size() >= s.size() ||
                            (!isalnum((unsigned char)s[pos + w.size()]) &&
                             s[pos + w.size()] != '_');
                        if (before_ok && after_ok) return true;
                        pos += w.size();
                    }
                    return false;
                };

                bool unsafe = false;
                for (auto &[OtherPtr, OtherAccesses] : analysis.accesses) {
                    if (OtherPtr == PtrVar) continue;
                    if (isa<ParmVarDecl>(OtherPtr)) continue;
                    if (OtherAccesses.empty()) continue;

                    // If this pointer would itself be rewritten as an
                    // index, the rewriter handles its references to
                    // base/end correctly.
                    auto &OC = analysis.tracked_pointers[OtherPtr];
                    std::string verr;
                    if (validatePointerCandidate(OtherPtr, OC, OtherAccesses, Ctx, verr))
                        continue;

                    // Otherwise, look for any reference to the removed
                    // params in the pointer's metadata.
                    bool refs = (!base_name.empty() && OC.base_array_text == base_name);
                    if (!refs) {
                        for (const auto &acc : OtherAccesses) {
                            if (containsWord(acc.operand_text, base_name) ||
                                containsWord(acc.operand_text, end_name) ||
                                containsWord(acc.operand_text, len_name) ||
                                containsWord(acc.offset_text, base_name) ||
                                containsWord(acc.offset_text, end_name) ||
                                containsWord(acc.offset_text, len_name)) {
                                refs = true;
                                break;
                            }
                        }
                    }
                    if (refs) {
                        unsafe = true;
                        break;
                    }
                }

                if (unsafe) {
                    if (VERBOSE)
                        llvm::outs() << "[Detect] Skipping RustSlice in "
                                      << FD->getNameAsString()
                                      << ": non-rewriteable pointer references removed param\n";
                    continue;
                }
            }

            info.driver_ptr_name = PtrVar->getNameAsString();
            g_transformed_functions[FDCanon] = info;
            if (VERBOSE)
                llvm::outs() << "[Detect] Root RustSlice in "
                              << FD->getNameAsString() << "\n";
            break; // one RustSlice per function
        }
    }

    // ---- Sub-phase B: singleton callees -------------------------------
    for (auto &[FDCanon, info] : g_transformed_functions) {
        auto fa_it = g_function_analyses.find(FDCanon);
        if (fa_it == g_function_analyses.end())
            continue;
        auto &analysis = fa_it->second;

        std::map<const FunctionDecl *, std::vector<const CallExpr *>> callee_calls;

        for (auto &[PtrVar, accesses] : analysis.accesses) {
            for (const auto &acc : accesses) {
                if (acc.kind == PointerAccessKind::PassedToFunc && acc.enclosing_stmt) {
                    if (const CallExpr *CE = dyn_cast<CallExpr>(acc.enclosing_stmt)) {
                        if (const FunctionDecl *Callee = CE->getDirectCallee()) {
                            const FunctionDecl *CalleeCanon = Callee->getCanonicalDecl();
                            if (g_transformed_functions.count(CalleeCanon))
                                continue;
                            if (!g_function_analyses.count(CalleeCanon))
                                continue;
                            callee_calls[CalleeCanon].push_back(CE);
                        }
                    }
                }
            }
        }

        for (auto &[CalleeCanon, calls] : callee_calls) {
            auto &callee_analysis = g_function_analyses[CalleeCanon];
            const FunctionDecl *CalleeFD = callee_analysis.FD;
            if (!CalleeFD)
                continue;

            bool all_singleton = true;
            std::vector<int> singleton_indices;
            std::string pointee_type;

            for (unsigned pi = 0; pi < CalleeFD->getNumParams(); pi++) {
                const ParmVarDecl *P = CalleeFD->getParamDecl(pi);
                if (!P->getType()->isPointerType())
                    continue;

                auto acc_it = callee_analysis.accesses.find(P);
                if (acc_it == callee_analysis.accesses.end()) {
                    all_singleton = false;
                    break;
                }

                bool is_singleton = true;
                for (const auto &acc : acc_it->second) {
                    switch (acc.kind) {
                    case PointerAccessKind::Deref:
                    case PointerAccessKind::DerefWrite:
                        break;
                    default:
                        is_singleton = false;
                        break;
                    }
                    if (!is_singleton) break;
                }

                if (!is_singleton) {
                    all_singleton = false;
                    break;
                }

                singleton_indices.push_back(pi);
                QualType pt = P->getType()->getPointeeType();
                pointee_type = pt.getUnqualifiedType().getAsString();
            }

            if (!all_singleton || singleton_indices.empty())
                continue;

            RustSliceInfo callee_info;
            callee_info.slice_type = makeSliceTypeName(pointee_type);
            callee_info.pointee_type = pointee_type;
            callee_info.slice_param_name = info.slice_param_name;
            callee_info.singleton_param_indices = singleton_indices;

            g_transformed_functions[CalleeCanon] = callee_info;
            if (VERBOSE)
                llvm::outs() << "[Detect] Singleton callee: "
                              << CalleeFD->getNameAsString() << "\n";
        }
    }

    // ---- Sub-phase C: pointer-pair propagation (fixpoint) ------------
    // Helper: does `E` mention `PD` anywhere in its subtree?
    auto exprReferencesParam = [](const Expr *E, const ParmVarDecl *PD) -> bool {
        if (!E || !PD) return false;
        class ParamFinder : public RecursiveASTVisitor<ParamFinder> {
        public:
            const ParmVarDecl *Target;
            bool Found = false;
            explicit ParamFinder(const ParmVarDecl *P) : Target(P) {}
            bool VisitDeclRefExpr(DeclRefExpr *DRE) {
                if (DRE->getDecl() == Target) {
                    Found = true;
                    return false;
                }
                return true;
            }
        };
        ParamFinder finder(PD);
        finder.TraverseStmt(const_cast<Expr *>(E));
        return finder.Found;
    };

    bool changed = true;
    while (changed) {
        changed = false;

        for (auto &[FDCanon, analysis] : g_function_analyses) {
            if (g_transformed_functions.count(FDCanon))
                continue;

            const FunctionDecl *FD = analysis.FD;
            if (!FD || !FD->hasBody())
                continue;

            // Collect all pointer params of the same type
            std::vector<std::pair<int, std::string>> ptr_params;
            for (unsigned i = 0; i < FD->getNumParams(); i++) {
                const ParmVarDecl *P = FD->getParamDecl(i);
                if (P->getType()->isPointerType()) {
                    QualType pt = P->getType()->getPointeeType();
                    std::string pt_str = pt.getUnqualifiedType().getAsString();
                    ptr_params.push_back({(int)i, pt_str});
                }
            }

            if (ptr_params.size() < 2)
                continue;

            // Find a valid pointer-pair: both params must be passed as base+end
            // to the same call to a transformed callee
            int first_ptr_param = -1;
            int second_ptr_param = -1;
            std::string pointee_type;
            bool valid_pair_found = false;

            // Collect all calls to transformed callees OR recursive self-calls
            class TransformedCallFinder : public RecursiveASTVisitor<TransformedCallFinder> {
            public:
                const FunctionDecl *SelfCanon;
                std::vector<const CallExpr *> calls;
                std::vector<const CallExpr *> self_calls;
                explicit TransformedCallFinder(const FunctionDecl *Self)
                    : SelfCanon(Self) {}
                bool VisitCallExpr(CallExpr *CE) {
                    if (const FunctionDecl *Callee = CE->getDirectCallee()) {
                        const FunctionDecl *CC = Callee->getCanonicalDecl();
                        if (CC == SelfCanon) {
                            self_calls.push_back(CE);
                        } else if (g_transformed_functions.count(CC)) {
                            calls.push_back(CE);
                        }
                    }
                    return true;
                }
            };

            TransformedCallFinder callFinder(FDCanon);
            callFinder.TraverseStmt(FD->getBody());

            // Helper lambda: check pointer-pair in a call's args
            auto checkPairInCall = [&](const CallExpr *CE, int base_idx, int end_idx) {
                if (base_idx < 0 || end_idx < 0) return;
                if ((unsigned)base_idx >= CE->getNumArgs() ||
                    (unsigned)end_idx >= CE->getNumArgs()) return;

                const Expr *BaseArg = CE->getArg(base_idx);
                const Expr *EndArg = CE->getArg(end_idx);


                for (size_t a = 0; a < ptr_params.size() && !valid_pair_found; a++) {
                    for (size_t b = a + 1; b < ptr_params.size() && !valid_pair_found; b++) {
                        if (ptr_params[a].second != ptr_params[b].second)
                            continue;
                        const ParmVarDecl *PA = FD->getParamDecl(ptr_params[a].first);
                        const ParmVarDecl *PB = FD->getParamDecl(ptr_params[b].first);

                        // Check: PA in base, PB in end
                        if (exprReferencesParam(BaseArg, PA) &&
                            exprReferencesParam(EndArg, PB) &&
                            !exprReferencesParam(BaseArg, PB) &&
                            !exprReferencesParam(EndArg, PA)) {
                            first_ptr_param = ptr_params[a].first;
                            second_ptr_param = ptr_params[b].first;
                            pointee_type = ptr_params[a].second;
                            valid_pair_found = true;
                        }
                        // Check: PB in base, PA in end
                        else if (exprReferencesParam(BaseArg, PB) &&
                                 exprReferencesParam(EndArg, PA) &&
                                 !exprReferencesParam(BaseArg, PA) &&
                                 !exprReferencesParam(EndArg, PB)) {
                            first_ptr_param = ptr_params[b].first;
                            second_ptr_param = ptr_params[a].first;
                            pointee_type = ptr_params[a].second;
                            valid_pair_found = true;
                        }
                    }
                }
            };

            // Check calls to already-transformed callees
            for (const CallExpr *CE : callFinder.calls) {
                const FunctionDecl *Callee = CE->getDirectCallee();
                if (!Callee) continue;
                auto ci = g_transformed_functions.find(Callee->getCanonicalDecl());
                if (ci == g_transformed_functions.end()) continue;
                const RustSliceInfo &callee_info = ci->second;
                checkPairInCall(CE, callee_info.base_param_index,
                                callee_info.end_param_index);
                if (valid_pair_found) break;
            }

            // Check recursive self-calls: for self-recursive functions,
            // params are often passed indirectly via derived local pointers
            // (e.g. mid = lo + (hi-lo)/2; binary_search(mid+1, hi)).
            // Use a different heuristic: check if there's pointer subtraction
            // between two params (e.g. hi - lo, end - start), which indicates
            // they define a range.
            if (!valid_pair_found && !callFinder.self_calls.empty()) {
                // Find pointer subtraction between param pairs in the function body
                class ParamSubFinder : public RecursiveASTVisitor<ParamSubFinder> {
                public:
                    const FunctionDecl *FD;
                    std::set<std::pair<int,int>> sub_pairs; // (lhs_param_idx, rhs_param_idx)
                    explicit ParamSubFinder(const FunctionDecl *F) : FD(F) {}
                    bool VisitBinaryOperator(BinaryOperator *BO) {
                        if (BO->getOpcode() != BO_Sub) return true;
                        const auto *LDRE = dyn_cast<DeclRefExpr>(
                            BO->getLHS()->IgnoreImpCasts());
                        const auto *RDRE = dyn_cast<DeclRefExpr>(
                            BO->getRHS()->IgnoreImpCasts());
                        if (!LDRE || !RDRE) return true;
                        const auto *LP = dyn_cast<ParmVarDecl>(LDRE->getDecl());
                        const auto *RP = dyn_cast<ParmVarDecl>(RDRE->getDecl());
                        if (!LP || !RP) return true;
                        // Find their indices
                        int li = -1, ri = -1;
                        for (unsigned i = 0; i < FD->getNumParams(); i++) {
                            if (FD->getParamDecl(i) == LP) li = i;
                            if (FD->getParamDecl(i) == RP) ri = i;
                        }
                        if (li >= 0 && ri >= 0)
                            sub_pairs.insert({li, ri});
                        return true;
                    }
                };
                ParamSubFinder subFinder(FD);
                subFinder.TraverseStmt(FD->getBody());

                for (auto &[li, ri] : subFinder.sub_pairs) {
                    if (valid_pair_found) break;
                    // Check that both are pointer params of the same type
                    bool li_found = false, ri_found = false;
                    std::string li_type, ri_type;
                    for (auto &[idx, tp] : ptr_params) {
                        if (idx == li) { li_found = true; li_type = tp; }
                        if (idx == ri) { ri_found = true; ri_type = tp; }
                    }
                    if (!li_found || !ri_found || li_type != ri_type)
                        continue;
                    // end - base pattern: LHS is end param, RHS is base param
                    first_ptr_param = ri;   // base
                    second_ptr_param = li;  // end
                    pointee_type = li_type;
                    valid_pair_found = true;
                }
            }

            if (!valid_pair_found)
                continue;

            // Detect inclusive end: either the end param is directly
            // dereferenced, or it's passed to a callee that expects inclusive end,
            // or the base/end comparison uses strict inequality (lo > hi → inclusive)
            const ParmVarDecl *EndParam = FD->getParamDecl(second_ptr_param);
            const ParmVarDecl *BaseParam = FD->getParamDecl(first_ptr_param);
            bool inclusive_end = false;
            auto end_acc_it = analysis.accesses.find(EndParam);
            if (end_acc_it != analysis.accesses.end()) {
                for (const auto &acc : end_acc_it->second) {
                    if (acc.kind == PointerAccessKind::Deref ||
                        acc.kind == PointerAccessKind::DerefWrite) {
                        inclusive_end = true;
                        break;
                    }
                    // Check if passed to a callee that expects inclusive end
                    if (acc.kind == PointerAccessKind::PassedToFunc && acc.enclosing_stmt) {
                        if (const CallExpr *CE = dyn_cast<CallExpr>(acc.enclosing_stmt)) {
                            if (const FunctionDecl *Callee = CE->getDirectCallee()) {
                                auto ci = g_transformed_functions.find(
                                    Callee->getCanonicalDecl());
                                if (ci != g_transformed_functions.end() &&
                                    ci->second.inclusive_end) {
                                    inclusive_end = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // Heuristic: check comparisons between base and end params
            // "base > end" or "end < base" (strict) → inclusive end
            // "base >= end" or "end <= base" (non-strict) → exclusive end
            if (!inclusive_end) {
                class ComparisonChecker : public RecursiveASTVisitor<ComparisonChecker> {
                public:
                    const ParmVarDecl *Base, *End;
                    bool found_inclusive = false;
                    ComparisonChecker(const ParmVarDecl *B, const ParmVarDecl *E)
                        : Base(B), End(E) {}
                    bool VisitBinaryOperator(BinaryOperator *BO) {
                        if (!BO->isComparisonOp()) return true;
                        const auto *L = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreImpCasts());
                        const auto *R = dyn_cast<DeclRefExpr>(BO->getRHS()->IgnoreImpCasts());
                        if (!L || !R) return true;
                        const auto *LP = dyn_cast<ParmVarDecl>(L->getDecl());
                        const auto *RP = dyn_cast<ParmVarDecl>(R->getDecl());
                        if (!LP || !RP) return true;
                        // base > end or end < base → inclusive
                        if ((LP == Base && RP == End && BO->getOpcode() == BO_GT) ||
                            (LP == End && RP == Base && BO->getOpcode() == BO_LT)) {
                            found_inclusive = true;
                        }
                        return true;
                    }
                };
                ComparisonChecker checker(BaseParam, EndParam);
                checker.TraverseStmt(FD->getBody());
                if (checker.found_inclusive)
                    inclusive_end = true;

                // Source-text fallback: check for "base > end" pattern in body
                if (!inclusive_end && FD->hasBody()) {
                    SourceManager &SM = Ctx.getSourceManager();
                    SourceLocation BS = FD->getBody()->getBeginLoc();
                    SourceLocation BE = FD->getBody()->getEndLoc();
                    unsigned bsOff = SM.getFileOffset(BS);
                    unsigned beOff = SM.getFileOffset(BE);
                    StringRef bodyText = SM.getBufferData(SM.getFileID(BS))
                                             .substr(bsOff, beOff - bsOff);
                    std::string bname = BaseParam->getNameAsString();
                    std::string ename = EndParam->getNameAsString();
                    // "base > end" → inclusive; "base >= end" → exclusive
                    if (bodyText.find(bname + " > " + ename) != StringRef::npos ||
                        bodyText.find(ename + " < " + bname) != StringRef::npos)
                        inclusive_end = true;
                }
            }

            // Detect return type change. Same safety check as in
            // sub-phase A: every return must be NULL or a tracked
            // pointer whose base is the slice base (here, the first
            // pointer parameter's name). Otherwise the function would
            // become `int`-returning while still returning an
            // unrelated pointer.
            bool return_changed = false;
            if (FD->getReturnType()->isPointerType() && FD->hasBody()) {
                std::string slice_base = BaseParam->getNameAsString();

                class ReturnSafetyChecker : public RecursiveASTVisitor<ReturnSafetyChecker> {
                public:
                    const std::string &slice_base;
                    const std::map<const VarDecl *, PointerCandidate> &tracked;
                    bool any_return = false;
                    bool all_safe = true;

                    ReturnSafetyChecker(const std::string &sb,
                                        const std::map<const VarDecl *, PointerCandidate> &t)
                        : slice_base(sb), tracked(t) {}

                    static bool isNullLike(const Expr *E) {
                        E = E->IgnoreParenImpCasts();
                        if (const auto *IL = dyn_cast<IntegerLiteral>(E))
                            return IL->getValue() == 0;
                        if (isa<GNUNullExpr>(E))
                            return true;
                        if (const auto *CE = dyn_cast<CStyleCastExpr>(E))
                            return isNullLike(CE->getSubExpr());
                        return false;
                    }

                    bool VisitReturnStmt(ReturnStmt *RS) {
                        const Expr *V = RS->getRetValue();
                        if (!V)
                            return true;
                        any_return = true;

                        const Expr *Stripped = V->IgnoreParenImpCasts();
                        while (const auto *C = dyn_cast<CStyleCastExpr>(Stripped))
                            Stripped = C->getSubExpr()->IgnoreParenImpCasts();

                        if (isNullLike(Stripped))
                            return true;

                        if (const auto *DRE = dyn_cast<DeclRefExpr>(Stripped)) {
                            if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                                auto it = tracked.find(VD);
                                if (it != tracked.end() &&
                                    it->second.base_array_text == slice_base)
                                    return true;
                            }
                        }

                        all_safe = false;
                        return false;
                    }
                };

                ReturnSafetyChecker checker(slice_base, analysis.tracked_pointers);
                checker.TraverseStmt(FD->getBody());
                if (checker.any_return && checker.all_safe)
                    return_changed = true;
            }

            RustSliceInfo info;
            info.slice_type = makeSliceTypeName(pointee_type);
            info.pointee_type = pointee_type;
            info.slice_param_name = "arr";
            info.base_param_index = first_ptr_param;
            info.end_param_index = second_ptr_param;
            info.inclusive_end = inclusive_end;
            info.return_type_changed = return_changed;

            g_transformed_functions[FDCanon] = info;
            changed = true;

            if (VERBOSE)
                llvm::outs() << "[Detect] Pointer-pair caller: "
                              << FD->getNameAsString() << "\n";
        }
    }

    // ---- Sub-phase D: global-return functions ------------------------
    // A function whose every return is either NULL or `&global_array[i]`
    // (always the *same* global array) is a candidate to have its
    // return type rewritten from T* to int. The caller will then index
    // into the global array directly. Recorded in
    // g_global_return_functions, exported to metadata, and applied by the
    // slice pass.
    for (auto &[FDCanon, analysis] : g_function_analyses) {
        if (g_transformed_functions.count(FDCanon))
            continue;
        if (g_global_return_functions.count(FDCanon))
            continue;

        const FunctionDecl *FD = analysis.FD;
        if (!FD || !FD->hasBody())
            continue;
        if (!FD->getReturnType()->isPointerType())
            continue;

        // Collect all return statements
        class GlobalReturnDetector : public RecursiveASTVisitor<GlobalReturnDetector> {
        public:
            std::string global_array_name;
            bool all_returns_valid = true;
            bool found_global_return = false;

            bool VisitReturnStmt(ReturnStmt *RS) {
                const Expr *RetVal = RS->getRetValue();
                if (!RetVal) {
                    all_returns_valid = false;
                    return true;
                }

                RetVal = RetVal->IgnoreParenImpCasts();

                // Check for NULL/0
                if (const auto *IL = dyn_cast<IntegerLiteral>(RetVal)) {
                    if (IL->getValue() == 0)
                        return true; // NULL literal, valid
                }
                if (const auto *GNSE = dyn_cast<GNUNullExpr>(RetVal)) {
                    return true; // __null, valid
                }
                // Check for macro-expanded NULL (cast of 0)
                if (const auto *CSC = dyn_cast<CStyleCastExpr>(RetVal)) {
                    const Expr *Sub = CSC->getSubExpr()->IgnoreParenImpCasts();
                    if (const auto *IL = dyn_cast<IntegerLiteral>(Sub)) {
                        if (IL->getValue() == 0)
                            return true; // (void*)0 = NULL
                    }
                }

                // Check for &global_array[expr]
                if (const auto *UO = dyn_cast<UnaryOperator>(RetVal)) {
                    if (UO->getOpcode() == UO_AddrOf) {
                        const Expr *Sub = UO->getSubExpr()->IgnoreImpCasts();
                        if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Sub)) {
                            const Expr *Base = ASE->getBase()->IgnoreImpCasts();
                            if (const auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
                                if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                                    if (VD->hasGlobalStorage()) {
                                        std::string name = VD->getNameAsString();
                                        if (global_array_name.empty()) {
                                            global_array_name = name;
                                            found_global_return = true;
                                        } else if (global_array_name != name) {
                                            all_returns_valid = false;
                                        }
                                        return true;
                                    }
                                }
                            }
                        }
                    }
                }

                // Any other return expression is invalid
                all_returns_valid = false;
                return true;
            }
        };

        GlobalReturnDetector detector;
        detector.TraverseStmt(FD->getBody());

        if (detector.found_global_return && detector.all_returns_valid) {
            GlobalReturnInfo gri;
            gri.global_array_name = detector.global_array_name;
            QualType pt = FD->getReturnType()->getPointeeType();
            gri.pointee_type = pt.getUnqualifiedType().getAsString();
            g_global_return_functions[FDCanon] = gri;

            if (VERBOSE)
                llvm::outs() << "[Detect] Global-return function: "
                              << FD->getNameAsString()
                              << " (returns into " << gri.global_array_name << ")\n";
        }
    }
}
// ============================================================================
// transformAllFunctions — rewrite every local pointer that's safe to
// rewrite, function by function. For root-RustSlice functions this
// also rewrites the iterating pointer in terms of arr.ptr / arr.len.
// ============================================================================

void FunctionAccessAnalyzer::transformAllFunctions(ASTContext &Ctx) {
    for (auto &[FDCanon, analysis] : g_function_analyses) {
        const FunctionDecl *FD = analysis.FD;
        if (!FD || !FD->hasBody())
            continue;

        auto rs_it = g_transformed_functions.find(FDCanon);

        // Singleton functions (swap-style) have no moving pointers, so
        // there is nothing for this pass to rewrite in their bodies; the
        // slice pass reshapes them wholesale from the metadata. Every
        // other function — including pointer-pair and root RustSlice
        // candidates — gets its local pointers rewritten here in plain
        // (base-param-relative) form.
        if (rs_it != g_transformed_functions.end() &&
            !rs_it->second.singleton_param_indices.empty())
            continue;

        m_edited_ranges.clear();

        // Two-pass: RustSlice-triggering pointers first, then others
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

        unsigned pre_count = g_pointers_replaced;

        // Pre-validation: determine which pointers will actually be
        // transformed so that cross-pointer comparisons can be resolved.
        std::set<const VarDecl *> will_transform;
        for (auto &pair : analysis.accesses) {
            const VarDecl *PtrVar = pair.first;
            auto &candidate = analysis.tracked_pointers[PtrVar];
            auto &access_list = pair.second;
            if (candidate.is_parameter && rs_it != g_transformed_functions.end())
                continue;
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

        // First pass: RustSlice-triggering pointers
        for (const VarDecl *PtrVar : rust_slice_candidates) {
            auto &access_list = analysis.accesses[PtrVar];
            auto &candidate = analysis.tracked_pointers[PtrVar];
            transformPointerVar(FD, PtrVar, candidate, access_list, Ctx);
        }

        // Second pass: remaining pointers (skip parameter pointers in
        // RustSlice functions — they're consumed or should remain as-is)
        // Also skip pointers removed from will_transform by init conflict detection.
        for (const VarDecl *PtrVar : other_pointers) {
            auto &access_list = analysis.accesses[PtrVar];
            auto &candidate = analysis.tracked_pointers[PtrVar];
            if (candidate.is_parameter && rs_it != g_transformed_functions.end())
                continue;
            if (will_transform.find(PtrVar) == will_transform.end())
                continue;
            transformPointerVar(FD, PtrVar, candidate, access_list, Ctx);
        }

        // A (ptr, len) root candidate whose iterating pointer didn't
        // actually transform (e.g. validation rejected it after
        // detection) is dropped so the slice pass doesn't reshape a
        // signature whose body was never index-rewritten. Pointer-pair
        // candidates ((lo, hi) forms) are kept even without body
        // rewrites: their reshaping never depended on a local pointer
        // transforming (propagation/recursion cases).
        if (rs_it != g_transformed_functions.end() &&
            rs_it->second.end_param_index < 0 &&
            rs_it->second.singleton_param_indices.empty() &&
            g_pointers_replaced == pre_count) {
            g_transformed_functions.erase(rs_it);
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

// Export the surviving detection results (slice reshapings and
// global-return rewrites) for this TU into g_metadata. Called at the
// end of onEndOfTranslationUnit, after transformAllFunctions has had
// its chance to drop candidates whose body rewrites failed.
void FunctionAccessAnalyzer::exportMetadata(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();

    for (auto &[FDCanon, info] : g_transformed_functions) {
        auto fa_it = g_function_analyses.find(FDCanon);
        if (fa_it == g_function_analyses.end() || !fa_it->second.FD)
            continue;
        const FunctionDecl *FD = fa_it->second.FD;

        xj::PtrIndexFunctionRecord *fnRec = metadataRecordFor(FD, Ctx);
        if (!fnRec)
            continue;
        // A record from an earlier TU (e.g. an inline function defined
        // in a shared, already-rewritten header) wins.
        if (fnRec->slice.present)
            continue;

        xj::PtrIndexSliceRecord &S = fnRec->slice;
        S.present = true;
        S.slice_param_name = info.slice_param_name;
        S.slice_type = info.slice_type;
        S.pointee_type = info.pointee_type;
        S.base_param_index = info.base_param_index;
        S.end_param_index = info.end_param_index;
        S.len_param_index = info.len_param_index;
        S.lookback = info.lookback;
        S.lookahead = info.lookahead;
        S.inclusive_end = info.inclusive_end;
        S.return_type_changed = info.return_type_changed;
        S.singleton_param_indices = info.singleton_param_indices;
    }

    for (auto &[FDCanon, gri] : g_global_return_functions) {
        auto fa_it = g_function_analyses.find(FDCanon);
        if (fa_it == g_function_analyses.end() || !fa_it->second.FD)
            continue;
        const FunctionDecl *FD = fa_it->second.FD;
        std::string name = FD->getNameAsString();
        if (g_metadata.global_return_functions.count(name))
            continue;

        xj::PtrIndexGlobalReturnRecord rec;
        if (auto FE = SM.getFileEntryRefForID(
                SM.getFileID(SM.getSpellingLoc(FD->getLocation()))))
            rec.file = llvm::sys::path::filename(FE->getName()).str();
        rec.global_array_name = gri.global_array_name;
        rec.pointee_type = gri.pointee_type;
        g_metadata.global_return_functions.emplace(name, std::move(rec));
    }
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
