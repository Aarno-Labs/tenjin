// FunctionAccessAnalyzer.cpp — see FunctionAccessAnalyzer.h for the
// high-level pipeline. Code below is grouped into:
//
//   1. Driver: constructor, run() (per-function analysis), and
//      onEndOfTranslationUnit() (the phase orchestrator).
//   2. Phase 0  — detectAllTransformations (RustSlice detection).
//   3. Phase 2+ — transformAllFunctions, applySingleton/PointerPair...
//   4. Helpers  — emitTypedefs, rewriteCallSites, fixReturnTypeChanges,
//                 rewriteForwardDeclarations, applyEdits, etc.
//
// All actual source rewriting is deferred to onEndOfTranslationUnit so
// that detection (which depends on which other functions get rewritten)
// can run a fixpoint without being interleaved with edits.

#include "FunctionAccessAnalyzer.h"

namespace {

// Replacing a pointer return type's token range does not necessarily leave a
// separator before the function name.  In the common `T *func()` spelling,
// Clang reports `T *` as the return-type range and `func` starts immediately
// after it.  Preserve existing whitespace when there is some, and supply it
// when the two ranges are adjacent.
void rewriteReturnTypeAsInt(Rewriter &Rewrite, const FunctionDecl *FD,
                            SourceManager &SM, const LangOptions &LO) {
    SourceLocation RetStart = FD->getReturnTypeSourceRange().getBegin();
    SourceLocation RetEnd = FD->getReturnTypeSourceRange().getEnd();
    SourceLocation FuncName = FD->getLocation();
    if (RetStart.isInvalid() || RetEnd.isInvalid() || FuncName.isInvalid())
        return;

    SourceLocation End = Lexer::getLocForEndOfToken(RetEnd, 0, SM, LO);
    unsigned start_offset = SM.getFileOffset(RetStart);
    unsigned end_offset = SM.getFileOffset(End);
    unsigned name_offset = SM.getFileOffset(FuncName);
    std::string replacement = end_offset == name_offset ? "int " : "int";
    Rewrite.ReplaceText(RetStart, end_offset - start_offset, replacement);
}

} // namespace

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
    // recursive). No edits emitted yet.
    detectAllTransformations(Ctx);

    // Rewrite individual local pointers function-by-function. May
    // un-mark a function for RustSlice if its iterating pointer fails
    // validation.
    transformAllFunctions(Ctx);

    // Rewrite RustSlice signatures + bodies.
    applySingletonTransformations(Ctx);
    applyPointerPairTransformations(Ctx);

    // Insert typedefs after the body rewrites have settled the set of
    // pointee types we actually need.
    emitTypedefs(Ctx);

    // Patch all callers of RustSlice functions and any forward
    // declarations / return-type changes.
    if (!g_transformed_functions.empty()) {
        rewriteCallSites(Ctx);
    }
    rewriteForwardDeclarations(Ctx);
    fixReturnTypeChanges(Ctx);

    // Finally, rewrite file-scope pointer variables (they were
    // collected once during the first run() call).
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
        }
    }
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
            // After the signature change, replaceRemovedParams blindly
            // rewrites every textual reference to those params to
            // arr.ptr/arr.len. That's correct for pointers we're also
            // rewriting to indices — `q_index < arr.len` makes sense.
            // But for a pointer that stays a real pointer (because it
            // was reseated from an opaque expression like
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
    // g_global_return_functions and applied later by fixReturnTypeChanges.
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
// emitTypedefs — insert one RustSlice_<T> typedef per pointee type
// ============================================================================
//
// Each typedef is inserted just before the earliest function that
// references it. Slice types whose function has a forward declaration
// in a header are skipped here — rewriteForwardDeclarations places
// the typedef in the header instead so both the prototype and the
// definition see it.

void FunctionAccessAnalyzer::emitTypedefs(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();

    // First pass: which slice types belong to functions whose prototype
    // lives in a non-main file (i.e. a real header)? Those are handled
    // by rewriteForwardDeclarations to keep header + impl in sync.
    std::set<std::string> has_header_decl;
    FileID MainFID = SM.getMainFileID();
    for (auto &[FDCanon, info] : g_transformed_functions) {
        for (const FunctionDecl *Redecl : FDCanon->redecls()) {
            if (Redecl->isThisDeclarationADefinition())
                continue;
            if (SM.isInSystemHeader(Redecl->getLocation()))
                continue;
            if (SM.getFileID(Redecl->getLocation()) != MainFID) {
                has_header_decl.insert(info.slice_type);
                break;
            }
        }
    }

    // Second pass: for each slice type, find the earliest function (or
    // forward declaration in the main file) that uses it, and build
    // the typedef text once.
    std::map<std::string, SourceLocation> earliest_loc;
    std::map<std::string, std::string> typedef_text;

    for (auto &[FDCanon, info] : g_transformed_functions) {
        if (has_header_decl.count(info.slice_type))
            continue;

        auto fa_it = g_function_analyses.find(FDCanon);
        if (fa_it == g_function_analyses.end())
            continue;
        const FunctionDecl *FD = fa_it->second.FD;
        if (!FD)
            continue;

        SourceLocation loc = FD->getBeginLoc();

        // Also consider forward declarations in the main file (e.g.
        // ones inlined by the preprocessor). The typedef must precede
        // every use of the slice type.
        for (const FunctionDecl *Redecl : FDCanon->redecls()) {
            if (Redecl->isThisDeclarationADefinition())
                continue;
            if (SM.isInSystemHeader(Redecl->getLocation()))
                continue;
            SourceLocation rdLoc = Redecl->getBeginLoc();
            if (rdLoc.isValid() && SM.isBeforeInTranslationUnit(rdLoc, loc))
                loc = rdLoc;
        }

        if (earliest_loc.find(info.slice_type) == earliest_loc.end() ||
            SM.isBeforeInTranslationUnit(loc, earliest_loc[info.slice_type])) {
            earliest_loc[info.slice_type] = loc;
        }

        if (typedef_text.find(info.slice_type) == typedef_text.end()) {
            typedef_text[info.slice_type] = "typedef struct { " +
                info.pointee_type + " *ptr; size_t len; } " +
                info.slice_type + ";\n\n";
        }
    }

    // g_emitted_typedefs is checked across translation units so a slice
    // type that's already been emitted in this run doesn't get emitted
    // again.
    for (auto &[type_name, loc] : earliest_loc) {
        if (g_emitted_typedefs.find(type_name) != g_emitted_typedefs.end())
            continue;
        g_emitted_typedefs.insert(type_name);
        TheRewriter.InsertTextBefore(loc, typedef_text[type_name]);
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

        // Only process functions that are either root RustSlice
        // candidates or contain at least one transformable local
        // pointer.
        auto rs_it = g_transformed_functions.find(FDCanon);
        bool is_root_rs = rs_it != g_transformed_functions.end() &&
                          !rs_it->second.singleton_param_indices.empty() == false &&
                          rs_it->second.base_param_index >= 0 &&
                          rs_it->second.end_param_index >= 0;

        // Check if this is a singleton or pointer-pair (handled separately)
        if (rs_it != g_transformed_functions.end()) {
            if (!rs_it->second.singleton_param_indices.empty())
                continue; // singleton — handled in applySingletonTransformations
            if (rs_it->second.base_param_index >= 0 && rs_it->second.end_param_index >= 0)
                continue; // pointer-pair — handled in applyPointerPairTransformations
        }

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

        // Post-process: replace remaining references to removed params
        // Only if at least one pointer was actually transformed
        if (rs_it != g_transformed_functions.end()) {
            if (g_pointers_replaced > pre_count) {
                replaceRemovedParams(FD, Ctx);
            } else {
                // The iterating pointer didn't actually transform (e.g.
                // validation rejected it after detection). Drop the
                // RustSlice entry so call-site rewriting doesn't try
                // to rewrite callers of an unchanged signature.
                g_transformed_functions.erase(rs_it);
            }
        }
    }
}

// Rewrite singleton-style functions like swap(int *a, int *b). Each
// pointer parameter becomes an int index alongside a single shared
// RustSlice parameter; the body is patched to use arr.ptr[index].
void FunctionAccessAnalyzer::applySingletonTransformations(ASTContext &Ctx) {
    for (auto &[FDCanon, info] : g_transformed_functions) {
        if (info.singleton_param_indices.empty())
            continue;

        auto fa_it = g_function_analyses.find(FDCanon);
        if (fa_it == g_function_analyses.end())
            continue;

        m_edited_ranges.clear();
        generateSingletonTransformation(fa_it->second.FD, info, fa_it->second, Ctx);
    }
}

// Rewrite pointer-pair functions like quick_sort(int *lo, int *hi)
// whose iteration happens entirely through recursion / callee calls
// (no local iterating pointer). Root-RustSlice functions, which DO
// have a local iterating pointer, were already handled in
// transformAllFunctions.
void FunctionAccessAnalyzer::applyPointerPairTransformations(ASTContext &Ctx) {
    for (auto &[FDCanon, info] : g_transformed_functions) {
        if (!info.singleton_param_indices.empty())
            continue;
        if (info.base_param_index < 0)
            continue;
        if (info.end_param_index < 0)
            continue;  // ptr+len form, not pointer-pair

        auto fa_it = g_function_analyses.find(FDCanon);
        if (fa_it == g_function_analyses.end())
            continue;

        const FunctionDecl *FD = fa_it->second.FD;

        m_edited_ranges.clear();
        generatePointerPairTransformation(FD, fa_it->second, Ctx);
    }
}

// ============================================================================
// Call site rewriting
// ============================================================================

// Locate the FunctionDecl whose body contains `Loc`. Used to know
// which caller's local-pointer translations to apply when rewriting a
// call site.
static const FunctionDecl *findEnclosingFunction(SourceLocation Loc,
                                                   SourceManager &SM) {
    for (auto &[fdCanon, fa] : g_function_analyses) {
        if (!fa.FD || !fa.FD->hasBody()) continue;
        SourceLocation bodyStart = fa.FD->getBody()->getBeginLoc();
        SourceLocation bodyEnd = fa.FD->getBody()->getEndLoc();
        if (SM.isBeforeInTranslationUnit(bodyStart, Loc) &&
            SM.isBeforeInTranslationUnit(Loc, bodyEnd)) {
            return fa.FD;
        }
    }
    return nullptr;
}

// Translate one call-site argument expression so that any transformed
// pointers / removed params it references render as their new spelling.
//
// Handled cases:
//   - A DeclRefExpr to a transformed local pointer -> append "_index_xj".
//   - A DeclRefExpr to a return-type-changed local -> use the bare
//     name (no suffix; the variable is already an int).
//   - A DeclRefExpr to one of the caller's *removed* params (the
//     original base / end pointer) -> rewrite to arr.ptr / arr.len.
//   - A simple `p + n` / `p - n` -> recurse on the LHS, keep the RHS.
//
// Anything else falls through to the original source text.
std::string FunctionAccessAnalyzer::translateArgExpr(const Expr *ArgExpr,
                                                       const FunctionDecl *CallerFD,
                                                       ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    ArgExpr = ArgExpr->IgnoreImpCasts();
    if (const auto *PE = dyn_cast<ParenExpr>(ArgExpr))
        ArgExpr = PE->getSubExpr()->IgnoreImpCasts();

    auto caller_it = g_transformed_functions.find(CallerFD->getCanonicalDecl());
    if (caller_it == g_transformed_functions.end())
        return getSourceText(ArgExpr, SM, LO);

    const RustSliceInfo &caller_info = caller_it->second;

    if (const auto *DRE = dyn_cast<DeclRefExpr>(ArgExpr)) {
        const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
        if (VD) {
            if (g_index_return_vars.count(VD)) {
                return VD->getNameAsString();
            }

            auto fa_it = g_function_analyses.find(CallerFD->getCanonicalDecl());
            if (fa_it != g_function_analyses.end()) {
                auto &analysis = fa_it->second;
                if (analysis.tracked_pointers.count(VD) &&
                    !analysis.tracked_pointers[VD].is_parameter &&
                    !analysis.accesses[VD].empty()) {
                    return VD->getNameAsString() + "_index_xj";
                }
            }

            if (const auto *PD = dyn_cast<ParmVarDecl>(VD)) {
                if (caller_info.base_param_index >= 0 &&
                    PD == CallerFD->getParamDecl(caller_info.base_param_index)) {
                    return caller_info.slice_param_name + ".ptr";
                }
                if (caller_info.end_param_index >= 0 &&
                    PD == CallerFD->getParamDecl(caller_info.end_param_index)) {
                    if (caller_info.inclusive_end)
                        return caller_info.slice_param_name + ".len - 1";
                    else
                        return caller_info.slice_param_name + ".len";
                }
            }
        }
    }

    if (const auto *BO = dyn_cast<BinaryOperator>(ArgExpr)) {
        if (BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub) {
            std::string lhs = translateArgExpr(BO->getLHS(), CallerFD, Ctx);
            std::string rhs = getSourceText(BO->getRHS(), SM, LO);
            std::string op = (BO->getOpcode() == BO_Add) ? " + " : " - ";
            return lhs + op + rhs;
        }
    }

    return getSourceText(ArgExpr, SM, LO);
}

// Walk every call site of every RustSlice-transformed function and
// rewrite its arguments. Three context groups are handled, all in
// this single pass:
//
//   1. Caller is itself RustSlice and the call is a direct pass-through
//      of its slice → forward `arr` unchanged.
//   2. Caller is RustSlice but the call is a sub-range → emit a
//      compound literal that constructs a sub-slice of `arr`.
//   3. Caller is not transformed → emit a compound literal that wraps
//      the original (base, len) / (lo, hi) into a fresh slice.
//
// Singleton callees (swap-style) are handled inline at the top of the
// loop because their argument shape differs.
void FunctionAccessAnalyzer::rewriteCallSites(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    class CallSiteCollector : public RecursiveASTVisitor<CallSiteCollector> {
    public:
        std::vector<CallExpr *> calls;

        bool VisitCallExpr(CallExpr *CE) {
            const FunctionDecl *Callee = CE->getDirectCallee();
            if (!Callee)
                return true;
            if (g_transformed_functions.count(Callee->getCanonicalDecl())) {
                calls.push_back(CE);
            }
            return true;
        }
    };

    CallSiteCollector collector;
    collector.TraverseDecl(Ctx.getTranslationUnitDecl());

    // Sort calls by reverse offset to avoid interference
    std::sort(collector.calls.begin(), collector.calls.end(),
              [&SM](const CallExpr *A, const CallExpr *B) {
                  return SM.getFileOffset(A->getBeginLoc()) >
                         SM.getFileOffset(B->getBeginLoc());
              });

    for (CallExpr *CE : collector.calls) {
        const FunctionDecl *Callee = CE->getDirectCallee();
        auto it = g_transformed_functions.find(Callee->getCanonicalDecl());
        if (it == g_transformed_functions.end())
            continue;

        const RustSliceInfo &info = it->second;
        unsigned NumArgs = CE->getNumArgs();
        if (NumArgs == 0) continue;

        const FunctionDecl *EnclosingFD = findEnclosingFunction(CE->getBeginLoc(), SM);
        auto caller_it = EnclosingFD ?
            g_transformed_functions.find(EnclosingFD->getCanonicalDecl()) :
            g_transformed_functions.end();

        // Get original argument texts
        std::vector<std::string> arg_texts;
        for (unsigned i = 0; i < NumArgs; i++) {
            arg_texts.push_back(getSourceText(CE->getArg(i), SM, LO));
        }

        // Handle singleton-param functions (like swap)
        if (!info.singleton_param_indices.empty()) {
            if (caller_it != g_transformed_functions.end()) {
                // Caller is transformed — pass its RustSlice + translated indices
                const RustSliceInfo &caller_info = caller_it->second;
                std::string new_args = caller_info.slice_param_name;
                for (unsigned i = 0; i < NumArgs; i++) {
                    new_args += ", " + translateArgExpr(CE->getArg(i), EnclosingFD, Ctx);
                }

                SourceLocation FirstArgLoc = CE->getArg(0)->getBeginLoc();
                SourceLocation LastArgEnd = Lexer::getLocForEndOfToken(
                    CE->getArg(NumArgs - 1)->getEndLoc(), 0, SM, LO);

                TheRewriter.ReplaceText(
                    CharSourceRange::getCharRange(FirstArgLoc, LastArgEnd),
                    new_args);
            } else {
                // Caller is NOT transformed — create compound literal
                std::string ptr_text = arg_texts[info.singleton_param_indices[0]];
                std::string compound = "(" + info.slice_type + "){" + ptr_text + ", 0}";

                std::string new_args = compound;
                for (unsigned i = 0; i < NumArgs; i++) {
                    if (std::find(info.singleton_param_indices.begin(),
                                  info.singleton_param_indices.end(),
                                  (int)i) != info.singleton_param_indices.end()) {
                        new_args += ", 0";
                    } else {
                        new_args += ", " + arg_texts[i];
                    }
                }

                SourceLocation FirstArgLoc = CE->getArg(0)->getBeginLoc();
                SourceLocation LastArgEnd = Lexer::getLocForEndOfToken(
                    CE->getArg(NumArgs - 1)->getEndLoc(), 0, SM, LO);

                TheRewriter.ReplaceText(
                    CharSourceRange::getCharRange(FirstArgLoc, LastArgEnd),
                    new_args);
            }
            continue;
        }

        // Handle pointer-pair and ptr+len functions
        if (info.base_param_index < 0)
            continue;

        if (caller_it != g_transformed_functions.end()) {
            // Both caller and callee are transformed
            const RustSliceInfo &caller_info = caller_it->second;
            std::string base_arg = arg_texts[info.base_param_index];

            // Check if base arg is the caller's base param (pass-through)
            bool is_pass_through = false;
            std::string caller_base_name;
            if (caller_info.base_param_index >= 0) {
                caller_base_name = EnclosingFD->getParamDecl(
                    caller_info.base_param_index)->getNameAsString();
                if (base_arg == caller_base_name) {
                    is_pass_through = true;
                }
            }

            auto buildSubslice = [&](const Expr *BaseArgExpr, const Expr *EndArgExpr) {
                std::string slice = caller_info.slice_param_name;

                // Translate base and end args using transformed names
                std::string trans_base = translateArgExpr(BaseArgExpr, EnclosingFD, Ctx);
                std::string trans_end = translateArgExpr(EndArgExpr, EnclosingFD, Ctx);

                std::string ptr_expr;
                if (trans_base == slice + ".ptr") {
                    ptr_expr = slice + ".ptr";
                } else {
                    ptr_expr = slice + ".ptr + " + trans_base;
                }

                std::string len_expr;
                // Check for full pass-through (both base and end match caller's)
                bool end_is_full = trans_end == (caller_info.inclusive_end ?
                                                 slice + ".len - 1" : slice + ".len");
                if (trans_base == slice + ".ptr" && end_is_full) {
                    len_expr = slice + ".len";
                } else if (end_is_full) {
                    // End is caller's full extent but base is shifted
                    // For inclusive: len = (arr.len-1) - base + 1 = arr.len - base
                    // For exclusive: len = arr.len - base
                    // Both simplify to arr.len - base
                    if (trans_base.find('+') != std::string::npos ||
                        trans_base.find('-') != std::string::npos) {
                        len_expr = slice + ".len - (" + trans_base + ")";
                    } else {
                        len_expr = slice + ".len - " + trans_base;
                    }
                } else {
                    if (info.inclusive_end) {
                        if (trans_base == slice + ".ptr") {
                            // len = trans_end + 1, but simplify "x - 1 + 1" → "x"
                            if (trans_end.size() >= 4 &&
                                trans_end.substr(trans_end.size() - 4) == " - 1") {
                                len_expr = trans_end.substr(0, trans_end.size() - 4);
                            } else {
                                len_expr = trans_end + " + 1";
                            }
                        } else {
                            len_expr = "(" + trans_end + ") - (" + trans_base + ") + 1";
                        }
                    } else {
                        if (trans_base == slice + ".ptr") {
                            len_expr = trans_end;
                        } else {
                            if (trans_base.find('+') != std::string::npos ||
                                trans_base.find('-') != std::string::npos) {
                                len_expr = slice + ".len - (" + trans_base + ")";
                            } else {
                                len_expr = slice + ".len - " + trans_base;
                            }
                        }
                    }
                }

                std::string compound = "(" + info.slice_type + "){" +
                                       ptr_expr + ", " + len_expr + "}";

                std::string new_args = compound;
                for (unsigned i = 0; i < NumArgs; i++) {
                    if ((int)i == info.base_param_index) continue;
                    if ((int)i == info.end_param_index) continue;
                    if ((int)i == info.len_param_index) continue;
                    new_args += ", " + translateArgExpr(CE->getArg(i), EnclosingFD, Ctx);
                }

                SourceLocation FirstArgLoc = CE->getArg(0)->getBeginLoc();
                SourceLocation LastArgEnd = Lexer::getLocForEndOfToken(
                    CE->getArg(NumArgs - 1)->getEndLoc(), 0, SM, LO);
                TheRewriter.ReplaceText(
                    CharSourceRange::getCharRange(FirstArgLoc, LastArgEnd),
                    new_args);
            };

            if (is_pass_through && info.end_param_index >= 0) {
                std::string end_arg = arg_texts[info.end_param_index];
                std::string caller_end_name = "";
                if (caller_info.end_param_index >= 0) {
                    caller_end_name = EnclosingFD->getParamDecl(
                        caller_info.end_param_index)->getNameAsString();
                }

                if (end_arg == caller_end_name) {
                    // Direct pass-through: callee(lo, hi) → callee(arr)
                    std::string new_args = caller_info.slice_param_name;
                    for (unsigned i = 0; i < NumArgs; i++) {
                        if ((int)i == info.base_param_index) continue;
                        if ((int)i == info.end_param_index) continue;
                        if ((int)i == info.len_param_index) continue;
                        new_args += ", " + arg_texts[i];
                    }

                    SourceLocation FirstArgLoc = CE->getArg(0)->getBeginLoc();
                    SourceLocation LastArgEnd = Lexer::getLocForEndOfToken(
                        CE->getArg(NumArgs - 1)->getEndLoc(), 0, SM, LO);
                    TheRewriter.ReplaceText(
                        CharSourceRange::getCharRange(FirstArgLoc, LastArgEnd),
                        new_args);
                } else {
                    // Subslice construction (base is pass-through, end differs)
                    buildSubslice(CE->getArg(info.base_param_index),
                                  CE->getArg(info.end_param_index));
                }
            } else if (info.end_param_index >= 0) {
                // Non-pass-through: both base and end may differ from caller's params
                buildSubslice(CE->getArg(info.base_param_index),
                              CE->getArg(info.end_param_index));
            }
        } else {
            // Non-transformed caller calling a transformed callee
            std::string base_text = arg_texts[info.base_param_index];
            std::string len_text;

            if (info.end_param_index >= 0) {
                std::string end_text = arg_texts[info.end_param_index];
                if (info.inclusive_end) {
                    std::string prefix = base_text + " + ";
                    if (end_text.substr(0, prefix.size()) == prefix) {
                        std::string suffix = end_text.substr(prefix.size());
                        // Simplify "x - 1 + 1" → "x"
                        if (suffix.size() >= 4 &&
                            suffix.substr(suffix.size() - 4) == " - 1") {
                            len_text = suffix.substr(0, suffix.size() - 4);
                        } else {
                            len_text = suffix + " + 1";
                        }
                    } else {
                        len_text = "(" + end_text + ") - " + base_text + " + 1";
                    }
                } else {
                    std::string prefix = base_text + " + ";
                    if (end_text.substr(0, prefix.size()) == prefix) {
                        len_text = end_text.substr(prefix.size());
                    } else {
                        if (base_text.find('+') != std::string::npos ||
                            base_text.find('-') != std::string::npos)
                            len_text = "(" + end_text + ") - (" + base_text + ")";
                        else
                            len_text = "(" + end_text + ") - " + base_text;
                    }
                }
            } else if (info.len_param_index >= 0) {
                len_text = arg_texts[info.len_param_index];
            }

            if (info.lookback > 0) {
                base_text = base_text + " - " + std::to_string(info.lookback);
                len_text = len_text + " + " + std::to_string(info.lookback);
            }
            if (info.lookahead > 0) {
                len_text = len_text + " + " + std::to_string(info.lookahead);
            }

            std::string compound = "(" + info.slice_type + "){" +
                                   base_text + ", " + len_text + "}";

            std::string new_args = compound;
            for (unsigned i = 0; i < NumArgs; i++) {
                if ((int)i == info.base_param_index) continue;
                if ((int)i == info.end_param_index) continue;
                if ((int)i == info.len_param_index) continue;
                new_args += ", " + arg_texts[i];
            }

            SourceLocation FirstArgLoc = CE->getArg(0)->getBeginLoc();
            SourceLocation LastArgEnd = Lexer::getLocForEndOfToken(
                CE->getArg(NumArgs - 1)->getEndLoc(), 0, SM, LO);
            TheRewriter.ReplaceText(
                CharSourceRange::getCharRange(FirstArgLoc, LastArgEnd),
                new_args);
        }
    }
}

// ============================================================================
// replaceRemovedParams — patch up references to base/end/len params
// inside the body of a RustSlice-transformed function.
// ============================================================================
//
// The signature rewrite drops these parameters, but the body may still
// mention them (e.g. `if (lo == NULL)` or `n - 1`). Each remaining use
// is rewritten to its slice-relative equivalent (arr.ptr / arr.len),
// with lookback/lookahead adjustments folded in when needed.

void FunctionAccessAnalyzer::replaceRemovedParams(const FunctionDecl *FD,
                                                    ASTContext &Ctx) {
    auto it = g_transformed_functions.find(FD->getCanonicalDecl());
    if (it == g_transformed_functions.end())
        return;

    const RustSliceInfo &info = it->second;
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    // Build the per-parameter replacement strings up-front.
    std::map<const ParmVarDecl *, std::string> param_replacements;
    const ParmVarDecl *base_param = nullptr;
    const ParmVarDecl *end_param = nullptr;

    if (info.base_param_index >= 0) {
        base_param = FD->getParamDecl(info.base_param_index);
        std::string repl = info.slice_param_name + ".ptr";
        if (info.lookback > 0)
            repl = "(" + info.slice_param_name + ".ptr + " +
                   std::to_string(info.lookback) + ")";
        param_replacements[base_param] = repl;
    }
    if (info.end_param_index >= 0) {
        end_param = FD->getParamDecl(info.end_param_index);
        std::string repl;
        if (info.inclusive_end)
            repl = "(" + info.slice_param_name + ".len - 1)";
        else
            repl = info.slice_param_name + ".len";
        if (info.lookahead > 0)
            repl = "(" + info.slice_param_name + ".len - " +
                   std::to_string(info.lookahead) + ")";
        param_replacements[end_param] = repl;
    }
    if (info.len_param_index >= 0) {
        std::string repl = info.slice_param_name + ".len";
        if (info.lookback > 0 || info.lookahead > 0)
            repl = "(" + info.slice_param_name + ".len - " +
                   std::to_string(info.lookback + info.lookahead) + ")";
        param_replacements[FD->getParamDecl(info.len_param_index)] = repl;
    }

    if (param_replacements.empty())
        return;

    std::sort(m_edited_ranges.begin(), m_edited_ranges.end());

    // Context-aware param reference replacement
    class ParamRefVisitor : public RecursiveASTVisitor<ParamRefVisitor> {
    public:
        Rewriter &Rewrite;
        SourceManager &SM;
        const LangOptions &LO;
        ASTContext &Ctx;
        const std::map<const ParmVarDecl *, std::string> &replacements;
        std::vector<std::pair<unsigned, unsigned>> &edited_ranges;
        const RustSliceInfo &info;
        const ParmVarDecl *base_param;
        const ParmVarDecl *end_param;

        ParamRefVisitor(Rewriter &R, SourceManager &SM, const LangOptions &LO,
                        ASTContext &Ctx,
                        const std::map<const ParmVarDecl *, std::string> &repls,
                        std::vector<std::pair<unsigned, unsigned>> &ranges,
                        const RustSliceInfo &info,
                        const ParmVarDecl *base_p, const ParmVarDecl *end_p)
            : Rewrite(R), SM(SM), LO(LO), Ctx(Ctx), replacements(repls),
              edited_ranges(ranges), info(info),
              base_param(base_p), end_param(end_p) {}

        void markEdited(SourceLocation Start, SourceLocation End) {
            edited_ranges.push_back({SM.getFileOffset(Start), SM.getFileOffset(End)});
        }

        bool isInEditedRange(unsigned offset) const {
            for (const auto &range : edited_ranges) {
                if (offset >= range.first && offset < range.second)
                    return true;
            }
            return false;
        }

        bool VisitDeclRefExpr(DeclRefExpr *DRE) {
            const ParmVarDecl *PD = dyn_cast<ParmVarDecl>(DRE->getDecl());
            if (!PD) return true;

            auto rep_it = replacements.find(PD);
            if (rep_it == replacements.end()) return true;

            unsigned offset = SM.getFileOffset(DRE->getBeginLoc());
            if (isInEditedRange(offset)) return true;

            // Check the parent expression for context-aware replacement
            const Stmt *Parent = skipTransparentParents(DRE, Ctx);

            // Case: *param (deref) — replace entire *param with arr.ptr[replacement_expr]
            if (Parent) {
                if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(Parent)) {
                    if (UO->getOpcode() == UO_Deref) {
                        SourceLocation StartLoc = UO->getBeginLoc();
                        SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                            UO->getEndLoc(), 0, SM, LO);

                        unsigned uo_offset = SM.getFileOffset(StartLoc);
                        if (!isInEditedRange(uo_offset)) {
                            // For *base_param: use arr.ptr[0]
                            // For *end_param: use arr.ptr[arr.len] or arr.ptr[arr.len-1]
                            std::string idx;
                            if (PD == base_param)
                                idx = "0";
                            else
                                idx = rep_it->second;
                            Rewrite.ReplaceText(
                                CharSourceRange::getCharRange(StartLoc, EndLoc),
                                info.slice_param_name + ".ptr[" + idx + "]");
                            markEdited(StartLoc, EndLoc);
                            return true;
                        }
                    }
                }

                // Case: comparison between two removed params (lo < hi → arr.len > 1)
                if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(Parent)) {
                    if (BO->isComparisonOp() && base_param && end_param) {
                        // Check if both sides are removed params
                        const DeclRefExpr *LDRE = dyn_cast<DeclRefExpr>(
                            BO->getLHS()->IgnoreImpCasts());
                        const DeclRefExpr *RDRE = dyn_cast<DeclRefExpr>(
                            BO->getRHS()->IgnoreImpCasts());

                        if (LDRE && RDRE) {
                            const ParmVarDecl *LPD = dyn_cast<ParmVarDecl>(LDRE->getDecl());
                            const ParmVarDecl *RPD = dyn_cast<ParmVarDecl>(RDRE->getDecl());

                            if ((LPD == base_param && RPD == end_param) ||
                                (LPD == end_param && RPD == base_param)) {
                                unsigned bo_offset = SM.getFileOffset(BO->getBeginLoc());
                                if (!isInEditedRange(bo_offset)) {
                                    // lo < hi → arr.len > 1 (inclusive) or > 0 (exclusive)
                                    std::string threshold = info.inclusive_end ? "1" : "0";
                                    std::string cmp;
                                    bool base_on_left = (LPD == base_param);
                                    switch (BO->getOpcode()) {
                                    case BO_LT:
                                        cmp = base_on_left ? "> " + threshold : "< " + threshold;
                                        break;
                                    case BO_LE:
                                        cmp = base_on_left ? ">= " + threshold : "<= " + threshold;
                                        break;
                                    case BO_GT:
                                        cmp = base_on_left ? "< " + threshold : "> " + threshold;
                                        break;
                                    case BO_GE:
                                        cmp = base_on_left ? "<= " + threshold : ">= " + threshold;
                                        break;
                                    case BO_NE:
                                        cmp = "!= 0";
                                        break;
                                    default:
                                        cmp = "> " + threshold;
                                        break;
                                    }

                                    SourceLocation StartLoc = BO->getBeginLoc();
                                    SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                                        BO->getEndLoc(), 0, SM, LO);
                                    Rewrite.ReplaceText(
                                        CharSourceRange::getCharRange(StartLoc, EndLoc),
                                        info.slice_param_name + ".len " + cmp);
                                    markEdited(StartLoc, EndLoc);
                                    return true;
                                }
                            }
                        }
                    }

                    // Case: (end - base) <cmp> N → arr.len <cmp> N
                    if (BO->isComparisonOp() && base_param && end_param) {
                        // Check if LHS is a subtraction of end - base
                        if (const BinaryOperator *Sub = dyn_cast<BinaryOperator>(
                                BO->getLHS()->IgnoreImpCasts())) {
                            if (Sub->getOpcode() == BO_Sub) {
                                const auto *SubL = dyn_cast<DeclRefExpr>(Sub->getLHS()->IgnoreImpCasts());
                                const auto *SubR = dyn_cast<DeclRefExpr>(Sub->getRHS()->IgnoreImpCasts());
                                if (SubL && SubR) {
                                    const auto *SubLP = dyn_cast<ParmVarDecl>(SubL->getDecl());
                                    const auto *SubRP = dyn_cast<ParmVarDecl>(SubR->getDecl());
                                    if (SubLP == end_param && SubRP == base_param) {
                                        unsigned bo_offset = SM.getFileOffset(BO->getBeginLoc());
                                        if (!isInEditedRange(bo_offset)) {
                                            // Replace "end - start <cmp> N" with "arr.len <cmp> N"
                                            SourceLocation SubStart = Sub->getBeginLoc();
                                            SourceLocation SubEnd = Lexer::getLocForEndOfToken(
                                                Sub->getEndLoc(), 0, SM, LO);
                                            Rewrite.ReplaceText(
                                                CharSourceRange::getCharRange(SubStart, SubEnd),
                                                info.slice_param_name + ".len");
                                            markEdited(SubStart, SubEnd);
                                            return true;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Case: param in a subtraction (end - base → arr.len)
                    if (BO->getOpcode() == BO_Sub && base_param && end_param) {
                        const auto *LDRE = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreImpCasts());
                        const auto *RDRE = dyn_cast<DeclRefExpr>(BO->getRHS()->IgnoreImpCasts());
                        if (LDRE && RDRE) {
                            const auto *LPD = dyn_cast<ParmVarDecl>(LDRE->getDecl());
                            const auto *RPD = dyn_cast<ParmVarDecl>(RDRE->getDecl());
                            if (LPD == end_param && RPD == base_param) {
                                unsigned bo_offset = SM.getFileOffset(BO->getBeginLoc());
                                if (!isInEditedRange(bo_offset)) {
                                    SourceLocation StartLoc = BO->getBeginLoc();
                                    SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                                        BO->getEndLoc(), 0, SM, LO);
                                    Rewrite.ReplaceText(
                                        CharSourceRange::getCharRange(StartLoc, EndLoc),
                                        info.slice_param_name + ".len");
                                    markEdited(StartLoc, EndLoc);
                                    return true;
                                }
                            }
                        }
                    }
                }
            }

            // Default: simple text replacement
            SourceLocation StartLoc = DRE->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                DRE->getEndLoc(), 0, SM, LO);

            Rewrite.ReplaceText(
                CharSourceRange::getCharRange(StartLoc, EndLoc), rep_it->second);
            markEdited(StartLoc, EndLoc);

            return true;
        }
    };


    ParamRefVisitor visitor(TheRewriter, SM, LO, Ctx, param_replacements,
                            m_edited_ranges, info, base_param, end_param);
    visitor.TraverseStmt(FD->getBody());
}

// ============================================================================
// rewriteForwardDeclarations — keep header prototypes in sync with the
// rewritten definitions.
// ============================================================================
//
// For every transformed function, we walk all redeclarations (skipping
// the definition itself) and rewrite the parameter list — and the
// return type, if it changed — to match. When the prototype lives in a
// non-main file we also drop the slice typedef in once at the top of
// that file so the prototype compiles.

void FunctionAccessAnalyzer::rewriteForwardDeclarations(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    std::set<FileID> typedef_emitted_files;

    for (auto &[FDCanon, info] : g_transformed_functions) {
        for (const FunctionDecl *Redecl : FDCanon->redecls()) {
            if (Redecl->isThisDeclarationADefinition())
                continue;
            if (SM.isInSystemHeader(Redecl->getLocation()))
                continue;
            if (Redecl->getNumParams() == 0)
                continue;

            // First time we touch a non-main file: drop the slice
            // typedef in front of the prototype.
            FileID DeclFID = SM.getFileID(Redecl->getLocation());
            FileID MainFID = SM.getMainFileID();
            if (DeclFID != MainFID &&
                typedef_emitted_files.find(DeclFID) == typedef_emitted_files.end()) {
                typedef_emitted_files.insert(DeclFID);
                std::string td = "typedef struct { " + info.pointee_type +
                    " *ptr; size_t len; } " + info.slice_type + ";\n\n";
                TheRewriter.InsertTextBefore(Redecl->getBeginLoc(), td);
            }

            // Build the rewritten parameter list. Same shape as the
            // body rewrite in TransformationMethods.cpp:
            //   - singleton: slice + (int <name>) for each pointer param
            //   - pointer-pair / ptr+len: slice + remaining params
            std::string new_params;
            if (!info.singleton_param_indices.empty()) {
                new_params = info.slice_type + " " + info.slice_param_name;
                for (unsigned i = 0; i < Redecl->getNumParams(); i++) {
                    const ParmVarDecl *P = Redecl->getParamDecl(i);
                    bool is_singleton = false;
                    for (int si : info.singleton_param_indices) {
                        if ((int)i == si) { is_singleton = true; break; }
                    }
                    if (is_singleton) {
                        new_params += ", int " + P->getNameAsString();
                    } else {
                        new_params += ", " + P->getType().getAsString();
                        if (!P->getNameAsString().empty())
                            new_params += " " + P->getNameAsString();
                    }
                }
            } else {
                new_params = info.slice_type + " " + info.slice_param_name;
                for (unsigned i = 0; i < Redecl->getNumParams(); i++) {
                    if ((int)i == info.base_param_index) continue;
                    if ((int)i == info.end_param_index) continue;
                    if ((int)i == info.len_param_index) continue;
                    const ParmVarDecl *P = Redecl->getParamDecl(i);
                    new_params += ", " + P->getType().getAsString();
                    if (!P->getNameAsString().empty())
                        new_params += " " + P->getNameAsString();
                }
            }

            // Replace parameter list
            SourceLocation FirstParamLoc = Redecl->getParamDecl(0)->getBeginLoc();
            unsigned LastIdx = Redecl->getNumParams() - 1;
            SourceLocation LastParamEnd = Lexer::getLocForEndOfToken(
                Redecl->getParamDecl(LastIdx)->getEndLoc(), 0, SM, LO);

            unsigned startOff = SM.getFileOffset(FirstParamLoc);
            unsigned endOff = SM.getFileOffset(LastParamEnd);
            TheRewriter.ReplaceText(FirstParamLoc, endOff - startOff, new_params);

            // Handle return type change
            if (info.return_type_changed) {
                SourceLocation RetStart = Redecl->getBeginLoc();
                SourceLocation FuncNameLoc = Redecl->getLocation();
                unsigned retLen = SM.getFileOffset(FuncNameLoc) - SM.getFileOffset(RetStart);
                TheRewriter.ReplaceText(RetStart, retLen, "int ");
            }

            if (VERBOSE)
                llvm::outs() << "[Phase5a] Rewrote forward declaration of "
                              << Redecl->getNameAsString() << "\n";
        }
    }

    // Same idea, but for functions whose entire return type was
    // collapsed to int (the global-return case in detectAllTransformations).
    for (auto &[FDCanon, gri] : g_global_return_functions) {
        for (const FunctionDecl *Redecl : FDCanon->redecls()) {
            if (Redecl->isThisDeclarationADefinition())
                continue;
            if (SM.isInSystemHeader(Redecl->getLocation()))
                continue;

            // Change return type from T* to int.
            rewriteReturnTypeAsInt(TheRewriter, Redecl, SM, LO);

            if (VERBOSE)
                llvm::outs() << "[Phase5a] Rewrote global-return forward declaration of "
                              << Redecl->getNameAsString() << "\n";
        }
    }
}

// ============================================================================
// fixReturnTypeChanges — propagate `T* -> int` return-type rewrites
// ============================================================================
//
// When a function's return type changes from a pointer type to int, we
// need to:
//   A. Rewrite every `return NULL` (or 0/__null) inside the body to
//      `return -1`.
//   B. Update callers: a local `T *p = func(...)` becomes `int p = ...;`,
//      and any subsequent uses of `p` need to dereference the global
//      array (`global_arr[p]`) instead of `*p`.
//
// The body of this method is split into Part A (return-value fix-up)
// and Part B (caller rewrites).

void FunctionAccessAnalyzer::fixReturnTypeChanges(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    // ---- Part A: rewrite `return NULL` to `return -1` -----------------
    for (auto &[FDCanon, info] : g_transformed_functions) {
        if (!info.return_type_changed)
            continue;

        auto fa_it = g_function_analyses.find(FDCanon);
        if (fa_it == g_function_analyses.end())
            continue;
        const FunctionDecl *FD = fa_it->second.FD;
        if (!FD || !FD->hasBody())
            continue;

        class ReturnNullFixer : public RecursiveASTVisitor<ReturnNullFixer> {
        public:
            Rewriter &Rewrite;
            SourceManager &SM;
            const LangOptions &LO;
            ASTContext &Ctx;

            ReturnNullFixer(Rewriter &R, SourceManager &SM, const LangOptions &LO, ASTContext &Ctx)
                : Rewrite(R), SM(SM), LO(LO), Ctx(Ctx) {}

            bool VisitReturnStmt(ReturnStmt *RS) {
                const Expr *RetVal = RS->getRetValue();
                if (!RetVal)
                    return true;

                // Get the source text of the return value
                SourceLocation ValStart = RetVal->getBeginLoc();
                SourceLocation ValEnd = Lexer::getLocForEndOfToken(
                    RetVal->getEndLoc(), 0, SM, LO);

                // Handle macro locations (e.g. NULL)
                if (ValStart.isMacroID())
                    ValStart = SM.getSpellingLoc(ValStart);
                if (ValEnd.isMacroID())
                    ValEnd = SM.getSpellingLoc(ValEnd);

                StringRef valText = Lexer::getSourceText(
                    CharSourceRange::getCharRange(ValStart, ValEnd), SM, LO);

                if (valText == "NULL" || valText == "0" || valText == "__null" ||
                    valText == "((void*)0)" || valText == "((void *)0)") {
                    unsigned len = SM.getFileOffset(ValEnd) - SM.getFileOffset(ValStart);
                    Rewrite.ReplaceText(ValStart, len, "-1");
                }
                return true;
            }
        };

        ReturnNullFixer fixer(TheRewriter, SM, LO, Ctx);
        fixer.TraverseStmt(FD->getBody());

        // Fallback: scan source text for "return NULL" / "return 0" that the
        // AST visitor may miss (e.g. when NULL is undefined due to missing headers)
        SourceLocation BodyStart = FD->getBody()->getBeginLoc();
        SourceLocation BodyEnd = FD->getBody()->getEndLoc();
        unsigned startOff = SM.getFileOffset(BodyStart);
        unsigned endOff = SM.getFileOffset(BodyEnd);
        StringRef bodyText = SM.getBufferData(SM.getFileID(BodyStart))
                                 .substr(startOff, endOff - startOff);
        // Search for "return NULL" or "return 0" patterns
        for (const char *pattern : {"return NULL", "return 0"}) {
            size_t pos = 0;
            StringRef pat(pattern);
            while ((pos = bodyText.find(pat, pos)) != StringRef::npos) {
                // Make sure the character after the match is ; or space/newline (not part of identifier)
                size_t afterPos = pos + pat.size();
                if (afterPos < bodyText.size()) {
                    char c = bodyText[afterPos];
                    if (c != ';' && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
                        pos = afterPos;
                        continue;
                    }
                }
                // Replace "NULL" or "0" with "-1"
                unsigned valStart = startOff + pos + strlen("return ");
                unsigned valLen = pat.size() - strlen("return ");
                SourceLocation valLoc = SM.getLocForStartOfFile(SM.getFileID(BodyStart))
                                            .getLocWithOffset(valStart);
                TheRewriter.ReplaceText(valLoc, valLen, "-1");
                pos = afterPos;
            }
        }
    }

    // Part B: Transform callers that receive return values from
    //         return-type-changed functions
    // Find all calls and their receiving variables
    class ReturnValueCallFinder : public RecursiveASTVisitor<ReturnValueCallFinder> {
    public:
        struct CallInfo {
            const CallExpr *CE;
            const VarDecl *RecvVar;        // variable receiving return value
            std::string base_text;          // base array text from call site
            bool is_init;                   // true = init, false = assignment
        };
        std::vector<CallInfo> calls;
        ASTContext &Ctx;

        ReturnValueCallFinder(ASTContext &C) : Ctx(C) {}

        bool VisitVarDecl(VarDecl *VD) {
            if (!VD->hasInit())
                return true;
            if (!VD->getType()->isPointerType())
                return true;

            const Expr *Init = VD->getInit()->IgnoreImpCasts();
            if (const auto *CE = dyn_cast<CallExpr>(Init)) {
                checkCall(CE, VD);
            }
            return true;
        }

        bool VisitBinaryOperator(BinaryOperator *BO) {
            if (BO->getOpcode() != BO_Assign)
                return true;
            const auto *LHS = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreImpCasts());
            if (!LHS)
                return true;
            const auto *VD = dyn_cast<VarDecl>(LHS->getDecl());
            if (!VD || !VD->getType()->isPointerType())
                return true;
            const Expr *RHS = BO->getRHS()->IgnoreImpCasts();
            if (const auto *CE = dyn_cast<CallExpr>(RHS)) {
                checkCall(CE, nullptr);
            }
            return true;
        }

    private:
        void checkCall(const CallExpr *CE, const VarDecl *RecvVD) {
            const FunctionDecl *Callee = CE->getDirectCallee();
            if (!Callee)
                return;
            auto it = g_transformed_functions.find(Callee->getCanonicalDecl());
            if (it == g_transformed_functions.end() || !it->second.return_type_changed)
                return;

            const RustSliceInfo &info = it->second;

            // Extract base text from the call's arguments
            std::string base = "";
            if (info.base_param_index >= 0 &&
                (unsigned)info.base_param_index < CE->getNumArgs()) {
                const Expr *BaseArg = CE->getArg(info.base_param_index);
                // If it's a compound literal (RustSlice_int){base, len}, extract base
                if (const auto *CLE = dyn_cast<CompoundLiteralExpr>(
                        BaseArg->IgnoreImpCasts())) {
                    if (const auto *ILE = dyn_cast<InitListExpr>(CLE->getInitializer())) {
                        if (ILE->getNumInits() > 0) {
                            SourceManager &SM = Ctx.getSourceManager();
                            const LangOptions &LO = Ctx.getLangOpts();
                            base = getSourceText(ILE->getInit(0), SM, LO);
                        }
                    }
                } else {
                    SourceManager &SM = Ctx.getSourceManager();
                    const LangOptions &LO = Ctx.getLangOpts();
                    base = getSourceText(BaseArg, SM, LO);
                }
            }

            CallInfo ci;
            ci.CE = CE;
            ci.RecvVar = RecvVD;
            ci.base_text = base;
            ci.is_init = (RecvVD != nullptr);
            calls.push_back(ci);
        }
    };

    ReturnValueCallFinder finder(Ctx);
    finder.TraverseDecl(Ctx.getTranslationUnitDecl());

    // Collect all receiving VarDecls and their base texts
    std::map<const VarDecl *, std::string> recv_var_bases;
    for (const auto &ci : finder.calls) {
        if (ci.RecvVar) {
            recv_var_bases[ci.RecvVar] = ci.base_text;
            g_index_return_vars.insert(ci.RecvVar);
        }
    }

    // Transform each receiving variable
    for (auto &[VD, base_text] : recv_var_bases) {
        // Find the enclosing function first
        const FunctionDecl *EnclosingFD = nullptr;
        for (auto &[fdCanon, fa] : g_function_analyses) {
            if (!fa.FD || !fa.FD->hasBody())
                continue;
            SourceLocation bodyStart = fa.FD->getBody()->getBeginLoc();
            SourceLocation bodyEnd = fa.FD->getBody()->getEndLoc();
            SourceLocation varLoc = VD->getLocation();
            if (SM.isBeforeInTranslationUnit(bodyStart, varLoc) &&
                SM.isBeforeInTranslationUnit(varLoc, bodyEnd)) {
                EnclosingFD = fa.FD;
                break;
            }
        }

        if (!EnclosingFD)
            continue;

        // Skip if the enclosing function was already transformed by Phase 4
        // (generatePointerPairTransformation already handled the declaration
        // and usage changes for this variable)
        if (g_transformed_functions.count(EnclosingFD->getCanonicalDecl()))
            continue;

        // Change declaration type: int *var → int var
        const DeclStmt *DS = nullptr;
        // Search all function bodies for the DeclStmt
        for (auto &[fdCanon, fa] : g_function_analyses) {
            if (fa.FD && fa.FD->hasBody()) {
                DS = findDeclStmtForVar(VD, fa.FD->getBody());
                if (DS)
                    break;
            }
        }

        if (DS) {
            // Replace the type part: "int *var" → "int var"
            SourceLocation TypeStart = DS->getBeginLoc();
            SourceLocation NameLoc = VD->getLocation();
            unsigned origLen = SM.getFileOffset(NameLoc) - SM.getFileOffset(TypeStart);
            TheRewriter.ReplaceText(TypeStart, origLen, "int ");
        }

        class VarUsageTransformer : public RecursiveASTVisitor<VarUsageTransformer> {
        public:
            Rewriter &Rewrite;
            SourceManager &SM;
            const LangOptions &LO;
            ASTContext &Ctx;
            const VarDecl *TargetVar;
            const std::string &BaseText;

            VarUsageTransformer(Rewriter &R, SourceManager &SM, const LangOptions &LO,
                                ASTContext &C, const VarDecl *V, const std::string &B)
                : Rewrite(R), SM(SM), LO(LO), Ctx(C), TargetVar(V), BaseText(B) {}

            bool VisitDeclRefExpr(DeclRefExpr *DRE) {
                if (DRE->getDecl() != TargetVar)
                    return true;

                const Stmt *Parent = skipTransparentParents(DRE, Ctx);
                if (!Parent)
                    return true;

                // Case: *var → base[var]
                if (const auto *UO = dyn_cast<UnaryOperator>(Parent)) {
                    if (UO->getOpcode() == UO_Deref) {
                        std::string varName = TargetVar->getNameAsString();
                        SourceLocation Start = UO->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            UO->getEndLoc(), 0, SM, LO);
                        unsigned len = SM.getFileOffset(End) - SM.getFileOffset(Start);
                        Rewrite.ReplaceText(Start, len,
                            BaseText + "[" + varName + "]");
                        return true;
                    }
                }

                // Case: if (var) → if (var != -1)
                // Case: if (!var) → if (var == -1)  (parent would be UnaryOperator LNot)
                if (const auto *UO = dyn_cast<UnaryOperator>(Parent)) {
                    if (UO->getOpcode() == UO_LNot) {
                        std::string varName = TargetVar->getNameAsString();
                        SourceLocation Start = UO->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            UO->getEndLoc(), 0, SM, LO);
                        unsigned len = SM.getFileOffset(End) - SM.getFileOffset(Start);
                        Rewrite.ReplaceText(Start, len,
                            varName + " == -1");
                        return true;
                    }
                }

                // Case: implicit boolean (if (var) where parent is IfStmt/While/etc)
                if (isa<IfStmt>(Parent) || isa<WhileStmt>(Parent) ||
                    isa<ConditionalOperator>(Parent)) {
                    // Check if DRE is the condition
                    if (const auto *IS = dyn_cast<IfStmt>(Parent)) {
                        if (IS->getCond()->IgnoreImpCasts() == DRE) {
                            std::string varName = TargetVar->getNameAsString();
                            SourceLocation Start = DRE->getBeginLoc();
                            SourceLocation End = Lexer::getLocForEndOfToken(
                                DRE->getEndLoc(), 0, SM, LO);
                            unsigned len = SM.getFileOffset(End) - SM.getFileOffset(Start);
                            Rewrite.ReplaceText(Start, len,
                                varName + " != -1");
                            return true;
                        }
                    }
                }

                // Case: var - base → var (pointer difference giving index)
                if (const auto *BO = dyn_cast<BinaryOperator>(Parent)) {
                    if (BO->getOpcode() == BO_Sub) {
                        const auto *RhsDRE = dyn_cast<DeclRefExpr>(
                            BO->getRHS()->IgnoreImpCasts());
                        if (RhsDRE) {
                            std::string rhs = getSourceText(BO->getRHS(), SM, LO);
                            if (rhs == BaseText) {
                                // var - base → var
                                std::string varName = TargetVar->getNameAsString();
                                // Check if wrapped in a cast: (int)(var - base) → var
                                const Stmt *GrandParent = skipTransparentParents(BO, Ctx);
                                if (GrandParent) {
                                    if (const auto *CSC = dyn_cast<CStyleCastExpr>(GrandParent)) {
                                        SourceLocation Start = CSC->getBeginLoc();
                                        SourceLocation End = Lexer::getLocForEndOfToken(
                                            CSC->getEndLoc(), 0, SM, LO);
                                        unsigned len = SM.getFileOffset(End) - SM.getFileOffset(Start);
                                        Rewrite.ReplaceText(Start, len, varName);
                                        return true;
                                    }
                                }
                                SourceLocation Start = BO->getBeginLoc();
                                SourceLocation End = Lexer::getLocForEndOfToken(
                                    BO->getEndLoc(), 0, SM, LO);
                                unsigned len = SM.getFileOffset(End) - SM.getFileOffset(Start);
                                Rewrite.ReplaceText(Start, len, varName);
                                return true;
                            }
                        }
                    }
                }

                return true;
            }
        };

        VarUsageTransformer transformer(TheRewriter, SM, LO, Ctx, VD, base_text);
        transformer.TraverseStmt(EnclosingFD->getBody());
    }

    // Part C: Transform functions that return pointers into global/static arrays
    for (auto &[FDCanon, gri] : g_global_return_functions) {
        auto fa_it = g_function_analyses.find(FDCanon);
        if (fa_it == g_function_analyses.end())
            continue;
        const FunctionDecl *FD = fa_it->second.FD;
        if (!FD || !FD->hasBody())
            continue;

        // C1: Change return type from T* to int
        rewriteReturnTypeAsInt(TheRewriter, FD, SM, LO);

        // C2: Transform return statements
        class GlobalReturnFixer : public RecursiveASTVisitor<GlobalReturnFixer> {
        public:
            Rewriter &Rewrite;
            SourceManager &SM;
            const LangOptions &LO;
            const std::string &GlobalArrayName;

            GlobalReturnFixer(Rewriter &R, SourceManager &SM, const LangOptions &LO,
                              const std::string &Name)
                : Rewrite(R), SM(SM), LO(LO), GlobalArrayName(Name) {}

            bool VisitReturnStmt(ReturnStmt *RS) {
                const Expr *RetVal = RS->getRetValue();
                if (!RetVal)
                    return true;

                // Check for NULL at AST level first
                const Expr *Stripped = RetVal->IgnoreParenImpCasts();
                bool isNullAST = false;
                if (const auto *IL = dyn_cast<IntegerLiteral>(Stripped)) {
                    if (IL->getValue() == 0) isNullAST = true;
                }
                if (dyn_cast<GNUNullExpr>(Stripped)) isNullAST = true;
                if (const auto *CSC = dyn_cast<CStyleCastExpr>(Stripped)) {
                    const Expr *Sub = CSC->getSubExpr()->IgnoreParenImpCasts();
                    if (const auto *IL = dyn_cast<IntegerLiteral>(Sub)) {
                        if (IL->getValue() == 0) isNullAST = true;
                    }
                }

                SourceLocation ValStart = RetVal->getBeginLoc();
                SourceLocation ValEnd = RetVal->getEndLoc();

                // Handle macro locations (e.g. NULL) — use expansion loc
                if (ValStart.isMacroID()) {
                    auto ExpRange = SM.getExpansionRange(RetVal->getSourceRange());
                    ValStart = ExpRange.getBegin();
                    ValEnd = ExpRange.getEnd();
                }
                SourceLocation ValEndTok = Lexer::getLocForEndOfToken(ValEnd, 0, SM, LO);

                // NULL/0 → -1 (AST or text check)
                if (isNullAST) {
                    unsigned len = SM.getFileOffset(ValEndTok) - SM.getFileOffset(ValStart);
                    Rewrite.ReplaceText(ValStart, len, "-1");
                    return true;
                }

                StringRef valText = Lexer::getSourceText(
                    CharSourceRange::getCharRange(ValStart, ValEndTok), SM, LO);
                if (valText == "NULL" || valText == "0" || valText == "__null" ||
                    valText == "((void*)0)" || valText == "((void *)0)") {
                    unsigned len = SM.getFileOffset(ValEndTok) - SM.getFileOffset(ValStart);
                    Rewrite.ReplaceText(ValStart, len, "-1");
                    return true;
                }

                // &global_array[expr] → expr
                const Expr *RV = RetVal->IgnoreImpCasts();
                if (const auto *UO = dyn_cast<UnaryOperator>(RV)) {
                    if (UO->getOpcode() == UO_AddrOf) {
                        const Expr *Sub = UO->getSubExpr()->IgnoreImpCasts();
                        if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Sub)) {
                            const Expr *Idx = ASE->getIdx();
                            // Get the index expression text
                            SourceLocation IdxStart = Idx->getBeginLoc();
                            SourceLocation IdxEnd = Lexer::getLocForEndOfToken(
                                Idx->getEndLoc(), 0, SM, LO);
                            StringRef idxText = Lexer::getSourceText(
                                CharSourceRange::getCharRange(IdxStart, IdxEnd), SM, LO);

                            unsigned len = SM.getFileOffset(ValEndTok) - SM.getFileOffset(ValStart);
                            Rewrite.ReplaceText(ValStart, len, idxText.str());
                            return true;
                        }
                    }
                }

                return true;
            }
        };

        GlobalReturnFixer fixer(TheRewriter, SM, LO, gri.global_array_name);
        fixer.TraverseStmt(FD->getBody());

        if (VERBOSE)
            llvm::outs() << "[Phase5b-C] Transformed global-return function: "
                          << FD->getNameAsString() << "\n";
    }

    // Calls whose return value is consumed directly do not have a receiving
    // variable for Part D to rewrite.  Fix pointer-null comparisons in place
    // now that the callee returns -1 for null.
    class GlobalReturnDirectCallTransformer
        : public RecursiveASTVisitor<GlobalReturnDirectCallTransformer> {
    public:
        Rewriter &Rewrite;
        SourceManager &SM;
        const LangOptions &LO;

        GlobalReturnDirectCallTransformer(Rewriter &R, SourceManager &SM,
                                          const LangOptions &LO)
            : Rewrite(R), SM(SM), LO(LO) {}

        bool VisitBinaryOperator(BinaryOperator *BO) {
            if (BO->getOpcode() != BO_EQ && BO->getOpcode() != BO_NE)
                return true;

            const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
            const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
            const Expr *NullExpr = nullptr;
            if (isGlobalReturnCall(LHS) && isNullLike(RHS))
                NullExpr = BO->getRHS();
            else if (isGlobalReturnCall(RHS) && isNullLike(LHS))
                NullExpr = BO->getLHS();

            if (!NullExpr)
                return true;

            SourceLocation Start = NullExpr->getBeginLoc();
            SourceLocation End = NullExpr->getEndLoc();
            if (Start.isMacroID() || End.isMacroID()) {
                auto Expansion = SM.getExpansionRange(NullExpr->getSourceRange());
                Start = Expansion.getBegin();
                End = Expansion.getEnd();
            }
            End = Lexer::getLocForEndOfToken(End, 0, SM, LO);
            Rewrite.ReplaceText(Start, SM.getFileOffset(End) - SM.getFileOffset(Start), "-1");
            return true;
        }

    private:
        static bool isGlobalReturnCall(const Expr *E) {
            const auto *CE = dyn_cast<CallExpr>(E);
            if (!CE)
                return false;
            const FunctionDecl *Callee = CE->getDirectCallee();
            return Callee &&
                g_global_return_functions.count(Callee->getCanonicalDecl());
        }

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
    };

    GlobalReturnDirectCallTransformer directCallTransformer(TheRewriter, SM, LO);
    directCallTransformer.TraverseDecl(Ctx.getTranslationUnitDecl());

    // Part D: Transform callers of global-return functions
    // Find all calls and their receiving variables
    class GlobalReturnCallFinder : public RecursiveASTVisitor<GlobalReturnCallFinder> {
    public:
        struct CallInfo {
            const CallExpr *CE;
            const VarDecl *RecvVar;
            std::string global_array_name;
        };
        std::vector<CallInfo> calls;
        ASTContext &Ctx;

        GlobalReturnCallFinder(ASTContext &C) : Ctx(C) {}

        bool VisitVarDecl(VarDecl *VD) {
            if (!VD->hasInit())
                return true;
            if (!VD->getType()->isPointerType())
                return true;

            const Expr *Init = VD->getInit()->IgnoreImpCasts();
            if (const auto *CE = dyn_cast<CallExpr>(Init)) {
                checkCall(CE, VD);
            }
            return true;
        }

        bool VisitBinaryOperator(BinaryOperator *BO) {
            if (BO->getOpcode() != BO_Assign)
                return true;
            const auto *LHS = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreImpCasts());
            if (!LHS)
                return true;
            const auto *VD = dyn_cast<VarDecl>(LHS->getDecl());
            if (!VD || !VD->getType()->isPointerType())
                return true;
            const Expr *RHS = BO->getRHS()->IgnoreImpCasts();
            if (const auto *CE = dyn_cast<CallExpr>(RHS)) {
                checkCall(CE, VD);
            }
            return true;
        }

    private:
        void checkCall(const CallExpr *CE, const VarDecl *RecvVD) {
            const FunctionDecl *Callee = CE->getDirectCallee();
            if (!Callee)
                return;
            auto it = g_global_return_functions.find(Callee->getCanonicalDecl());
            if (it == g_global_return_functions.end())
                return;

            CallInfo ci;
            ci.CE = CE;
            ci.RecvVar = RecvVD;
            ci.global_array_name = it->second.global_array_name;
            calls.push_back(ci);
        }
    };

    GlobalReturnCallFinder gFinder(Ctx);
    gFinder.TraverseDecl(Ctx.getTranslationUnitDecl());

    // Collect receiving variables and their global array bases
    std::map<const VarDecl *, std::string> global_recv_vars;
    for (const auto &ci : gFinder.calls) {
        if (ci.RecvVar) {
            global_recv_vars[ci.RecvVar] = ci.global_array_name;
        }
    }

    // Transform each receiving variable's declaration and usages
    for (auto &[VD, global_array] : global_recv_vars) {
        // Find the enclosing function
        const FunctionDecl *EnclosingFD = nullptr;
        for (auto &[fdCanon, fa] : g_function_analyses) {
            if (!fa.FD || !fa.FD->hasBody())
                continue;
            SourceLocation bodyStart = fa.FD->getBody()->getBeginLoc();
            SourceLocation bodyEnd = fa.FD->getBody()->getEndLoc();
            SourceLocation varLoc = VD->getLocation();
            if (SM.isBeforeInTranslationUnit(bodyStart, varLoc) &&
                SM.isBeforeInTranslationUnit(varLoc, bodyEnd)) {
                EnclosingFD = fa.FD;
                break;
            }
        }

        if (!EnclosingFD)
            continue;

        // Skip if the enclosing function was already transformed
        if (g_transformed_functions.count(EnclosingFD->getCanonicalDecl()))
            continue;

        // Change declaration type: T *var → int var
        const DeclStmt *DS = nullptr;
        for (auto &[fdCanon, fa] : g_function_analyses) {
            if (fa.FD && fa.FD->hasBody()) {
                DS = findDeclStmtForVar(VD, fa.FD->getBody());
                if (DS)
                    break;
            }
        }

        if (DS) {
            SourceLocation TypeStart = DS->getBeginLoc();
            SourceLocation NameLoc = VD->getLocation();
            unsigned origLen = SM.getFileOffset(NameLoc) - SM.getFileOffset(TypeStart);
            TheRewriter.ReplaceText(TypeStart, origLen, "int ");
        }

        // Transform usages of the receiving variable
        class GlobalVarUsageTransformer : public RecursiveASTVisitor<GlobalVarUsageTransformer> {
        public:
            Rewriter &Rewrite;
            SourceManager &SM;
            const LangOptions &LO;
            ASTContext &Ctx;
            const VarDecl *TargetVar;
            const std::string &GlobalArray;

            GlobalVarUsageTransformer(Rewriter &R, SourceManager &SM, const LangOptions &LO,
                                      ASTContext &C, const VarDecl *V, const std::string &G)
                : Rewrite(R), SM(SM), LO(LO), Ctx(C), TargetVar(V), GlobalArray(G) {}

            bool VisitDeclRefExpr(DeclRefExpr *DRE) {
                if (DRE->getDecl() != TargetVar)
                    return true;

                const Stmt *Parent = skipTransparentParents(DRE, Ctx);
                if (!Parent)
                    return true;

                std::string varName = TargetVar->getNameAsString();

                // Case: var->field → global_array[var].field
                if (const auto *ME = dyn_cast<MemberExpr>(Parent)) {
                    if (ME->isArrow()) {
                        std::string field = ME->getMemberDecl()->getNameAsString();
                        SourceLocation Start = ME->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            ME->getEndLoc(), 0, SM, LO);
                        unsigned len = SM.getFileOffset(End) - SM.getFileOffset(Start);
                        Rewrite.ReplaceText(Start, len,
                            GlobalArray + "[" + varName + "]." + field);
                        return true;
                    }
                }

                // Case: *var → global_array[var]
                if (const auto *UO = dyn_cast<UnaryOperator>(Parent)) {
                    if (UO->getOpcode() == UO_Deref) {
                        SourceLocation Start = UO->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            UO->getEndLoc(), 0, SM, LO);
                        unsigned len = SM.getFileOffset(End) - SM.getFileOffset(Start);
                        Rewrite.ReplaceText(Start, len,
                            GlobalArray + "[" + varName + "]");
                        return true;
                    }
                    // Case: !var → var == -1
                    if (UO->getOpcode() == UO_LNot) {
                        SourceLocation Start = UO->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            UO->getEndLoc(), 0, SM, LO);
                        unsigned len = SM.getFileOffset(End) - SM.getFileOffset(Start);
                        Rewrite.ReplaceText(Start, len, varName + " == -1");
                        return true;
                    }
                }

                // Case: var == NULL / var != NULL → var == -1 / var != -1
                if (const auto *BO = dyn_cast<BinaryOperator>(Parent)) {
                    if (BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE) {
                        // Check if the other side is NULL
                        const Expr *Other = nullptr;
                        if (BO->getLHS()->IgnoreImpCasts() == DRE)
                            Other = BO->getRHS()->IgnoreImpCasts();
                        else
                            Other = BO->getLHS()->IgnoreImpCasts();

                        bool isNull = false;
                        const Expr *OtherStripped = Other->IgnoreParens();
                        if (const auto *IL = dyn_cast<IntegerLiteral>(OtherStripped)) {
                            if (IL->getValue() == 0) isNull = true;
                        }
                        if (dyn_cast<GNUNullExpr>(OtherStripped)) isNull = true;
                        if (const auto *CSC = dyn_cast<CStyleCastExpr>(OtherStripped)) {
                            const Expr *Sub = CSC->getSubExpr()->IgnoreParenImpCasts();
                            if (const auto *IL = dyn_cast<IntegerLiteral>(Sub)) {
                                if (IL->getValue() == 0) isNull = true;
                            }
                        }
                        // Also check source text for "NULL" macro
                        if (!isNull) {
                            SourceLocation OtherStart = Other->getBeginLoc();
                            SourceLocation OtherEnd = Other->getEndLoc();
                            if (OtherStart.isMacroID()) {
                                auto ExpRange = SM.getExpansionRange(
                                    SourceRange(OtherStart, OtherEnd));
                                OtherStart = ExpRange.getBegin();
                                OtherEnd = ExpRange.getEnd();
                            }
                            SourceLocation OtherEndTok = Lexer::getLocForEndOfToken(
                                OtherEnd, 0, SM, LO);
                            StringRef otherText = Lexer::getSourceText(
                                CharSourceRange::getCharRange(OtherStart, OtherEndTok), SM, LO);
                            if (otherText == "NULL" || otherText == "0" || otherText == "__null")
                                isNull = true;
                        }

                        if (isNull) {
                            std::string op = (BO->getOpcode() == BO_EQ) ? " == " : " != ";
                            SourceLocation Start = BO->getBeginLoc();
                            SourceLocation End = BO->getEndLoc();
                            if (Start.isMacroID() || End.isMacroID()) {
                                auto ExpRange = SM.getExpansionRange(
                                    BO->getSourceRange());
                                Start = ExpRange.getBegin();
                                End = ExpRange.getEnd();
                            }
                            SourceLocation EndTok = Lexer::getLocForEndOfToken(
                                End, 0, SM, LO);
                            unsigned len = SM.getFileOffset(EndTok) - SM.getFileOffset(Start);
                            Rewrite.ReplaceText(Start, len, varName + op + "-1");
                            return true;
                        }
                    }
                }

                // Case: implicit boolean (if (var)) → if (var != -1)
                if (isa<IfStmt>(Parent) || isa<WhileStmt>(Parent) ||
                    isa<ConditionalOperator>(Parent)) {
                    if (const auto *IS = dyn_cast<IfStmt>(Parent)) {
                        if (IS->getCond()->IgnoreImpCasts() == DRE) {
                            SourceLocation Start = DRE->getBeginLoc();
                            SourceLocation End = Lexer::getLocForEndOfToken(
                                DRE->getEndLoc(), 0, SM, LO);
                            unsigned len = SM.getFileOffset(End) - SM.getFileOffset(Start);
                            Rewrite.ReplaceText(Start, len, varName + " != -1");
                            return true;
                        }
                    }
                }

                // The variable now stores an index, but an unchanged callee
                // still expects the pointer produced by the original helper.
                // Reconstruct that pointer at the argument boundary.
                if (const auto *CE = dyn_cast<CallExpr>(Parent)) {
                    for (const Expr *Arg : CE->arguments()) {
                        if (Arg->IgnoreParenImpCasts() != DRE)
                            continue;
                        SourceLocation Start = DRE->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            DRE->getEndLoc(), 0, SM, LO);
                        unsigned len = SM.getFileOffset(End) - SM.getFileOffset(Start);
                        Rewrite.ReplaceText(Start, len,
                            "&" + GlobalArray + "[" + varName + "]");
                        return true;
                    }
                }

                return true;
            }
        };

        GlobalVarUsageTransformer transformer(TheRewriter, SM, LO, Ctx, VD, global_array);
        transformer.TraverseStmt(EnclosingFD->getBody());

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
