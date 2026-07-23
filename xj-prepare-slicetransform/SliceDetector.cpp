// SliceDetector.cpp — see SliceDetector.h for the sub-phase overview.
//
// The detection rules here are a port of the pre-split pointer tool's
// detectAllTransformations, re-anchored on the *index-transformed*
// source: where the old code classified accesses of the original
// pointers, this code matches the plain index forms the pointer pass
// emits (`idx < n`, `idx < (end - base)`, `base + idx OP end`,
// `return base + idx`). Metadata records supply pointer identity; every
// record is verified against the AST before it is trusted.

#include "SliceDetector.h"

#include "clang/AST/ParentMapContext.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/Support/Path.h"

using namespace clang;

namespace xj {

// ============================================================================
// Small helpers
// ============================================================================

static std::string fileBasenameOf(const FunctionDecl *FD, SourceManager &SM) {
    if (auto FE = SM.getFileEntryRefForID(
            SM.getFileID(SM.getSpellingLoc(FD->getLocation()))))
        return llvm::sys::path::filename(FE->getName()).str();
    return "";
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

// The named decl a bare (possibly parenthesized/cast) reference resolves
// to, or nullptr when the expression is anything more complex.
static const ValueDecl *bareDeclRef(const Expr *E) {
    const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
    return DRE ? DRE->getDecl() : nullptr;
}

// Does `S`'s subtree mention `D`?
static bool mentionsDecl(const Stmt *S, const ValueDecl *D) {
    if (!S || !D)
        return false;
    class Finder : public RecursiveASTVisitor<Finder> {
      public:
        const ValueDecl *Target;
        bool Found = false;
        explicit Finder(const ValueDecl *T) : Target(T) {}
        bool VisitDeclRefExpr(DeclRefExpr *DRE) {
            if (DRE->getDecl() == Target) {
                Found = true;
                return false;
            }
            return true;
        }
    };
    Finder F(D);
    F.TraverseStmt(const_cast<Stmt *>(S));
    return F.Found;
}

static bool mentionsAnyDecl(const Stmt *S,
                            const std::set<const ValueDecl *> &Ds) {
    for (const ValueDecl *D : Ds)
        if (mentionsDecl(S, D))
            return true;
    return false;
}

// Does `S`'s subtree mention a pointer-typed *local* variable (i.e. a
// pointer the index rewrite left untouched)? Parameters excluded.
static bool mentionsRawPointerLocal(const Stmt *S) {
    if (!S)
        return false;
    class Finder : public RecursiveASTVisitor<Finder> {
      public:
        bool Found = false;
        bool VisitDeclRefExpr(DeclRefExpr *DRE) {
            const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
            if (VD && !isa<ParmVarDecl>(VD) && VD->getType()->isPointerType() &&
                !VD->hasGlobalStorage()) {
                Found = true;
                return false;
            }
            return true;
        }
    };
    Finder F;
    F.TraverseStmt(const_cast<Stmt *>(S));
    return F.Found;
}

// Step up to the first parent that isn't an ImplicitCastExpr or ParenExpr.
static const Stmt *skipTransparentParents(const Stmt *S, ASTContext &Ctx) {
    const Stmt *Current = S;
    while (true) {
        auto Parents = Ctx.getParents(*Current);
        if (Parents.empty())
            return nullptr;
        const Stmt *P = Parents[0].get<Stmt>();
        if (!P)
            return nullptr;
        if (isa<ImplicitCastExpr>(P) || isa<ParenExpr>(P)) {
            Current = P;
            continue;
        }
        return P;
    }
}

// Find the local VarDecl named `name` in `Body` (nullptr if absent).
static const VarDecl *findLocalVarNamed(Stmt *Body, const std::string &name) {
    class Finder : public RecursiveASTVisitor<Finder> {
      public:
        const std::string &Name;
        const VarDecl *Found = nullptr;
        explicit Finder(const std::string &N) : Name(N) {}
        bool VisitVarDecl(VarDecl *VD) {
            if (VD->getNameAsString() == Name) {
                Found = VD;
                return false;
            }
            return true;
        }
    };
    Finder F(name);
    F.TraverseStmt(Body);
    return F.Found;
}

// Collect comparison operators in source (traversal) order.
static std::vector<const BinaryOperator *> collectComparisons(Stmt *Body) {
    class Collector : public RecursiveASTVisitor<Collector> {
      public:
        std::vector<const BinaryOperator *> out;
        bool VisitBinaryOperator(BinaryOperator *BO) {
            if (BO->isComparisonOp())
                out.push_back(BO);
            return true;
        }
    };
    Collector C;
    C.TraverseStmt(Body);
    return C.out;
}

// Is some `*param` dereference (read or write) present in `Body`?
static bool paramIsDereferenced(Stmt *Body, const ParmVarDecl *PD) {
    class Finder : public RecursiveASTVisitor<Finder> {
      public:
        const ParmVarDecl *Target;
        bool Found = false;
        explicit Finder(const ParmVarDecl *P) : Target(P) {}
        bool VisitUnaryOperator(UnaryOperator *UO) {
            if (UO->getOpcode() != UO_Deref)
                return true;
            if (bareDeclRef(UO->getSubExpr()) == Target) {
                Found = true;
                return false;
            }
            return true;
        }
    };
    Finder F(PD);
    F.TraverseStmt(Body);
    return F.Found;
}

// Every return in the function is NULL-like or `base + idx` with `idx`
// one of the index variables iterating over the slice's base. Anything
// else (an unrelated pointer, a raw untransformed pointer) makes the
// T*→int rewrite unsafe.
static bool allReturnsAreIndexShaped(const FunctionDecl *FD,
                                     const ParmVarDecl *BasePD,
                                     const std::set<std::string> &BaseIdxVars) {
    class Checker : public RecursiveASTVisitor<Checker> {
      public:
        const ParmVarDecl *BasePD;
        const std::set<std::string> &BaseIdxVars;
        bool any_return = false;
        bool all_safe = true;

        Checker(const ParmVarDecl *B, const std::set<std::string> &I)
            : BasePD(B), BaseIdxVars(I) {}

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

            if (const auto *Add = dyn_cast<BinaryOperator>(Stripped)) {
                if (Add->getOpcode() == BO_Add &&
                    bareDeclRef(Add->getLHS()) == BasePD) {
                    if (const auto *RD = bareDeclRef(Add->getRHS())) {
                        if (BaseIdxVars.count(RD->getNameAsString()))
                            return true;
                    }
                }
            }

            all_safe = false;
            return false;
        }
    };

    Checker C(BasePD, BaseIdxVars);
    C.TraverseStmt(FD->getBody());
    return C.any_return && C.all_safe;
}

// ============================================================================
// TU collection and lookups
// ============================================================================

void SliceDetector::collectTU(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    for (Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
        auto *FD = dyn_cast<FunctionDecl>(D);
        if (!FD || SM.isInSystemHeader(FD->getLocation()))
            continue;
        if (FD->isThisDeclarationADefinition() && FD->hasBody()) {
            tu_defs.push_back(FD);
            def_by_canon[FD->getCanonicalDecl()] = FD;
        }
    }
}

const PtrIndexFunctionRecord *
SliceDetector::recordFor(const FunctionDecl *FD, SourceManager &SM) const {
    auto it = Meta.functions.find(FD->getNameAsString());
    if (it == Meta.functions.end())
        return nullptr;
    if (!it->second.file.empty() && it->second.file != fileBasenameOf(FD, SM))
        return nullptr; // same-named function from a different file
    return &it->second;
}

const PtrIndexSliceRecord *
SliceDetector::sliceInfoFor(const FunctionDecl *Callee) const {
    auto it = detected.find(Callee->getCanonicalDecl());
    if (it != detected.end())
        return &it->second;
    // Fall back to a record detected while processing an earlier TU
    // (e.g. the callee's defining TU was processed before this one).
    auto mit = Meta.functions.find(Callee->getNameAsString());
    if (mit != Meta.functions.end() && mit->second.slice.present)
        return &mit->second.slice;
    return nullptr;
}

void SliceDetector::markDetected(const FunctionDecl *Canon,
                                 PtrIndexSliceRecord rec) {
    rec.present = true;
    detected[Canon] = std::move(rec);
    detect_order.push_back(Canon);
}

// ============================================================================
// Sub-phase A: root candidates
// ============================================================================

void SliceDetector::detectRoots(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();

    for (const FunctionDecl *FD : tu_defs) {
        if (FD->getNumParams() == 0)
            continue;
        const FunctionDecl *Canon = FD->getCanonicalDecl();
        if (detected.count(Canon))
            continue;
        const PtrIndexFunctionRecord *fnRec = recordFor(FD, SM);
        if (!fnRec)
            continue;

        for (const PtrIndexPointerRecord &P : fnRec->pointers) {
            if (!P.moved || P.index_var.empty())
                continue;
            // Parity with the pre-split tool: only *local* iterating
            // pointers anchor a root slice. (Param-driven roots are a
            // deliberate follow-up generalization.)
            if (P.param_index >= 0)
                continue;
            // Non-constant dereference offsets disqualify slice bounds.
            if (P.variable_offsets)
                continue;

            // Verify the record against the AST: the index variable must
            // exist as an integer local, and the base must name a pointer
            // parameter.
            const VarDecl *IdxVD = findLocalVarNamed(FD->getBody(), P.index_var);
            if (!IdxVD || !IdxVD->getType()->isIntegerType())
                continue;

            const ParmVarDecl *base_pd = nullptr;
            int base_param_idx = -1;
            for (unsigned i = 0; i < FD->getNumParams(); i++) {
                const ParmVarDecl *Parm = FD->getParamDecl(i);
                if (Parm->getNameAsString() == P.base_text &&
                    Parm->getType()->isPointerType()) {
                    base_pd = Parm;
                    base_param_idx = static_cast<int>(i);
                    break;
                }
            }
            if (!base_pd)
                continue;

            // Find the first comparison involving the index variable and
            // resolve the bound it measures against (mirrors the old
            // first-ComparisonExpr-then-break behavior).
            PtrIndexSliceRecord rec;
            rec.slice_param_name = "arr";
            rec.base_param_index = base_param_idx;
            QualType pt = base_pd->getType()->getPointeeType();
            rec.pointee_type = pt.getUnqualifiedType().getAsString();
            rec.slice_type = makeSliceTypeName(rec.pointee_type);
            rec.lookback = -P.min_offset;
            rec.lookahead = P.max_offset;

            bool found_bound = false;
            for (const BinaryOperator *BO : collectComparisons(FD->getBody())) {
                const Expr *LHS = BO->getLHS();
                const Expr *RHS = BO->getRHS();

                bool lhs_is_idx = bareDeclRef(LHS) == IdxVD;

                // Form (c): `base + idx OP end` — pointer-form equality
                // the pointer pass keeps for (lo, hi) walks.
                bool lhs_is_base_plus_idx = false;
                if (!lhs_is_idx) {
                    if (const auto *Add = dyn_cast<BinaryOperator>(
                            LHS->IgnoreParenImpCasts())) {
                        lhs_is_base_plus_idx =
                            Add->getOpcode() == BO_Add &&
                            bareDeclRef(Add->getLHS()) == base_pd &&
                            bareDeclRef(Add->getRHS()) == IdxVD;
                    }
                }
                if (!lhs_is_idx && !lhs_is_base_plus_idx)
                    continue;

                if (lhs_is_base_plus_idx) {
                    // The other side must be a pointer parameter: the end.
                    if (const auto *PD =
                            dyn_cast_or_null<ParmVarDecl>(bareDeclRef(RHS))) {
                        if (PD->getType()->isPointerType()) {
                            rec.end_param_index =
                                static_cast<int>(PD->getFunctionScopeIndex());
                            found_bound = true;
                        }
                    }
                } else if (const auto *PD = dyn_cast_or_null<ParmVarDecl>(
                               bareDeclRef(RHS))) {
                    // Form (a): `idx OP n` with a non-pointer length param.
                    if (!PD->getType()->isPointerType()) {
                        rec.len_param_index =
                            static_cast<int>(PD->getFunctionScopeIndex());
                        found_bound = true;
                    }
                } else if (const auto *Sub = dyn_cast<BinaryOperator>(
                               RHS->IgnoreParenImpCasts())) {
                    // Form (b): `idx OP (end - base)`.
                    if (Sub->getOpcode() == BO_Sub &&
                        bareDeclRef(Sub->getRHS()) == base_pd) {
                        if (const auto *PD = dyn_cast_or_null<ParmVarDecl>(
                                bareDeclRef(Sub->getLHS()))) {
                            rec.end_param_index =
                                static_cast<int>(PD->getFunctionScopeIndex());
                            found_bound = true;
                        }
                    }
                }
                // Only the pointer's first comparison is considered.
                break;
            }

            if (!found_bound)
                continue;

            const ParmVarDecl *end_pd =
                rec.end_param_index >= 0
                    ? FD->getParamDecl(rec.end_param_index)
                    : nullptr;
            const ParmVarDecl *len_pd =
                rec.len_param_index >= 0
                    ? FD->getParamDecl(rec.len_param_index)
                    : nullptr;

            // Inclusive end: the end pointer itself is dereferenced.
            if (end_pd)
                rec.inclusive_end = paramIsDereferenced(FD->getBody(), end_pd);

            // T*→int return collapse: every return must be NULL-like or
            // `base + idx` over this slice's base.
            if (FD->getReturnType()->isPointerType()) {
                std::set<std::string> base_idx_vars;
                for (const PtrIndexPointerRecord &Q : fnRec->pointers) {
                    if (Q.moved && !Q.index_var.empty() &&
                        Q.base_text == P.base_text)
                        base_idx_vars.insert(Q.index_var);
                }
                rec.return_type_changed =
                    allReturnsAreIndexShaped(FD, base_pd, base_idx_vars);
            }

            // Safety: refuse when a *raw* pointer local (one the index
            // rewrite left untouched) aliases or is bounded by the
            // removed params. After reshaping, the remaining references
            // to those params become arr.ptr/arr.len forms, which is
            // meaningless for a pointer that stays a real pointer.
            {
                std::set<const ValueDecl *> removed;
                removed.insert(base_pd);
                if (end_pd)
                    removed.insert(end_pd);
                if (len_pd)
                    removed.insert(len_pd);

                class AliasFinder : public RecursiveASTVisitor<AliasFinder> {
                  public:
                    const std::set<const ValueDecl *> &Removed;
                    bool unsafe = false;
                    explicit AliasFinder(const std::set<const ValueDecl *> &R)
                        : Removed(R) {}

                    bool VisitVarDecl(VarDecl *VD) {
                        if (!isa<ParmVarDecl>(VD) &&
                            VD->getType()->isPointerType() && VD->hasInit() &&
                            mentionsAnyDecl(VD->getInit(), Removed))
                            unsafe = true;
                        return !unsafe;
                    }

                    bool VisitBinaryOperator(BinaryOperator *BO) {
                        if (BO->isAssignmentOp()) {
                            const auto *LVD = dyn_cast_or_null<VarDecl>(
                                bareDeclRef(BO->getLHS()));
                            if (LVD && !isa<ParmVarDecl>(LVD) &&
                                LVD->getType()->isPointerType() &&
                                mentionsAnyDecl(BO->getRHS(), Removed))
                                unsafe = true;
                        } else if (BO->isComparisonOp()) {
                            bool lhs_raw = mentionsRawPointerLocal(BO->getLHS());
                            bool rhs_raw = mentionsRawPointerLocal(BO->getRHS());
                            bool lhs_rm = mentionsAnyDecl(BO->getLHS(), Removed);
                            bool rhs_rm = mentionsAnyDecl(BO->getRHS(), Removed);
                            if ((lhs_raw && rhs_rm) || (rhs_raw && lhs_rm))
                                unsafe = true;
                        }
                        return !unsafe;
                    }
                };

                AliasFinder finder(removed);
                finder.TraverseStmt(FD->getBody());
                if (finder.unsafe)
                    continue;
            }

            markDetected(Canon, std::move(rec));
            break; // one RustSlice per function
        }
    }
}

// ============================================================================
// Sub-phase B: singleton callees
// ============================================================================

void SliceDetector::detectSingletons(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();

    // Iterate over the roots detected so far (matching the pre-split
    // tool's ordering: singleton detection sees only sub-phase A results).
    std::vector<const FunctionDecl *> roots = detect_order;

    for (const FunctionDecl *CallerCanon : roots) {
        auto def_it = def_by_canon.find(CallerCanon);
        if (def_it == def_by_canon.end())
            continue;
        const FunctionDecl *CallerFD = def_it->second;
        const PtrIndexSliceRecord &caller_rec = detected[CallerCanon];
        const PtrIndexFunctionRecord *fnRec = recordFor(CallerFD, SM);

        // Index variables of the caller, for the argument gate below.
        std::set<std::string> caller_idx_vars;
        if (fnRec) {
            for (const PtrIndexPointerRecord &P : fnRec->pointers)
                if (P.moved && !P.index_var.empty())
                    caller_idx_vars.insert(P.index_var);
        }

        // Collect calls out of the caller whose arguments involve a
        // pointer variable or a synthesized index (the transformed
        // spelling of "a tracked pointer was passed to this callee").
        class CallCollector : public RecursiveASTVisitor<CallCollector> {
          public:
            const std::set<std::string> &IdxVars;
            std::vector<const CallExpr *> calls;
            explicit CallCollector(const std::set<std::string> &I)
                : IdxVars(I) {}

            bool argIsPointerish(const Expr *Arg) {
                class Finder : public RecursiveASTVisitor<Finder> {
                  public:
                    const std::set<std::string> &IdxVars;
                    bool Found = false;
                    explicit Finder(const std::set<std::string> &I)
                        : IdxVars(I) {}
                    bool VisitDeclRefExpr(DeclRefExpr *DRE) {
                        const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
                        if (!VD)
                            return true;
                        if (VD->getType()->isPointerType() ||
                            IdxVars.count(VD->getNameAsString())) {
                            Found = true;
                            return false;
                        }
                        return true;
                    }
                };
                Finder F(IdxVars);
                F.TraverseStmt(const_cast<Expr *>(Arg));
                return F.Found;
            }

            bool VisitCallExpr(CallExpr *CE) {
                for (const Expr *Arg : CE->arguments()) {
                    if (argIsPointerish(Arg)) {
                        calls.push_back(CE);
                        break;
                    }
                }
                return true;
            }
        };

        CallCollector collector(caller_idx_vars);
        collector.TraverseStmt(CallerFD->getBody());

        for (const CallExpr *CE : collector.calls) {
            const FunctionDecl *Callee = CE->getDirectCallee();
            if (!Callee)
                continue;
            const FunctionDecl *CalleeCanon = Callee->getCanonicalDecl();
            if (detected.count(CalleeCanon))
                continue;
            auto callee_def_it = def_by_canon.find(CalleeCanon);
            if (callee_def_it == def_by_canon.end())
                continue; // definition not in this TU
            const FunctionDecl *CalleeFD = callee_def_it->second;

            // Every pointer parameter of the callee must be used *only*
            // as a plain dereference (`*a`, read or write).
            bool all_singleton = true;
            std::vector<int> singleton_indices;
            std::string pointee_type;

            for (unsigned pi = 0; pi < CalleeFD->getNumParams(); pi++) {
                const ParmVarDecl *Parm = CalleeFD->getParamDecl(pi);
                if (!Parm->getType()->isPointerType())
                    continue;

                class UseCollector : public RecursiveASTVisitor<UseCollector> {
                  public:
                    const ParmVarDecl *Target;
                    std::vector<const DeclRefExpr *> uses;
                    explicit UseCollector(const ParmVarDecl *P) : Target(P) {}
                    bool VisitDeclRefExpr(DeclRefExpr *DRE) {
                        if (DRE->getDecl() == Target)
                            uses.push_back(DRE);
                        return true;
                    }
                };
                UseCollector uses(Parm);
                uses.TraverseStmt(CalleeFD->getBody());

                if (uses.uses.empty()) {
                    all_singleton = false;
                    break;
                }
                for (const DeclRefExpr *DRE : uses.uses) {
                    const Stmt *Parent = skipTransparentParents(DRE, Ctx);
                    const auto *UO = dyn_cast_or_null<UnaryOperator>(Parent);
                    if (!UO || UO->getOpcode() != UO_Deref) {
                        all_singleton = false;
                        break;
                    }
                }
                if (!all_singleton)
                    break;

                singleton_indices.push_back(static_cast<int>(pi));
                QualType pt = Parm->getType()->getPointeeType();
                pointee_type = pt.getUnqualifiedType().getAsString();
            }

            if (!all_singleton || singleton_indices.empty())
                continue;

            PtrIndexSliceRecord callee_rec;
            callee_rec.slice_type = makeSliceTypeName(pointee_type);
            callee_rec.pointee_type = pointee_type;
            callee_rec.slice_param_name = caller_rec.slice_param_name;
            callee_rec.singleton_param_indices = singleton_indices;
            markDetected(CalleeCanon, std::move(callee_rec));
        }
    }
}

// ============================================================================
// Sub-phase C: pointer-pair propagation (fixpoint)
// ============================================================================

void SliceDetector::detectPointerPairs(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();

    // Does `E` mention `PD` anywhere in its subtree?
    auto exprReferencesParam = [](const Expr *E, const ParmVarDecl *PD) {
        return mentionsDecl(E, PD);
    };

    bool changed = true;
    while (changed) {
        changed = false;

        for (const FunctionDecl *FD : tu_defs) {
            const FunctionDecl *Canon = FD->getCanonicalDecl();
            if (detected.count(Canon))
                continue;

            // Collect all pointer params, with pointee type strings.
            std::vector<std::pair<int, std::string>> ptr_params;
            for (unsigned i = 0; i < FD->getNumParams(); i++) {
                const ParmVarDecl *Parm = FD->getParamDecl(i);
                if (Parm->getType()->isPointerType()) {
                    QualType pt = Parm->getType()->getPointeeType();
                    ptr_params.push_back(
                        {(int)i, pt.getUnqualifiedType().getAsString()});
                }
            }
            if (ptr_params.size() < 2)
                continue;

            // Collect calls to detected callees and recursive self-calls.
            class TransformedCallFinder
                : public RecursiveASTVisitor<TransformedCallFinder> {
              public:
                const SliceDetector *Self;
                const FunctionDecl *SelfCanon;
                std::vector<const CallExpr *> calls;
                std::vector<const CallExpr *> self_calls;
                TransformedCallFinder(const SliceDetector *S,
                                      const FunctionDecl *C)
                    : Self(S), SelfCanon(C) {}
                bool VisitCallExpr(CallExpr *CE) {
                    if (const FunctionDecl *Callee = CE->getDirectCallee()) {
                        if (Callee->getCanonicalDecl() == SelfCanon)
                            self_calls.push_back(CE);
                        else if (Self->sliceInfoFor(Callee))
                            calls.push_back(CE);
                    }
                    return true;
                }
            };
            TransformedCallFinder callFinder(this, Canon);
            callFinder.TraverseStmt(FD->getBody());

            int first_ptr_param = -1;
            int second_ptr_param = -1;
            std::string pointee_type;
            bool valid_pair_found = false;

            // Both params of a same-type pair must be passed as the
            // base/end arguments of one call to a detected callee.
            auto checkPairInCall = [&](const CallExpr *CE, int base_idx,
                                       int end_idx) {
                if (base_idx < 0 || end_idx < 0)
                    return;
                if ((unsigned)base_idx >= CE->getNumArgs() ||
                    (unsigned)end_idx >= CE->getNumArgs())
                    return;

                const Expr *BaseArg = CE->getArg(base_idx);
                const Expr *EndArg = CE->getArg(end_idx);

                for (size_t a = 0; a < ptr_params.size() && !valid_pair_found;
                     a++) {
                    for (size_t b = a + 1;
                         b < ptr_params.size() && !valid_pair_found; b++) {
                        if (ptr_params[a].second != ptr_params[b].second)
                            continue;
                        const ParmVarDecl *PA =
                            FD->getParamDecl(ptr_params[a].first);
                        const ParmVarDecl *PB =
                            FD->getParamDecl(ptr_params[b].first);

                        if (exprReferencesParam(BaseArg, PA) &&
                            exprReferencesParam(EndArg, PB) &&
                            !exprReferencesParam(BaseArg, PB) &&
                            !exprReferencesParam(EndArg, PA)) {
                            first_ptr_param = ptr_params[a].first;
                            second_ptr_param = ptr_params[b].first;
                            pointee_type = ptr_params[a].second;
                            valid_pair_found = true;
                        } else if (exprReferencesParam(BaseArg, PB) &&
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

            for (const CallExpr *CE : callFinder.calls) {
                const PtrIndexSliceRecord *callee_info =
                    sliceInfoFor(CE->getDirectCallee());
                if (!callee_info)
                    continue;
                checkPairInCall(CE, callee_info->base_param_index,
                                callee_info->end_param_index);
                if (valid_pair_found)
                    break;
            }

            // Recursive self-calls: params are often passed indirectly
            // via derived locals (e.g. quick_sort(lo, p - 1)). Heuristic:
            // pointer subtraction between two same-type params (hi - lo)
            // indicates they define a range.
            if (!valid_pair_found && !callFinder.self_calls.empty()) {
                class ParamSubFinder
                    : public RecursiveASTVisitor<ParamSubFinder> {
                  public:
                    const FunctionDecl *FD;
                    std::set<std::pair<int, int>> sub_pairs; // (lhs, rhs)
                    explicit ParamSubFinder(const FunctionDecl *F) : FD(F) {}
                    bool VisitBinaryOperator(BinaryOperator *BO) {
                        if (BO->getOpcode() != BO_Sub)
                            return true;
                        const auto *LP = dyn_cast_or_null<ParmVarDecl>(
                            bareDeclRef(BO->getLHS()));
                        const auto *RP = dyn_cast_or_null<ParmVarDecl>(
                            bareDeclRef(BO->getRHS()));
                        if (!LP || !RP)
                            return true;
                        int li = -1, ri = -1;
                        for (unsigned i = 0; i < FD->getNumParams(); i++) {
                            if (FD->getParamDecl(i) == LP)
                                li = i;
                            if (FD->getParamDecl(i) == RP)
                                ri = i;
                        }
                        if (li >= 0 && ri >= 0)
                            sub_pairs.insert({li, ri});
                        return true;
                    }
                };
                ParamSubFinder subFinder(FD);
                subFinder.TraverseStmt(FD->getBody());

                for (const auto &[li, ri] : subFinder.sub_pairs) {
                    if (valid_pair_found)
                        break;
                    bool li_found = false, ri_found = false;
                    std::string li_type, ri_type;
                    for (const auto &[idx, tp] : ptr_params) {
                        if (idx == li) {
                            li_found = true;
                            li_type = tp;
                        }
                        if (idx == ri) {
                            ri_found = true;
                            ri_type = tp;
                        }
                    }
                    if (!li_found || !ri_found || li_type != ri_type)
                        continue;
                    // end - base pattern: LHS is the end, RHS the base.
                    first_ptr_param = ri;
                    second_ptr_param = li;
                    pointee_type = li_type;
                    valid_pair_found = true;
                }
            }

            if (!valid_pair_found)
                continue;

            const ParmVarDecl *BaseParam = FD->getParamDecl(first_ptr_param);
            const ParmVarDecl *EndParam = FD->getParamDecl(second_ptr_param);

            // Inclusive end: the end param is dereferenced, or passed to
            // an inclusive-end callee, or compared strictly against the
            // base (lo > hi keeps iterating through hi).
            bool inclusive_end = paramIsDereferenced(FD->getBody(), EndParam);

            if (!inclusive_end) {
                class EndArgFinder : public RecursiveASTVisitor<EndArgFinder> {
                  public:
                    const SliceDetector *Self;
                    const ParmVarDecl *EndParam;
                    bool found = false;
                    EndArgFinder(const SliceDetector *S, const ParmVarDecl *E)
                        : Self(S), EndParam(E) {}
                    bool VisitCallExpr(CallExpr *CE) {
                        const FunctionDecl *Callee = CE->getDirectCallee();
                        if (!Callee)
                            return true;
                        const PtrIndexSliceRecord *info =
                            Self->sliceInfoFor(Callee);
                        if (!info || !info->inclusive_end)
                            return true;
                        for (const Expr *Arg : CE->arguments()) {
                            if (bareDeclRef(Arg) == EndParam) {
                                found = true;
                                return false;
                            }
                        }
                        return true;
                    }
                };
                EndArgFinder endArg(this, EndParam);
                endArg.TraverseStmt(FD->getBody());
                inclusive_end = endArg.found;
            }

            if (!inclusive_end) {
                class ComparisonChecker
                    : public RecursiveASTVisitor<ComparisonChecker> {
                  public:
                    const ParmVarDecl *Base, *End;
                    bool found_inclusive = false;
                    ComparisonChecker(const ParmVarDecl *B, const ParmVarDecl *E)
                        : Base(B), End(E) {}
                    bool VisitBinaryOperator(BinaryOperator *BO) {
                        if (!BO->isComparisonOp())
                            return true;
                        const ValueDecl *L = bareDeclRef(BO->getLHS());
                        const ValueDecl *R = bareDeclRef(BO->getRHS());
                        if ((L == Base && R == End &&
                             BO->getOpcode() == BO_GT) ||
                            (L == End && R == Base && BO->getOpcode() == BO_LT))
                            found_inclusive = true;
                        return true;
                    }
                };
                ComparisonChecker checker(BaseParam, EndParam);
                checker.TraverseStmt(FD->getBody());
                inclusive_end = checker.found_inclusive;

                // Source-text fallback for spellings the AST walk missed.
                if (!inclusive_end) {
                    SourceLocation BS = FD->getBody()->getBeginLoc();
                    SourceLocation BE = FD->getBody()->getEndLoc();
                    unsigned bsOff = SM.getFileOffset(BS);
                    unsigned beOff = SM.getFileOffset(BE);
                    StringRef bodyText = SM.getBufferData(SM.getFileID(BS))
                                             .substr(bsOff, beOff - bsOff);
                    std::string bname = BaseParam->getNameAsString();
                    std::string ename = EndParam->getNameAsString();
                    if (bodyText.find(bname + " > " + ename) !=
                            StringRef::npos ||
                        bodyText.find(ename + " < " + bname) !=
                            StringRef::npos)
                        inclusive_end = true;
                }
            }

            // T*→int return collapse, same rule as roots: NULL-like or
            // `base + idx` returns only.
            bool return_changed = false;
            if (FD->getReturnType()->isPointerType()) {
                std::set<std::string> base_idx_vars;
                if (const PtrIndexFunctionRecord *fnRec = recordFor(FD, SM)) {
                    for (const PtrIndexPointerRecord &Q : fnRec->pointers) {
                        if (Q.moved && !Q.index_var.empty() &&
                            Q.base_text == BaseParam->getNameAsString())
                            base_idx_vars.insert(Q.index_var);
                    }
                }
                return_changed =
                    allReturnsAreIndexShaped(FD, BaseParam, base_idx_vars);
            }

            PtrIndexSliceRecord rec;
            rec.slice_type = makeSliceTypeName(pointee_type);
            rec.pointee_type = pointee_type;
            rec.slice_param_name = "arr";
            rec.base_param_index = first_ptr_param;
            rec.end_param_index = second_ptr_param;
            rec.inclusive_end = inclusive_end;
            rec.return_type_changed = return_changed;
            markDetected(Canon, std::move(rec));
            changed = true;
        }
    }
}

// ============================================================================
// Sub-phase D: global-return functions
// ============================================================================

void SliceDetector::detectGlobalReturns(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();

    for (const FunctionDecl *FD : tu_defs) {
        const FunctionDecl *Canon = FD->getCanonicalDecl();
        if (detected.count(Canon) || global_returns.count(Canon))
            continue;
        if (!FD->getReturnType()->isPointerType())
            continue;

        // A function whose every return is NULL or `&global_array[i]`
        // (always the same global array) has its return type rewritten
        // from T* to int; callers index the array directly.
        class GlobalReturnDetector
            : public RecursiveASTVisitor<GlobalReturnDetector> {
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
                if (isNullLike(RetVal))
                    return true;

                if (const auto *UO = dyn_cast<UnaryOperator>(RetVal)) {
                    if (UO->getOpcode() == UO_AddrOf) {
                        const Expr *Sub = UO->getSubExpr()->IgnoreImpCasts();
                        if (const auto *ASE =
                                dyn_cast<ArraySubscriptExpr>(Sub)) {
                            if (const auto *VD = dyn_cast_or_null<VarDecl>(
                                    bareDeclRef(ASE->getBase()))) {
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

                all_returns_valid = false;
                return true;
            }
        };

        GlobalReturnDetector detector;
        detector.TraverseStmt(FD->getBody());

        if (detector.found_global_return && detector.all_returns_valid) {
            PtrIndexGlobalReturnRecord rec;
            rec.file = fileBasenameOf(FD, SM);
            rec.global_array_name = detector.global_array_name;
            QualType pt = FD->getReturnType()->getPointeeType();
            rec.pointee_type = pt.getUnqualifiedType().getAsString();
            global_returns[Canon] = std::move(rec);
        }
    }
}

// ============================================================================
// Export into the shared metadata
// ============================================================================

void SliceDetector::exportResults(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();

    for (const FunctionDecl *Canon : detect_order) {
        auto def_it = def_by_canon.find(Canon);
        if (def_it == def_by_canon.end())
            continue;
        const FunctionDecl *FD = def_it->second;
        std::string name = FD->getNameAsString();
        std::string file = fileBasenameOf(FD, SM);

        auto it = Meta.functions.find(name);
        if (it == Meta.functions.end()) {
            PtrIndexFunctionRecord fnRec;
            fnRec.file = file;
            it = Meta.functions.emplace(name, std::move(fnRec)).first;
        } else if (it->second.file != file) {
            continue; // same-named function from a different file
        }
        // A record from an earlier TU (e.g. an inline function in a
        // shared, already-rewritten header) wins.
        if (it->second.slice.present)
            continue;
        it->second.slice = detected[Canon];
    }

    for (const auto &[Canon, rec] : global_returns) {
        auto def_it = def_by_canon.find(Canon);
        if (def_it == def_by_canon.end())
            continue;
        std::string name = def_it->second->getNameAsString();
        if (Meta.global_return_functions.count(name))
            continue;
        Meta.global_return_functions.emplace(name, rec);
    }
}

// ============================================================================
// Driver
// ============================================================================

void SliceDetector::run(ASTContext &Ctx) {
    collectTU(Ctx);
    detectRoots(Ctx);
    detectSingletons(Ctx);
    detectPointerPairs(Ctx);
    detectGlobalReturns(Ctx);
    exportResults(Ctx);
}

} // namespace xj
