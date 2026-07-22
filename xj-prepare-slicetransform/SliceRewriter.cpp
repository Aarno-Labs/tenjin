// SliceRewriter.cpp — see SliceRewriter.h for the phase overview.
//
// The rewrite texts produced here intentionally mirror what the
// pre-split xj-prepare-pointertransform emitted, so that the two-pass
// chain reproduces the single tool's output byte-for-byte (guarded by
// the golden fixtures under tests/{pointer,slice}_transform_cases/).

#include "SliceRewriter.h"

#include "clang/AST/ParentMapContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include <algorithm>

using namespace clang;

namespace xj {

// ============================================================================
// Small AST/source helpers (mirrors of the pointer tool's Common.cpp)
// ============================================================================

static std::string getSourceText(SourceRange Range, const SourceManager &SM,
                                 const LangOptions &LO) {
    CharSourceRange CSR = CharSourceRange::getTokenRange(Range);
    return Lexer::getSourceText(CSR, SM, LO).str();
}

static std::string getSourceText(const Expr *E, const SourceManager &SM,
                                 const LangOptions &LO) {
    return getSourceText(E->getSourceRange(), SM, LO);
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

static const DeclStmt *findDeclStmtForVar(const VarDecl *VD, Stmt *FunctionBody) {
    if (!VD || !FunctionBody)
        return nullptr;
    class DeclStmtFinder : public RecursiveASTVisitor<DeclStmtFinder> {
      public:
        const VarDecl *Target;
        const DeclStmt *Found = nullptr;
        explicit DeclStmtFinder(const VarDecl *V) : Target(V) {}
        bool VisitDeclStmt(DeclStmt *DS) {
            for (const Decl *D : DS->decls()) {
                if (D == Target) {
                    Found = DS;
                    return false;
                }
            }
            return true;
        }
    };
    DeclStmtFinder finder(VD);
    finder.TraverseStmt(FunctionBody);
    return finder.Found;
}

// Does `E`'s subtree reference a variable named `name`?
static bool mentionsName(const Expr *E, const std::set<std::string> &names,
                         std::string *which = nullptr) {
    if (!E)
        return false;
    class NameFinder : public RecursiveASTVisitor<NameFinder> {
      public:
        const std::set<std::string> &Names;
        std::string Found;
        explicit NameFinder(const std::set<std::string> &N) : Names(N) {}
        bool VisitDeclRefExpr(DeclRefExpr *DRE) {
            if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                if (Names.count(VD->getNameAsString())) {
                    Found = VD->getNameAsString();
                    return false;
                }
            }
            return true;
        }
    };
    NameFinder finder(names);
    finder.TraverseStmt(const_cast<Expr *>(E));
    if (!finder.Found.empty() && which)
        *which = finder.Found;
    return !finder.Found.empty();
}

static bool isNullLikeExpr(const Expr *E) {
    E = E->IgnoreParenImpCasts();
    if (const auto *IL = dyn_cast<IntegerLiteral>(E))
        return IL->getValue() == 0;
    if (isa<GNUNullExpr>(E))
        return true;
    if (const auto *CE = dyn_cast<CStyleCastExpr>(E))
        return isNullLikeExpr(CE->getSubExpr());
    return false;
}

// Replacing a pointer return type's token range does not necessarily leave
// a separator before the function name (`T *func()` reports `T *` as the
// range with `func` immediately after). Preserve existing whitespace when
// there is some, and supply it when the ranges abut.
static void rewriteReturnTypeAsInt(Rewriter &Rewrite, const FunctionDecl *FD,
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

static bool inRanges(const std::vector<std::pair<unsigned, unsigned>> &ranges,
                     unsigned offset) {
    for (const auto &r : ranges)
        if (offset >= r.first && offset < r.second)
            return true;
    return false;
}

// The spelling of a comparison operator with the index conceptually on
// the LHS (the pointer pass normalized its comparison rewrites this way).
static const char *comparisonOpText(BinaryOperatorKind Op) {
    switch (Op) {
    case BO_LT: return "<";
    case BO_GT: return ">";
    case BO_LE: return "<=";
    case BO_GE: return ">=";
    case BO_EQ: return "==";
    case BO_NE: return "!=";
    default: return "??";
    }
}

// ============================================================================
// TU collection and metadata verification
// ============================================================================

void SliceRewriter::collectTU(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    for (Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
        if (auto *FD = dyn_cast<FunctionDecl>(D)) {
            if (SM.isInSystemHeader(FD->getLocation()))
                continue;
            if (FD->hasBody() && FD->isThisDeclarationADefinition())
                tu_functions.push_back(FD);
        } else if (auto *TD = dyn_cast<TypedefNameDecl>(D)) {
            existing_typedefs.insert(TD->getNameAsString());
        }
    }
}

void SliceRewriter::verifyTargets(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();

    // Name -> canonical FunctionDecl for every non-system function in the TU.
    std::map<std::string, const FunctionDecl *> by_name;
    for (Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
        auto *FD = dyn_cast<FunctionDecl>(D);
        if (!FD || SM.isInSystemHeader(FD->getLocation()))
            continue;
        by_name.emplace(FD->getNameAsString(), FD->getCanonicalDecl());
    }

    auto fileBasename = [&](const FunctionDecl *FD) -> std::string {
        if (auto FE = SM.getFileEntryRefForID(
                SM.getFileID(SM.getSpellingLoc(FD->getLocation())))) {
            StringRef name = FE->getName();
            size_t slash = name.rfind('/');
            return (slash == StringRef::npos ? name : name.substr(slash + 1)).str();
        }
        return "";
    };

    for (const auto &[name, rec] : Meta.functions) {
        if (!rec.slice.present)
            continue;
        auto it = by_name.find(name);
        if (it == by_name.end())
            continue;
        const FunctionDecl *Canon = it->second;
        const PtrIndexSliceRecord &S = rec.slice;

        const FunctionDecl *Def = nullptr;
        for (const FunctionDecl *Redecl : Canon->redecls()) {
            if (Redecl->isThisDeclarationADefinition() && Redecl->hasBody()) {
                Def = Redecl;
                break;
            }
        }

        // Structural verification against the definition (when present):
        // the recorded parameter shape must still be there. A mismatch
        // usually means the file was already reshaped (e.g. an inline
        // function in a header rewritten while processing an earlier TU).
        bool body_ok = false;
        if (Def) {
            if (!rec.file.empty() && fileBasename(Def) != rec.file)
                continue; // same-named function from a different file
            unsigned n = Def->getNumParams();
            body_ok = true;
            auto paramOk = [&](int idx, bool want_pointer) {
                if (idx < 0)
                    return true;
                if ((unsigned)idx >= n)
                    return false;
                return Def->getParamDecl(idx)->getType()->isPointerType() ==
                       want_pointer;
            };
            body_ok &= S.base_param_index >= 0 || !S.singleton_param_indices.empty();
            body_ok &= paramOk(S.base_param_index, true);
            body_ok &= paramOk(S.end_param_index, true);
            body_ok &= paramOk(S.len_param_index, false);
            for (int si : S.singleton_param_indices)
                body_ok &= paramOk(si, true);
            if (S.base_param_index >= 0 && body_ok) {
                QualType pt = Def->getParamDecl(S.base_param_index)
                                  ->getType()
                                  ->getPointeeType();
                body_ok &= pt.getUnqualifiedType().getAsString() == S.pointee_type;
            }
        }

        SliceTarget T;
        T.FnRec = &rec;
        T.S = &S;
        T.Canon = Canon;
        T.Def = body_ok ? Def : nullptr;
        T.body_applicable = body_ok;
        slice_targets.emplace(Canon, std::move(T));
    }

    for (const auto &[name, gr] : Meta.global_return_functions) {
        auto it = by_name.find(name);
        if (it == by_name.end())
            continue;
        global_return_fns.emplace(name, it->second);
    }
}

const SliceRewriter::SliceTarget *
SliceRewriter::targetFor(const FunctionDecl *FD) const {
    if (!FD)
        return nullptr;
    auto it = slice_targets.find(FD->getCanonicalDecl());
    return it == slice_targets.end() ? nullptr : &it->second;
}

const FunctionDecl *
SliceRewriter::enclosingFunction(SourceLocation Loc, SourceManager &SM) const {
    for (const FunctionDecl *FD : tu_functions) {
        SourceLocation bodyStart = FD->getBody()->getBeginLoc();
        SourceLocation bodyEnd = FD->getBody()->getEndLoc();
        if (SM.isBeforeInTranslationUnit(bodyStart, Loc) &&
            SM.isBeforeInTranslationUnit(Loc, bodyEnd))
            return FD;
    }
    return nullptr;
}

// ============================================================================
// Signature rewriting (shared by singleton and slice bodies)
// ============================================================================

void SliceRewriter::rewriteSignature(const SliceTarget &T, ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();
    const FunctionDecl *FD = T.Def;
    const PtrIndexSliceRecord &S = *T.S;

    std::string new_params = S.slice_type + " " + S.slice_param_name;
    if (!S.singleton_param_indices.empty()) {
        for (unsigned i = 0; i < FD->getNumParams(); i++) {
            const ParmVarDecl *P = FD->getParamDecl(i);
            bool is_singleton =
                std::find(S.singleton_param_indices.begin(),
                          S.singleton_param_indices.end(),
                          (int)i) != S.singleton_param_indices.end();
            if (is_singleton)
                new_params += ", int " + P->getNameAsString();
            else
                new_params +=
                    ", " + P->getType().getAsString() + " " + P->getNameAsString();
        }
    } else {
        for (unsigned i = 0; i < FD->getNumParams(); i++) {
            if ((int)i == S.base_param_index) continue;
            if ((int)i == S.end_param_index) continue;
            if ((int)i == S.len_param_index) continue;
            const ParmVarDecl *P = FD->getParamDecl(i);
            new_params +=
                ", " + P->getType().getAsString() + " " + P->getNameAsString();
        }
    }

    SourceLocation FirstParamLoc = FD->getParamDecl(0)->getBeginLoc();
    unsigned LastIdx = FD->getNumParams() - 1;
    SourceLocation LastParamEnd = Lexer::getLocForEndOfToken(
        FD->getParamDecl(LastIdx)->getEndLoc(), 0, SM, LO);
    unsigned startOff = SM.getFileOffset(FirstParamLoc);
    unsigned endOff = SM.getFileOffset(LastParamEnd);
    TheRewriter.ReplaceText(FirstParamLoc, endOff - startOff, new_params);

    if (S.return_type_changed) {
        // Note: this eats any leading storage-class specifier (`static
        // int *f` becomes `int f`) — matching the pre-split tool.
        SourceLocation RetStart = FD->getBeginLoc();
        SourceLocation FuncNameLoc = FD->getLocation();
        unsigned retLen = SM.getFileOffset(FuncNameLoc) - SM.getFileOffset(RetStart);
        TheRewriter.ReplaceText(RetStart, retLen, "int ");
    }
}

// ============================================================================
// Singleton (swap-style) bodies
// ============================================================================

void SliceRewriter::applySingletonBody(const SliceTarget &T, ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();
    const FunctionDecl *FD = T.Def;
    const PtrIndexSliceRecord &S = *T.S;

    std::set<const ParmVarDecl *> singleton_params;
    for (int pi : S.singleton_param_indices)
        singleton_params.insert(FD->getParamDecl(pi));

    // Every `*a` (read or write) becomes `arr.ptr[a]`.
    class DerefRewriter : public RecursiveASTVisitor<DerefRewriter> {
      public:
        Rewriter &Rewrite;
        SourceManager &SM;
        const LangOptions &LO;
        const std::set<const ParmVarDecl *> &Params;
        const std::string &ArrName;

        DerefRewriter(Rewriter &R, SourceManager &SM, const LangOptions &LO,
                      const std::set<const ParmVarDecl *> &P, const std::string &A)
            : Rewrite(R), SM(SM), LO(LO), Params(P), ArrName(A) {}

        bool VisitUnaryOperator(UnaryOperator *UO) {
            if (UO->getOpcode() != UO_Deref)
                return true;
            const auto *DRE =
                dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
            if (!DRE)
                return true;
            const auto *PD = dyn_cast<ParmVarDecl>(DRE->getDecl());
            if (!PD || !Params.count(PD))
                return true;
            SourceLocation StartLoc = UO->getBeginLoc();
            SourceLocation EndLoc =
                Lexer::getLocForEndOfToken(UO->getEndLoc(), 0, SM, LO);
            Rewrite.ReplaceText(CharSourceRange::getCharRange(StartLoc, EndLoc),
                                ArrName + ".ptr[" + PD->getNameAsString() + "]");
            return true;
        }
    };

    DerefRewriter rewriter(TheRewriter, SM, LO, singleton_params,
                           S.slice_param_name);
    rewriter.TraverseStmt(FD->getBody());

    rewriteSignature(T, Ctx);
}

// ============================================================================
// Slice-function bodies (root (ptr,len), root (lo,hi), pointer-pair)
// ============================================================================

void SliceRewriter::applySliceBody(const SliceTarget &T, ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();
    const FunctionDecl *FD = T.Def;
    const PtrIndexSliceRecord &S = *T.S;
    const std::string slice = S.slice_param_name;

    const ParmVarDecl *base_pd =
        S.base_param_index >= 0 ? FD->getParamDecl(S.base_param_index) : nullptr;
    const ParmVarDecl *end_pd =
        S.end_param_index >= 0 ? FD->getParamDecl(S.end_param_index) : nullptr;
    const ParmVarDecl *len_pd =
        S.len_param_index >= 0 ? FD->getParamDecl(S.len_param_index) : nullptr;
    if (!base_pd)
        return;
    std::string base_name = base_pd->getNameAsString();

    // Default replacement forms, exactly as the pre-split tool's
    // replaceRemovedParams computed them.
    std::string baseRepl = slice + ".ptr";
    if (S.lookback > 0)
        baseRepl = "(" + slice + ".ptr + " + std::to_string(S.lookback) + ")";
    std::string endRepl;
    if (end_pd) {
        endRepl = S.inclusive_end ? "(" + slice + ".len - 1)" : slice + ".len";
        if (S.lookahead > 0)
            endRepl = "(" + slice + ".len - " + std::to_string(S.lookahead) + ")";
    }
    std::string lenRepl = slice + ".len";
    if (S.lookback > 0 || S.lookahead > 0)
        lenRepl = "(" + slice + ".len - " +
                  std::to_string(S.lookback + S.lookahead) + ")";

    // Moved-pointer facts from the metadata: which index variables exist,
    // and which of them iterate over the removed base param.
    struct RsPtr {
        const PtrIndexPointerRecord *rec;
        bool matched_comparison = false;
    };
    std::map<std::string, RsPtr> rs_ptrs; // by index_var
    std::set<std::string> all_index_vars;
    std::set<std::string> rs_index_vars;
    for (const auto &P : T.FnRec->pointers) {
        if (!P.moved || P.index_var.empty())
            continue;
        all_index_vars.insert(P.index_var);
        if (P.base_text == base_name) {
            rs_ptrs[P.index_var] = RsPtr{&P, false};
            rs_index_vars.insert(P.index_var);
        }
    }

    std::vector<std::pair<unsigned, unsigned>> edited;
    auto markEdited = [&](SourceLocation Start, SourceLocation End) {
        edited.push_back({SM.getFileOffset(Start), SM.getFileOffset(End)});
    };
    auto replaceRange = [&](SourceLocation Start, SourceLocation End,
                            const std::string &text) {
        TheRewriter.ReplaceText(CharSourceRange::getCharRange(Start, End), text);
        markEdited(Start, End);
    };
    auto tokEnd = [&](SourceLocation Loc) {
        return Lexer::getLocForEndOfToken(Loc, 0, SM, LO);
    };

    auto isParamDRE = [&](const Expr *E, const ParmVarDecl *PD) -> bool {
        if (!PD)
            return false;
        const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
        return DRE && DRE->getDecl() == PD;
    };
    auto indexVarName = [&](const Expr *E) -> std::string {
        const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
        if (!DRE)
            return "";
        const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
        if (!VD)
            return "";
        std::string name = VD->getNameAsString();
        return all_index_vars.count(name) ? name : "";
    };

    // The bound adjustment the pointer pass would have applied when it
    // rewrote this pointer's comparison against the slice's extent.
    auto boundText = [&](const RsPtr &rp) -> std::string {
        long adj = rp.rec->max_offset;
        if (S.inclusive_end)
            adj += 1;
        if (adj > 0)
            return slice + ".len - " + std::to_string(adj);
        return slice + ".len";
    };

    // ---- Pass 1: comparisons -----------------------------------------
    // Collected first, then rewritten, so the generic param-reference
    // pass below doesn't tear apart expressions these consume whole.
    class ComparisonCollector : public RecursiveASTVisitor<ComparisonCollector> {
      public:
        std::vector<BinaryOperator *> comparisons;
        bool VisitBinaryOperator(BinaryOperator *BO) {
            if (BO->isComparisonOp())
                comparisons.push_back(BO);
            return true;
        }
    };
    ComparisonCollector cmpCollector;
    cmpCollector.TraverseStmt(FD->getBody());

    for (BinaryOperator *BO : cmpCollector.comparisons) {
        unsigned bo_off = SM.getFileOffset(BO->getBeginLoc());
        if (inRanges(edited, bo_off))
            continue;

        const Expr *LHS = BO->getLHS();
        const Expr *RHS = BO->getRHS();

        // (a) `idx OP n` — the pointer pass's index-vs-len comparison.
        //     The len operand becomes the adjusted slice extent.
        std::string idx = indexVarName(LHS);
        if (!idx.empty() && rs_ptrs.count(idx) && isParamDRE(RHS, len_pd)) {
            replaceRange(RHS->IgnoreParenImpCasts()->getBeginLoc(),
                         tokEnd(RHS->IgnoreParenImpCasts()->getEndLoc()),
                         boundText(rs_ptrs[idx]));
            rs_ptrs[idx].matched_comparison = true;
            continue;
        }

        // (b) `idx OP (end - base)` — the (lo,hi) index comparison.
        if (!idx.empty() && rs_ptrs.count(idx)) {
            const Expr *R = RHS->IgnoreImpCasts();
            const auto *PE = dyn_cast<ParenExpr>(R);
            const auto *Sub = PE ? dyn_cast<BinaryOperator>(
                                       PE->getSubExpr()->IgnoreImpCasts())
                                 : nullptr;
            if (Sub && Sub->getOpcode() == BO_Sub &&
                isParamDRE(Sub->getLHS(), end_pd) &&
                isParamDRE(Sub->getRHS(), base_pd)) {
                replaceRange(R->getBeginLoc(), tokEnd(R->getEndLoc()),
                             boundText(rs_ptrs[idx]));
                rs_ptrs[idx].matched_comparison = true;
                continue;
            }
        }

        // (c) `base + idx OP (end)` — pointer-form equality the pointer
        //     pass keeps when comparing against another pointer. When the
        //     other side is the removed end param, it becomes an index
        //     bound (the whole comparison is replaced, as before).
        if (const auto *Add =
                dyn_cast<BinaryOperator>(LHS->IgnoreParenImpCasts())) {
            if (Add->getOpcode() == BO_Add && isParamDRE(Add->getLHS(), base_pd)) {
                std::string add_idx = indexVarName(Add->getRHS());
                if (!add_idx.empty() && rs_ptrs.count(add_idx) &&
                    isParamDRE(RHS, end_pd)) {
                    std::string text = add_idx + " " +
                                       comparisonOpText(BO->getOpcode()) + " " +
                                       boundText(rs_ptrs[add_idx]);
                    replaceRange(BO->getBeginLoc(), tokEnd(BO->getEndLoc()), text);
                    rs_ptrs[add_idx].matched_comparison = true;
                    continue;
                }
            }
        }

        // (d) `base OP end` (both removed params) — collapses to a
        //     comparison of the slice's length against a threshold.
        if (base_pd && end_pd) {
            bool base_on_left =
                isParamDRE(LHS, base_pd) && isParamDRE(RHS, end_pd);
            bool end_on_left = isParamDRE(LHS, end_pd) && isParamDRE(RHS, base_pd);
            if (base_on_left || end_on_left) {
                std::string threshold = S.inclusive_end ? "1" : "0";
                std::string cmp;
                switch (BO->getOpcode()) {
                case BO_LT: cmp = base_on_left ? "> " + threshold : "< " + threshold; break;
                case BO_LE: cmp = base_on_left ? ">= " + threshold : "<= " + threshold; break;
                case BO_GT: cmp = base_on_left ? "< " + threshold : "> " + threshold; break;
                case BO_GE: cmp = base_on_left ? "<= " + threshold : ">= " + threshold; break;
                case BO_NE: cmp = "!= 0"; break;
                default: cmp = "> " + threshold; break;
                }
                replaceRange(BO->getBeginLoc(), tokEnd(BO->getEndLoc()),
                             slice + ".len " + cmp);
                continue;
            }
        }

        // (e) `(end - base) OP N` — the difference becomes arr.len.
        if (base_pd && end_pd) {
            if (const auto *Sub =
                    dyn_cast<BinaryOperator>(LHS->IgnoreImpCasts())) {
                if (Sub->getOpcode() == BO_Sub &&
                    isParamDRE(Sub->getLHS(), end_pd) &&
                    isParamDRE(Sub->getRHS(), base_pd)) {
                    replaceRange(Sub->getBeginLoc(), tokEnd(Sub->getEndLoc()),
                                 slice + ".len");
                    continue;
                }
            }
        }
    }

    // ---- Pass 2: standalone `end - base` subtractions ----------------
    if (end_pd) {
        class SubCollector : public RecursiveASTVisitor<SubCollector> {
          public:
            std::vector<BinaryOperator *> subs;
            bool VisitBinaryOperator(BinaryOperator *BO) {
                if (BO->getOpcode() == BO_Sub)
                    subs.push_back(BO);
                return true;
            }
        };
        SubCollector subCollector;
        subCollector.TraverseStmt(FD->getBody());
        for (BinaryOperator *BO : subCollector.subs) {
            if (inRanges(edited, SM.getFileOffset(BO->getBeginLoc())))
                continue;
            if (isParamDRE(BO->getLHS(), end_pd) &&
                isParamDRE(BO->getRHS(), base_pd)) {
                replaceRange(BO->getBeginLoc(), tokEnd(BO->getEndLoc()),
                             slice + ".len");
            }
        }
    }

    // ---- Pass 3: `*base` / `*end` dereferences -----------------------
    {
        class DerefCollector : public RecursiveASTVisitor<DerefCollector> {
          public:
            std::vector<UnaryOperator *> derefs;
            bool VisitUnaryOperator(UnaryOperator *UO) {
                if (UO->getOpcode() == UO_Deref)
                    derefs.push_back(UO);
                return true;
            }
        };
        DerefCollector derefCollector;
        derefCollector.TraverseStmt(FD->getBody());
        for (UnaryOperator *UO : derefCollector.derefs) {
            if (inRanges(edited, SM.getFileOffset(UO->getBeginLoc())))
                continue;
            std::string idx;
            if (isParamDRE(UO->getSubExpr(), base_pd))
                idx = "0";
            else if (end_pd && isParamDRE(UO->getSubExpr(), end_pd))
                idx = endRepl;
            else
                continue;
            replaceRange(UO->getBeginLoc(), tokEnd(UO->getEndLoc()),
                         slice + ".ptr[" + idx + "]");
        }
    }

    // ---- Pass 4: locals receiving return-type-changed call results ---
    // `int *m = partition(lo, hi);` -> `int m = partition(lo, hi);`
    // (the call's arguments are reshaped later by the call-site pass).
    // The pre-split tool only did this inside pointer-pair functions.
    if (S.end_param_index >= 0) {
        class RecvVarCollector : public RecursiveASTVisitor<RecvVarCollector> {
          public:
            std::vector<VarDecl *> vars;
            bool VisitVarDecl(VarDecl *VD) {
                if (VD->hasInit() && VD->getType()->isPointerType())
                    vars.push_back(VD);
                return true;
            }
        };
        RecvVarCollector recvCollector;
        recvCollector.TraverseStmt(FD->getBody());
        for (VarDecl *VD : recvCollector.vars) {
            const auto *CE = dyn_cast<CallExpr>(VD->getInit()->IgnoreImpCasts());
            if (!CE)
                continue;
            const SliceTarget *CalleeT =
                targetFor(CE->getDirectCallee()
                              ? CE->getDirectCallee()->getCanonicalDecl()
                              : nullptr);
            if (!CalleeT || !CalleeT->S->return_type_changed)
                continue;
            const DeclStmt *DS = findDeclStmtForVar(VD, FD->getBody());
            if (DS) {
                SourceLocation TypeStart = DS->getBeginLoc();
                SourceLocation NameEnd = tokEnd(VD->getLocation());
                replaceRange(TypeStart, NameEnd, "int " + VD->getNameAsString());
            }
            index_return_vars.insert(VD);
        }
    }

    // ---- Pass 5: returns ---------------------------------------------
    if (S.return_type_changed) {
        class ReturnCollector : public RecursiveASTVisitor<ReturnCollector> {
          public:
            std::vector<ReturnStmt *> returns;
            bool VisitReturnStmt(ReturnStmt *RS) {
                returns.push_back(RS);
                return true;
            }
        };
        ReturnCollector retCollector;
        retCollector.TraverseStmt(FD->getBody());
        for (ReturnStmt *RS : retCollector.returns) {
            const Expr *RetVal = RS->getRetValue();
            if (!RetVal)
                continue;

            // `return base + idx;` -> `return idx;`
            const Expr *Stripped = RetVal->IgnoreParenImpCasts();
            while (const auto *C = dyn_cast<CStyleCastExpr>(Stripped))
                Stripped = C->getSubExpr()->IgnoreParenImpCasts();
            if (const auto *Add = dyn_cast<BinaryOperator>(Stripped)) {
                if (Add->getOpcode() == BO_Add &&
                    isParamDRE(Add->getLHS(), base_pd)) {
                    std::string idx = indexVarName(Add->getRHS());
                    if (!idx.empty()) {
                        replaceRange(RetVal->getBeginLoc(),
                                     tokEnd(RetVal->getEndLoc()), idx);
                        continue;
                    }
                }
            }

            // `return NULL;` (and spelled variants) -> `return -1;`.
            // Macro-expanded spellings (`return NULL` with NULL from a
            // system header) are left to the raw-text fallback below, so
            // no edit ever lands inside the macro's defining header.
            SourceLocation ValStart = RetVal->getBeginLoc();
            SourceLocation ValEnd = tokEnd(RetVal->getEndLoc());
            if (ValStart.isMacroID() || ValEnd.isMacroID())
                continue;
            StringRef valText = Lexer::getSourceText(
                CharSourceRange::getCharRange(ValStart, ValEnd), SM, LO);
            if (valText == "NULL" || valText == "0" || valText == "__null" ||
                valText == "((void*)0)" || valText == "((void *)0)") {
                replaceRange(ValStart, ValEnd, "-1");
            }
        }

        // Raw-text fallback for `return NULL` / `return 0` spellings the
        // AST walk can miss (e.g. NULL undefined due to missing headers).
        SourceLocation BodyStart = FD->getBody()->getBeginLoc();
        SourceLocation BodyEnd = FD->getBody()->getEndLoc();
        unsigned startOff = SM.getFileOffset(BodyStart);
        unsigned endOff = SM.getFileOffset(BodyEnd);
        StringRef bodyText = SM.getBufferData(SM.getFileID(BodyStart))
                                 .substr(startOff, endOff - startOff);
        for (const char *pattern : {"return NULL", "return 0"}) {
            StringRef pat(pattern);
            size_t pos = 0;
            while ((pos = bodyText.find(pat, pos)) != StringRef::npos) {
                size_t afterPos = pos + pat.size();
                if (afterPos < bodyText.size()) {
                    char c = bodyText[afterPos];
                    if (c != ';' && c != ' ' && c != '\n' && c != '\r' &&
                        c != '\t') {
                        pos = afterPos;
                        continue;
                    }
                }
                unsigned valStart = startOff + pos + strlen("return ");
                unsigned valLen = pat.size() - strlen("return ");
                if (!inRanges(edited, valStart)) {
                    SourceLocation valLoc =
                        SM.getLocForStartOfFile(SM.getFileID(BodyStart))
                            .getLocWithOffset(valStart);
                    TheRewriter.ReplaceText(valLoc, valLen, "-1");
                    edited.push_back({valStart, valStart + valLen});
                }
                pos = afterPos;
            }
        }
    }

    // ---- Pass 6: index initializer lookback adjustment ----------------
    // A pointer whose comparison anchored it to the slice extent starts
    // `lookback` positions in when the slice was widened downward.
    for (auto &[idx_name, rp] : rs_ptrs) {
        if (!rp.matched_comparison)
            continue;
        long lb = -rp.rec->min_offset;
        if (lb <= 0)
            continue;
        // Find `int <idx_name> = <init>;` in the body.
        class IndexDeclFinder : public RecursiveASTVisitor<IndexDeclFinder> {
          public:
            const std::string &Name;
            VarDecl *Found = nullptr;
            explicit IndexDeclFinder(const std::string &N) : Name(N) {}
            bool VisitVarDecl(VarDecl *VD) {
                if (VD->getNameAsString() == Name && VD->hasInit()) {
                    Found = VD;
                    return false;
                }
                return true;
            }
        };
        IndexDeclFinder finder(idx_name);
        finder.TraverseStmt(FD->getBody());
        if (!finder.Found)
            continue;
        const Expr *Init = finder.Found->getInit();
        std::string init_text = getSourceText(Init, SM, LO);
        std::string new_init = init_text == "0"
                                   ? std::to_string(lb)
                                   : init_text + " + " + std::to_string(lb);
        replaceRange(Init->getBeginLoc(), tokEnd(Init->getEndLoc()), new_init);
    }

    // ---- Pass 7: remaining references to the removed params ----------
    class ParamRefCollector : public RecursiveASTVisitor<ParamRefCollector> {
      public:
        std::vector<DeclRefExpr *> refs;
        bool VisitDeclRefExpr(DeclRefExpr *DRE) {
            refs.push_back(DRE);
            return true;
        }
    };
    ParamRefCollector refCollector;
    refCollector.TraverseStmt(FD->getBody());

    for (DeclRefExpr *DRE : refCollector.refs) {
        const auto *PD = dyn_cast<ParmVarDecl>(DRE->getDecl());
        if (!PD)
            continue;
        std::string repl;
        bool is_base = PD == base_pd;
        if (is_base)
            repl = baseRepl;
        else if (PD == end_pd)
            repl = endRepl;
        else if (PD == len_pd)
            repl = lenRepl;
        else
            continue;

        unsigned offset = SM.getFileOffset(DRE->getBeginLoc());
        if (inRanges(edited, offset))
            continue;

        if (is_base) {
            // A base reference adjacent to an index variable is part of
            // an index-transformed access (`base[idx]`, `base + idx`);
            // those were emitted against the un-widened base, so they
            // rebase onto arr.ptr directly (no lookback shift) when the
            // index belongs to a slice-anchored pointer.
            const Stmt *Parent = skipTransparentParents(DRE, Ctx);
            std::string idx_name;
            if (Parent) {
                if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Parent)) {
                    if (ASE->getBase()->IgnoreParenImpCasts() == DRE &&
                        mentionsName(ASE->getIdx(), all_index_vars, &idx_name)) {
                        // fallthrough with idx_name set
                    } else {
                        idx_name.clear();
                    }
                } else if (const auto *Add = dyn_cast<BinaryOperator>(Parent)) {
                    if (Add->getOpcode() == BO_Add) {
                        const Expr *Other =
                            Add->getLHS()->IgnoreParenImpCasts() == DRE
                                ? Add->getRHS()
                                : Add->getLHS();
                        mentionsName(Other, all_index_vars, &idx_name);
                    }
                }
            }
            if (!idx_name.empty()) {
                auto rp = rs_ptrs.find(idx_name);
                if (rp != rs_ptrs.end() && rp->second.matched_comparison)
                    repl = slice + ".ptr";
                else
                    repl = S.lookback > 0 ? "(" + slice + ".ptr + " +
                                                std::to_string(S.lookback) + ")"
                                          : slice + ".ptr";
            }
        }

        replaceRange(DRE->getBeginLoc(), tokEnd(DRE->getEndLoc()), repl);
    }

    rewriteSignature(T, Ctx);
}

// ============================================================================
// Typedef emission
// ============================================================================

void SliceRewriter::emitTypedefs(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    FileID MainFID = SM.getMainFileID();

    // Slice types whose function has a prototype in a real header are
    // handled by rewriteForwardDeclarations (the typedef must precede the
    // prototype there).
    std::set<std::string> has_header_decl;
    for (const auto &[Canon, T] : slice_targets) {
        for (const FunctionDecl *Redecl : Canon->redecls()) {
            if (Redecl->isThisDeclarationADefinition())
                continue;
            if (SM.isInSystemHeader(Redecl->getLocation()))
                continue;
            if (SM.getFileID(Redecl->getLocation()) != MainFID) {
                has_header_decl.insert(T.S->slice_type);
                break;
            }
        }
    }

    std::map<std::string, SourceLocation> earliest_loc;
    std::map<std::string, std::string> typedef_text;

    auto consider = [&](const std::string &slice_type, SourceLocation loc) {
        if (loc.isInvalid())
            return;
        if (SM.getFileID(loc) != MainFID)
            return;
        auto it = earliest_loc.find(slice_type);
        if (it == earliest_loc.end() ||
            SM.isBeforeInTranslationUnit(loc, it->second))
            earliest_loc[slice_type] = loc;
    };

    for (const auto &[Canon, T] : slice_targets) {
        if (has_header_decl.count(T.S->slice_type))
            continue;

        if (T.Def)
            consider(T.S->slice_type, T.Def->getBeginLoc());
        for (const FunctionDecl *Redecl : Canon->redecls()) {
            if (Redecl->isThisDeclarationADefinition())
                continue;
            if (SM.isInSystemHeader(Redecl->getLocation()))
                continue;
            consider(T.S->slice_type, Redecl->getBeginLoc());
        }
        if (!T.Def) {
            // Definition lives in another TU; the compound literals at
            // this TU's call sites still need the type. Anchor the
            // typedef at the earliest calling function.
            for (const FunctionDecl *FD : tu_functions) {
                // (cheap scan; call sites are found precisely later)
                consider(T.S->slice_type, FD->getBeginLoc());
            }
        }

        if (!typedef_text.count(T.S->slice_type))
            typedef_text[T.S->slice_type] = "typedef struct { " +
                                            T.S->pointee_type +
                                            " *ptr; size_t len; } " +
                                            T.S->slice_type + ";\n\n";
    }

    for (auto &[type_name, loc] : earliest_loc) {
        if (existing_typedefs.count(type_name))
            continue; // already visible in this TU (e.g. rewritten header)
        if (!emitted_typedefs.insert(type_name).second)
            continue;
        TheRewriter.InsertTextBefore(loc, typedef_text[type_name]);
    }
}

// ============================================================================
// Call-site rewriting
// ============================================================================

std::string SliceRewriter::translateArgExpr(const Expr *ArgExpr,
                                            const SliceTarget &CallerT,
                                            ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();
    const PtrIndexSliceRecord &CS = *CallerT.S;
    const FunctionDecl *CallerFD = CallerT.Def;

    ArgExpr = ArgExpr->IgnoreImpCasts();
    if (const auto *PE = dyn_cast<ParenExpr>(ArgExpr))
        ArgExpr = PE->getSubExpr()->IgnoreImpCasts();

    const ParmVarDecl *base_pd =
        CS.base_param_index >= 0 && CallerFD
            ? CallerFD->getParamDecl(CS.base_param_index)
            : nullptr;
    const ParmVarDecl *end_pd = CS.end_param_index >= 0 && CallerFD
                                    ? CallerFD->getParamDecl(CS.end_param_index)
                                    : nullptr;
    const ParmVarDecl *len_pd = CS.len_param_index >= 0 && CallerFD
                                    ? CallerFD->getParamDecl(CS.len_param_index)
                                    : nullptr;

    if (const auto *DRE = dyn_cast<DeclRefExpr>(ArgExpr)) {
        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
            if (index_return_vars.count(VD))
                return VD->getNameAsString();
            if (const auto *PD = dyn_cast<ParmVarDecl>(VD)) {
                if (PD == base_pd)
                    return CS.slice_param_name + ".ptr";
                if (PD == end_pd)
                    return CS.inclusive_end ? CS.slice_param_name + ".len - 1"
                                            : CS.slice_param_name + ".len";
                if (PD == len_pd)
                    return CS.slice_param_name + ".len";
            }
        }
    }

    if (const auto *BO = dyn_cast<BinaryOperator>(ArgExpr)) {
        if (BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub) {
            // `base + X` collapses to the offset X: the pointer pass
            // spells transformed-pointer args as `base + p_index_xj`,
            // and the slice machinery works in offset space.
            if (BO->getOpcode() == BO_Add && base_pd) {
                const auto *LDRE =
                    dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts());
                if (LDRE && LDRE->getDecl() == base_pd)
                    return getSourceText(BO->getRHS(), SM, LO);
            }
            std::string lhs = translateArgExpr(BO->getLHS(), CallerT, Ctx);
            std::string rhs = getSourceText(BO->getRHS(), SM, LO);
            std::string op = (BO->getOpcode() == BO_Add) ? " + " : " - ";
            return lhs + op + rhs;
        }
    }

    return getSourceText(ArgExpr, SM, LO);
}

void SliceRewriter::rewriteCallSites(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    class CallSiteCollector : public RecursiveASTVisitor<CallSiteCollector> {
      public:
        const SliceRewriter *Self;
        std::vector<CallExpr *> calls;
        explicit CallSiteCollector(const SliceRewriter *S) : Self(S) {}
        bool VisitCallExpr(CallExpr *CE) {
            const FunctionDecl *Callee = CE->getDirectCallee();
            if (Callee && Self->targetFor(Callee->getCanonicalDecl()))
                calls.push_back(CE);
            return true;
        }
    };
    CallSiteCollector collector(this);
    collector.TraverseDecl(Ctx.getTranslationUnitDecl());

    std::sort(collector.calls.begin(), collector.calls.end(),
              [&SM](const CallExpr *A, const CallExpr *B) {
                  return SM.getFileOffset(A->getBeginLoc()) >
                         SM.getFileOffset(B->getBeginLoc());
              });

    for (CallExpr *CE : collector.calls) {
        const SliceTarget *CalleeT =
            targetFor(CE->getDirectCallee()->getCanonicalDecl());
        const PtrIndexSliceRecord &info = *CalleeT->S;
        unsigned NumArgs = CE->getNumArgs();
        if (NumArgs == 0)
            continue;
        // The recorded shape must fit this call (a mismatched arg count
        // means the call site was already reshaped, or never matched).
        if (info.base_param_index >= 0 &&
            (unsigned)info.base_param_index >= NumArgs)
            continue;
        if (info.end_param_index >= 0 && (unsigned)info.end_param_index >= NumArgs)
            continue;
        if (info.len_param_index >= 0 && (unsigned)info.len_param_index >= NumArgs)
            continue;
        bool singleton_ok = true;
        for (int si : info.singleton_param_indices)
            if ((unsigned)si >= NumArgs)
                singleton_ok = false;
        if (!singleton_ok)
            continue;

        const FunctionDecl *EnclosingFD = enclosingFunction(CE->getBeginLoc(), SM);
        const SliceTarget *CallerT =
            EnclosingFD ? targetFor(EnclosingFD->getCanonicalDecl()) : nullptr;
        if (CallerT && !CallerT->body_applicable)
            CallerT = nullptr; // caller wasn't reshaped in this TU

        std::vector<std::string> arg_texts;
        for (unsigned i = 0; i < NumArgs; i++)
            arg_texts.push_back(getSourceText(CE->getArg(i), SM, LO));

        SourceLocation FirstArgLoc = CE->getArg(0)->getBeginLoc();
        SourceLocation LastArgEnd = Lexer::getLocForEndOfToken(
            CE->getArg(NumArgs - 1)->getEndLoc(), 0, SM, LO);
        auto replaceArgs = [&](const std::string &new_args) {
            TheRewriter.ReplaceText(
                CharSourceRange::getCharRange(FirstArgLoc, LastArgEnd), new_args);
        };

        // ---- Singleton callees (swap-style) --------------------------
        if (!info.singleton_param_indices.empty()) {
            if (CallerT) {
                std::string new_args = CallerT->S->slice_param_name;
                for (unsigned i = 0; i < NumArgs; i++)
                    new_args +=
                        ", " + translateArgExpr(CE->getArg(i), *CallerT, Ctx);
                replaceArgs(new_args);
            } else {
                std::string ptr_text = arg_texts[info.singleton_param_indices[0]];
                std::string compound =
                    "(" + info.slice_type + "){" + ptr_text + ", 0}";
                std::string new_args = compound;
                for (unsigned i = 0; i < NumArgs; i++) {
                    bool is_singleton =
                        std::find(info.singleton_param_indices.begin(),
                                  info.singleton_param_indices.end(),
                                  (int)i) != info.singleton_param_indices.end();
                    new_args += is_singleton ? ", 0" : ", " + arg_texts[i];
                }
                replaceArgs(new_args);
            }
            continue;
        }

        if (info.base_param_index < 0)
            continue;

        if (CallerT) {
            // Caller and callee both reshaped.
            const PtrIndexSliceRecord &caller_info = *CallerT->S;
            std::string slice = caller_info.slice_param_name;
            std::string base_arg = arg_texts[info.base_param_index];

            bool is_pass_through = false;
            std::string caller_base_name;
            if (caller_info.base_param_index >= 0) {
                caller_base_name =
                    EnclosingFD->getParamDecl(caller_info.base_param_index)
                        ->getNameAsString();
                if (base_arg == caller_base_name)
                    is_pass_through = true;
            }

            auto buildSubslice = [&](const Expr *BaseArgExpr,
                                     const Expr *EndArgExpr) {
                std::string trans_base =
                    translateArgExpr(BaseArgExpr, *CallerT, Ctx);
                std::string trans_end = translateArgExpr(EndArgExpr, *CallerT, Ctx);

                std::string ptr_expr;
                if (trans_base == slice + ".ptr")
                    ptr_expr = slice + ".ptr";
                else
                    ptr_expr = slice + ".ptr + " + trans_base;

                std::string len_expr;
                bool end_is_full =
                    trans_end == (caller_info.inclusive_end
                                      ? slice + ".len - 1"
                                      : slice + ".len");
                if (trans_base == slice + ".ptr" && end_is_full) {
                    len_expr = slice + ".len";
                } else if (end_is_full) {
                    if (trans_base.find('+') != std::string::npos ||
                        trans_base.find('-') != std::string::npos)
                        len_expr = slice + ".len - (" + trans_base + ")";
                    else
                        len_expr = slice + ".len - " + trans_base;
                } else {
                    if (info.inclusive_end) {
                        if (trans_base == slice + ".ptr") {
                            if (trans_end.size() >= 4 &&
                                trans_end.substr(trans_end.size() - 4) == " - 1")
                                len_expr = trans_end.substr(0, trans_end.size() - 4);
                            else
                                len_expr = trans_end + " + 1";
                        } else {
                            len_expr =
                                "(" + trans_end + ") - (" + trans_base + ") + 1";
                        }
                    } else {
                        if (trans_base == slice + ".ptr") {
                            len_expr = trans_end;
                        } else {
                            if (trans_base.find('+') != std::string::npos ||
                                trans_base.find('-') != std::string::npos)
                                len_expr = slice + ".len - (" + trans_base + ")";
                            else
                                len_expr = slice + ".len - " + trans_base;
                        }
                    }
                }

                std::string compound = "(" + info.slice_type + "){" + ptr_expr +
                                       ", " + len_expr + "}";
                std::string new_args = compound;
                for (unsigned i = 0; i < NumArgs; i++) {
                    if ((int)i == info.base_param_index) continue;
                    if ((int)i == info.end_param_index) continue;
                    if ((int)i == info.len_param_index) continue;
                    new_args +=
                        ", " + translateArgExpr(CE->getArg(i), *CallerT, Ctx);
                }
                replaceArgs(new_args);
            };

            if (is_pass_through && info.end_param_index >= 0) {
                std::string end_arg = arg_texts[info.end_param_index];
                std::string caller_end_name;
                if (caller_info.end_param_index >= 0)
                    caller_end_name =
                        EnclosingFD->getParamDecl(caller_info.end_param_index)
                            ->getNameAsString();

                if (end_arg == caller_end_name) {
                    std::string new_args = caller_info.slice_param_name;
                    for (unsigned i = 0; i < NumArgs; i++) {
                        if ((int)i == info.base_param_index) continue;
                        if ((int)i == info.end_param_index) continue;
                        if ((int)i == info.len_param_index) continue;
                        new_args += ", " + arg_texts[i];
                    }
                    replaceArgs(new_args);
                } else {
                    buildSubslice(CE->getArg(info.base_param_index),
                                  CE->getArg(info.end_param_index));
                }
            } else if (info.end_param_index >= 0) {
                buildSubslice(CE->getArg(info.base_param_index),
                              CE->getArg(info.end_param_index));
            } else if (info.len_param_index >= 0) {
                // (ptr, len) callee from a reshaped caller. Pass-through
                // when the args are exactly the caller's own base + len.
                std::string len_arg = arg_texts[info.len_param_index];
                std::string caller_len_name;
                if (caller_info.len_param_index >= 0)
                    caller_len_name =
                        EnclosingFD->getParamDecl(caller_info.len_param_index)
                            ->getNameAsString();
                if (is_pass_through && !caller_len_name.empty() &&
                    len_arg == caller_len_name) {
                    std::string new_args = caller_info.slice_param_name;
                    for (unsigned i = 0; i < NumArgs; i++) {
                        if ((int)i == info.base_param_index) continue;
                        if ((int)i == info.len_param_index) continue;
                        new_args += ", " + arg_texts[i];
                    }
                    replaceArgs(new_args);
                } else {
                    std::string trans_base = translateArgExpr(
                        CE->getArg(info.base_param_index), *CallerT, Ctx);
                    std::string trans_len = translateArgExpr(
                        CE->getArg(info.len_param_index), *CallerT, Ctx);
                    std::string ptr_expr = trans_base == slice + ".ptr"
                                               ? slice + ".ptr"
                                               : slice + ".ptr + " + trans_base;
                    std::string compound = "(" + info.slice_type + "){" +
                                           ptr_expr + ", " + trans_len + "}";
                    std::string new_args = compound;
                    for (unsigned i = 0; i < NumArgs; i++) {
                        if ((int)i == info.base_param_index) continue;
                        if ((int)i == info.len_param_index) continue;
                        new_args +=
                            ", " + translateArgExpr(CE->getArg(i), *CallerT, Ctx);
                    }
                    replaceArgs(new_args);
                }
            }
        } else {
            // Caller keeps its original shape: wrap the (ptr,len)/(lo,hi)
            // args into a freshly constructed slice.
            std::string base_text = arg_texts[info.base_param_index];
            std::string len_text;

            if (info.end_param_index >= 0) {
                std::string end_text = arg_texts[info.end_param_index];
                if (info.inclusive_end) {
                    std::string prefix = base_text + " + ";
                    if (end_text.substr(0, prefix.size()) == prefix) {
                        std::string suffix = end_text.substr(prefix.size());
                        if (suffix.size() >= 4 &&
                            suffix.substr(suffix.size() - 4) == " - 1")
                            len_text = suffix.substr(0, suffix.size() - 4);
                        else
                            len_text = suffix + " + 1";
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
            if (info.lookahead > 0)
                len_text = len_text + " + " + std::to_string(info.lookahead);

            std::string compound =
                "(" + info.slice_type + "){" + base_text + ", " + len_text + "}";
            std::string new_args = compound;
            for (unsigned i = 0; i < NumArgs; i++) {
                if ((int)i == info.base_param_index) continue;
                if ((int)i == info.end_param_index) continue;
                if ((int)i == info.len_param_index) continue;
                new_args += ", " + arg_texts[i];
            }
            replaceArgs(new_args);
        }
    }
}

// ============================================================================
// Forward declarations
// ============================================================================

void SliceRewriter::rewriteForwardDeclarations(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();
    FileID MainFID = SM.getMainFileID();

    std::set<FileID> typedef_emitted_files;

    for (const auto &[Canon, T] : slice_targets) {
        const PtrIndexSliceRecord &info = *T.S;
        for (const FunctionDecl *Redecl : Canon->redecls()) {
            if (Redecl->isThisDeclarationADefinition())
                continue;
            if (SM.isInSystemHeader(Redecl->getLocation()))
                continue;
            if (Redecl->getNumParams() == 0)
                continue;

            // Only prototypes still in the pre-slice shape are rewritten
            // (an already-reshaped header from an earlier TU is left be).
            unsigned n = Redecl->getNumParams();
            auto stillOldShape = [&]() {
                if (info.base_param_index >= 0) {
                    if ((unsigned)info.base_param_index >= n)
                        return false;
                    return Redecl->getParamDecl(info.base_param_index)
                        ->getType()
                        ->isPointerType();
                }
                for (int si : info.singleton_param_indices) {
                    if ((unsigned)si >= n ||
                        !Redecl->getParamDecl(si)->getType()->isPointerType())
                        return false;
                }
                return true;
            };
            if (!stillOldShape())
                continue;

            FileID DeclFID = SM.getFileID(Redecl->getLocation());
            if (DeclFID != MainFID && !existing_typedefs.count(info.slice_type) &&
                typedef_emitted_files.insert(DeclFID).second) {
                std::string td = "typedef struct { " + info.pointee_type +
                                 " *ptr; size_t len; } " + info.slice_type +
                                 ";\n\n";
                TheRewriter.InsertTextBefore(Redecl->getBeginLoc(), td);
            }

            std::string new_params = info.slice_type + " " + info.slice_param_name;
            if (!info.singleton_param_indices.empty()) {
                for (unsigned i = 0; i < n; i++) {
                    const ParmVarDecl *P = Redecl->getParamDecl(i);
                    bool is_singleton =
                        std::find(info.singleton_param_indices.begin(),
                                  info.singleton_param_indices.end(),
                                  (int)i) != info.singleton_param_indices.end();
                    if (is_singleton) {
                        new_params += ", int " + P->getNameAsString();
                    } else {
                        new_params += ", " + P->getType().getAsString();
                        if (!P->getNameAsString().empty())
                            new_params += " " + P->getNameAsString();
                    }
                }
            } else {
                for (unsigned i = 0; i < n; i++) {
                    if ((int)i == info.base_param_index) continue;
                    if ((int)i == info.end_param_index) continue;
                    if ((int)i == info.len_param_index) continue;
                    const ParmVarDecl *P = Redecl->getParamDecl(i);
                    new_params += ", " + P->getType().getAsString();
                    if (!P->getNameAsString().empty())
                        new_params += " " + P->getNameAsString();
                }
            }

            SourceLocation FirstParamLoc = Redecl->getParamDecl(0)->getBeginLoc();
            unsigned LastIdx = n - 1;
            SourceLocation LastParamEnd = Lexer::getLocForEndOfToken(
                Redecl->getParamDecl(LastIdx)->getEndLoc(), 0, SM, LO);
            unsigned startOff = SM.getFileOffset(FirstParamLoc);
            unsigned endOff = SM.getFileOffset(LastParamEnd);
            TheRewriter.ReplaceText(FirstParamLoc, endOff - startOff, new_params);

            if (info.return_type_changed) {
                SourceLocation RetStart = Redecl->getBeginLoc();
                SourceLocation FuncNameLoc = Redecl->getLocation();
                unsigned retLen =
                    SM.getFileOffset(FuncNameLoc) - SM.getFileOffset(RetStart);
                TheRewriter.ReplaceText(RetStart, retLen, "int ");
            }
        }
    }

    // Global-return functions: only the return type changes on their
    // prototypes (int instead of T*).
    for (const auto &[name, Canon] : global_return_fns) {
        for (const FunctionDecl *Redecl : Canon->redecls()) {
            if (Redecl->isThisDeclarationADefinition())
                continue;
            if (SM.isInSystemHeader(Redecl->getLocation()))
                continue;
            if (!Redecl->getReturnType()->isPointerType())
                continue; // already rewritten
            rewriteReturnTypeAsInt(TheRewriter, Redecl, SM, LO);
        }
    }
}

// ============================================================================
// T* -> int return-type propagation into callers
// ============================================================================

void SliceRewriter::fixReturnTypeChanges(ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    // ---- Callers receiving return values from reshaped functions -----
    // `T *q = find(...)` -> `int q = ...` plus use fix-ups. Only for
    // callers that were not themselves reshaped (those were handled
    // during applySliceBody).
    struct RecvInfo {
        const VarDecl *VD;
        std::string base_text;
    };
    std::vector<RecvInfo> recv_vars;

    class ReturnValueCallFinder : public RecursiveASTVisitor<ReturnValueCallFinder> {
      public:
        SliceRewriter *Self;
        ASTContext &Ctx;
        std::vector<RecvInfo> *Out;
        ReturnValueCallFinder(SliceRewriter *S, ASTContext &C,
                              std::vector<RecvInfo> *O)
            : Self(S), Ctx(C), Out(O) {}

        bool VisitVarDecl(VarDecl *VD) {
            if (!VD->hasInit() || !VD->getType()->isPointerType())
                return true;
            const auto *CE = dyn_cast<CallExpr>(VD->getInit()->IgnoreImpCasts());
            if (!CE)
                return true;
            const FunctionDecl *Callee = CE->getDirectCallee();
            const SliceTarget *T =
                Callee ? Self->targetFor(Callee->getCanonicalDecl()) : nullptr;
            if (!T || !T->S->return_type_changed)
                return true;

            std::string base;
            if (T->S->base_param_index >= 0 &&
                (unsigned)T->S->base_param_index < CE->getNumArgs()) {
                const Expr *BaseArg = CE->getArg(T->S->base_param_index);
                SourceManager &SM = Ctx.getSourceManager();
                const LangOptions &LO = Ctx.getLangOpts();
                if (const auto *CLE = dyn_cast<CompoundLiteralExpr>(
                        BaseArg->IgnoreImpCasts())) {
                    if (const auto *ILE =
                            dyn_cast<InitListExpr>(CLE->getInitializer())) {
                        if (ILE->getNumInits() > 0)
                            base = getSourceText(ILE->getInit(0), SM, LO);
                    }
                } else {
                    base = getSourceText(BaseArg, SM, LO);
                }
            }
            Out->push_back({VD, base});
            return true;
        }
    };

    ReturnValueCallFinder finder(this, Ctx, &recv_vars);
    finder.TraverseDecl(Ctx.getTranslationUnitDecl());

    for (const RecvInfo &ri : recv_vars) {
        const VarDecl *VD = ri.VD;
        if (index_return_vars.count(VD))
            continue; // already retyped during applySliceBody

        const FunctionDecl *EnclosingFD = enclosingFunction(VD->getLocation(), SM);
        if (!EnclosingFD)
            continue;
        const SliceTarget *CallerT = targetFor(EnclosingFD->getCanonicalDecl());
        if (CallerT && CallerT->body_applicable)
            continue; // reshaped callers handled in applySliceBody

        index_return_vars.insert(VD);

        const DeclStmt *DS = findDeclStmtForVar(VD, EnclosingFD->getBody());
        if (DS) {
            SourceLocation TypeStart = DS->getBeginLoc();
            SourceLocation NameLoc = VD->getLocation();
            unsigned origLen =
                SM.getFileOffset(NameLoc) - SM.getFileOffset(TypeStart);
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

            VarUsageTransformer(Rewriter &R, SourceManager &SM,
                                const LangOptions &LO, ASTContext &C,
                                const VarDecl *V, const std::string &B)
                : Rewrite(R), SM(SM), LO(LO), Ctx(C), TargetVar(V), BaseText(B) {}

            bool VisitDeclRefExpr(DeclRefExpr *DRE) {
                if (DRE->getDecl() != TargetVar)
                    return true;
                const Stmt *Parent = skipTransparentParents(DRE, Ctx);
                if (!Parent)
                    return true;

                std::string varName = TargetVar->getNameAsString();

                if (const auto *UO = dyn_cast<UnaryOperator>(Parent)) {
                    // *var -> base[var]
                    if (UO->getOpcode() == UO_Deref) {
                        SourceLocation Start = UO->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            UO->getEndLoc(), 0, SM, LO);
                        Rewrite.ReplaceText(
                            Start, SM.getFileOffset(End) - SM.getFileOffset(Start),
                            BaseText + "[" + varName + "]");
                        return true;
                    }
                    // !var -> var == -1
                    if (UO->getOpcode() == UO_LNot) {
                        SourceLocation Start = UO->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            UO->getEndLoc(), 0, SM, LO);
                        Rewrite.ReplaceText(
                            Start, SM.getFileOffset(End) - SM.getFileOffset(Start),
                            varName + " == -1");
                        return true;
                    }
                }

                // if (var) -> if (var != -1)
                if (const auto *IS = dyn_cast<IfStmt>(Parent)) {
                    if (IS->getCond()->IgnoreImpCasts() == DRE) {
                        SourceLocation Start = DRE->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            DRE->getEndLoc(), 0, SM, LO);
                        Rewrite.ReplaceText(
                            Start, SM.getFileOffset(End) - SM.getFileOffset(Start),
                            varName + " != -1");
                        return true;
                    }
                }

                // var - base (possibly cast) -> var
                if (const auto *BO = dyn_cast<BinaryOperator>(Parent)) {
                    if (BO->getOpcode() == BO_Sub) {
                        if (dyn_cast<DeclRefExpr>(BO->getRHS()->IgnoreImpCasts())) {
                            std::string rhs = getSourceText(BO->getRHS(), SM, LO);
                            if (rhs == BaseText) {
                                const Stmt *GrandParent =
                                    skipTransparentParents(BO, Ctx);
                                const Stmt *ReplNode = BO;
                                if (GrandParent &&
                                    isa<CStyleCastExpr>(GrandParent))
                                    ReplNode = GrandParent;
                                SourceLocation Start = ReplNode->getBeginLoc();
                                SourceLocation End = Lexer::getLocForEndOfToken(
                                    ReplNode->getEndLoc(), 0, SM, LO);
                                Rewrite.ReplaceText(Start,
                                                    SM.getFileOffset(End) -
                                                        SM.getFileOffset(Start),
                                                    varName);
                                return true;
                            }
                        }
                    }
                }

                return true;
            }
        };

        VarUsageTransformer transformer(TheRewriter, SM, LO, Ctx, VD, ri.base_text);
        transformer.TraverseStmt(const_cast<Stmt *>(EnclosingFD->getBody()));
    }

    // ---- Direct-call null comparisons on global-return functions -----
    class GlobalReturnDirectCallTransformer
        : public RecursiveASTVisitor<GlobalReturnDirectCallTransformer> {
      public:
        SliceRewriter *Self;
        Rewriter &Rewrite;
        SourceManager &SM;
        const LangOptions &LO;

        GlobalReturnDirectCallTransformer(SliceRewriter *S, Rewriter &R,
                                          SourceManager &SM,
                                          const LangOptions &LO)
            : Self(S), Rewrite(R), SM(SM), LO(LO) {}

        bool isGlobalReturnCall(const Expr *E) const {
            const auto *CE = dyn_cast<CallExpr>(E);
            if (!CE)
                return false;
            const FunctionDecl *Callee = CE->getDirectCallee();
            return Callee &&
                   Self->global_return_fns.count(Callee->getNameAsString());
        }

        bool VisitBinaryOperator(BinaryOperator *BO) {
            if (BO->getOpcode() != BO_EQ && BO->getOpcode() != BO_NE)
                return true;
            const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
            const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
            const Expr *NullExpr = nullptr;
            if (isGlobalReturnCall(LHS) && isNullLikeExpr(RHS))
                NullExpr = BO->getRHS();
            else if (isGlobalReturnCall(RHS) && isNullLikeExpr(LHS))
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
            Rewrite.ReplaceText(Start,
                                SM.getFileOffset(End) - SM.getFileOffset(Start),
                                "-1");
            return true;
        }
    };

    GlobalReturnDirectCallTransformer directCallTransformer(this, TheRewriter, SM,
                                                            LO);
    directCallTransformer.TraverseDecl(Ctx.getTranslationUnitDecl());

    // ---- Global-return function definitions --------------------------
    for (const auto &[name, Canon] : global_return_fns) {
        const FunctionDecl *Def = nullptr;
        for (const FunctionDecl *Redecl : Canon->redecls()) {
            if (Redecl->isThisDeclarationADefinition() && Redecl->hasBody()) {
                Def = Redecl;
                break;
            }
        }
        if (!Def || !Def->getReturnType()->isPointerType())
            continue;

        rewriteReturnTypeAsInt(TheRewriter, Def, SM, LO);

        class GlobalReturnFixer : public RecursiveASTVisitor<GlobalReturnFixer> {
          public:
            Rewriter &Rewrite;
            SourceManager &SM;
            const LangOptions &LO;

            GlobalReturnFixer(Rewriter &R, SourceManager &SM,
                              const LangOptions &LO)
                : Rewrite(R), SM(SM), LO(LO) {}

            bool VisitReturnStmt(ReturnStmt *RS) {
                const Expr *RetVal = RS->getRetValue();
                if (!RetVal)
                    return true;

                const Expr *Stripped = RetVal->IgnoreParenImpCasts();
                bool isNullAST = false;
                if (const auto *IL = dyn_cast<IntegerLiteral>(Stripped)) {
                    if (IL->getValue() == 0)
                        isNullAST = true;
                }
                if (isa<GNUNullExpr>(Stripped))
                    isNullAST = true;
                if (const auto *CSC = dyn_cast<CStyleCastExpr>(Stripped)) {
                    const Expr *Sub = CSC->getSubExpr()->IgnoreParenImpCasts();
                    if (const auto *IL = dyn_cast<IntegerLiteral>(Sub)) {
                        if (IL->getValue() == 0)
                            isNullAST = true;
                    }
                }

                SourceLocation ValStart = RetVal->getBeginLoc();
                SourceLocation ValEnd = RetVal->getEndLoc();
                if (ValStart.isMacroID()) {
                    auto ExpRange = SM.getExpansionRange(RetVal->getSourceRange());
                    ValStart = ExpRange.getBegin();
                    ValEnd = ExpRange.getEnd();
                }
                SourceLocation ValEndTok =
                    Lexer::getLocForEndOfToken(ValEnd, 0, SM, LO);

                if (isNullAST) {
                    Rewrite.ReplaceText(ValStart,
                                        SM.getFileOffset(ValEndTok) -
                                            SM.getFileOffset(ValStart),
                                        "-1");
                    return true;
                }

                StringRef valText = Lexer::getSourceText(
                    CharSourceRange::getCharRange(ValStart, ValEndTok), SM, LO);
                if (valText == "NULL" || valText == "0" || valText == "__null" ||
                    valText == "((void*)0)" || valText == "((void *)0)") {
                    Rewrite.ReplaceText(ValStart,
                                        SM.getFileOffset(ValEndTok) -
                                            SM.getFileOffset(ValStart),
                                        "-1");
                    return true;
                }

                // &global_array[expr] -> expr
                const Expr *RV = RetVal->IgnoreImpCasts();
                if (const auto *UO = dyn_cast<UnaryOperator>(RV)) {
                    if (UO->getOpcode() == UO_AddrOf) {
                        const Expr *Sub = UO->getSubExpr()->IgnoreImpCasts();
                        if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Sub)) {
                            const Expr *Idx = ASE->getIdx();
                            SourceLocation IdxStart = Idx->getBeginLoc();
                            SourceLocation IdxEnd = Lexer::getLocForEndOfToken(
                                Idx->getEndLoc(), 0, SM, LO);
                            StringRef idxText = Lexer::getSourceText(
                                CharSourceRange::getCharRange(IdxStart, IdxEnd),
                                SM, LO);
                            Rewrite.ReplaceText(ValStart,
                                                SM.getFileOffset(ValEndTok) -
                                                    SM.getFileOffset(ValStart),
                                                idxText.str());
                            return true;
                        }
                    }
                }
                return true;
            }
        };

        GlobalReturnFixer fixer(TheRewriter, SM, LO);
        fixer.TraverseStmt(const_cast<Stmt *>(Def->getBody()));
    }

    // ---- Callers of global-return functions --------------------------
    struct GlobalRecvInfo {
        const VarDecl *VD;
        std::string global_array;
    };
    std::vector<GlobalRecvInfo> global_recv_vars;

    class GlobalReturnCallFinder
        : public RecursiveASTVisitor<GlobalReturnCallFinder> {
      public:
        SliceRewriter *Self;
        std::vector<GlobalRecvInfo> *Out;
        GlobalReturnCallFinder(SliceRewriter *S, std::vector<GlobalRecvInfo> *O)
            : Self(S), Out(O) {}

        void checkCall(const CallExpr *CE, const VarDecl *RecvVD) {
            const FunctionDecl *Callee = CE->getDirectCallee();
            if (!Callee || !RecvVD)
                return;
            auto it = Self->Meta.global_return_functions.find(
                Callee->getNameAsString());
            if (it == Self->Meta.global_return_functions.end())
                return;
            if (!Self->global_return_fns.count(Callee->getNameAsString()))
                return;
            Out->push_back({RecvVD, it->second.global_array_name});
        }

        bool VisitVarDecl(VarDecl *VD) {
            if (!VD->hasInit() || !VD->getType()->isPointerType())
                return true;
            if (const auto *CE = dyn_cast<CallExpr>(VD->getInit()->IgnoreImpCasts()))
                checkCall(CE, VD);
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
            if (const auto *CE = dyn_cast<CallExpr>(BO->getRHS()->IgnoreImpCasts()))
                checkCall(CE, VD);
            return true;
        }
    };

    GlobalReturnCallFinder gFinder(this, &global_recv_vars);
    gFinder.TraverseDecl(Ctx.getTranslationUnitDecl());

    std::set<const VarDecl *> done;
    for (const GlobalRecvInfo &gi : global_recv_vars) {
        const VarDecl *VD = gi.VD;
        if (!done.insert(VD).second)
            continue;

        const FunctionDecl *EnclosingFD = enclosingFunction(VD->getLocation(), SM);
        if (!EnclosingFD)
            continue;
        const SliceTarget *CallerT = targetFor(EnclosingFD->getCanonicalDecl());
        if (CallerT && CallerT->body_applicable)
            continue;

        const DeclStmt *DS = findDeclStmtForVar(VD, EnclosingFD->getBody());
        if (DS) {
            SourceLocation TypeStart = DS->getBeginLoc();
            SourceLocation NameLoc = VD->getLocation();
            unsigned origLen =
                SM.getFileOffset(NameLoc) - SM.getFileOffset(TypeStart);
            TheRewriter.ReplaceText(TypeStart, origLen, "int ");
        }

        class GlobalVarUsageTransformer
            : public RecursiveASTVisitor<GlobalVarUsageTransformer> {
          public:
            Rewriter &Rewrite;
            SourceManager &SM;
            const LangOptions &LO;
            ASTContext &Ctx;
            const VarDecl *TargetVar;
            const std::string &GlobalArray;

            GlobalVarUsageTransformer(Rewriter &R, SourceManager &SM,
                                      const LangOptions &LO, ASTContext &C,
                                      const VarDecl *V, const std::string &G)
                : Rewrite(R), SM(SM), LO(LO), Ctx(C), TargetVar(V),
                  GlobalArray(G) {}

            bool VisitDeclRefExpr(DeclRefExpr *DRE) {
                if (DRE->getDecl() != TargetVar)
                    return true;
                const Stmt *Parent = skipTransparentParents(DRE, Ctx);
                if (!Parent)
                    return true;

                std::string varName = TargetVar->getNameAsString();

                // var->field -> global_array[var].field
                if (const auto *ME = dyn_cast<MemberExpr>(Parent)) {
                    if (ME->isArrow()) {
                        std::string field = ME->getMemberDecl()->getNameAsString();
                        SourceLocation Start = ME->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            ME->getEndLoc(), 0, SM, LO);
                        Rewrite.ReplaceText(
                            Start, SM.getFileOffset(End) - SM.getFileOffset(Start),
                            GlobalArray + "[" + varName + "]." + field);
                        return true;
                    }
                }

                if (const auto *UO = dyn_cast<UnaryOperator>(Parent)) {
                    // *var -> global_array[var]
                    if (UO->getOpcode() == UO_Deref) {
                        SourceLocation Start = UO->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            UO->getEndLoc(), 0, SM, LO);
                        Rewrite.ReplaceText(
                            Start, SM.getFileOffset(End) - SM.getFileOffset(Start),
                            GlobalArray + "[" + varName + "]");
                        return true;
                    }
                    // !var -> var == -1
                    if (UO->getOpcode() == UO_LNot) {
                        SourceLocation Start = UO->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            UO->getEndLoc(), 0, SM, LO);
                        Rewrite.ReplaceText(
                            Start, SM.getFileOffset(End) - SM.getFileOffset(Start),
                            varName + " == -1");
                        return true;
                    }
                }

                // var == NULL / var != NULL -> var == -1 / var != -1
                if (const auto *BO = dyn_cast<BinaryOperator>(Parent)) {
                    if (BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE) {
                        const Expr *Other = nullptr;
                        if (BO->getLHS()->IgnoreImpCasts() == DRE)
                            Other = BO->getRHS()->IgnoreImpCasts();
                        else
                            Other = BO->getLHS()->IgnoreImpCasts();

                        bool isNull = false;
                        const Expr *OtherStripped = Other->IgnoreParens();
                        if (const auto *IL =
                                dyn_cast<IntegerLiteral>(OtherStripped)) {
                            if (IL->getValue() == 0)
                                isNull = true;
                        }
                        if (isa<GNUNullExpr>(OtherStripped))
                            isNull = true;
                        if (const auto *CSC =
                                dyn_cast<CStyleCastExpr>(OtherStripped)) {
                            const Expr *Sub =
                                CSC->getSubExpr()->IgnoreParenImpCasts();
                            if (const auto *IL = dyn_cast<IntegerLiteral>(Sub)) {
                                if (IL->getValue() == 0)
                                    isNull = true;
                            }
                        }
                        if (!isNull) {
                            SourceLocation OtherStart = Other->getBeginLoc();
                            SourceLocation OtherEnd = Other->getEndLoc();
                            if (OtherStart.isMacroID()) {
                                auto ExpRange = SM.getExpansionRange(
                                    SourceRange(OtherStart, OtherEnd));
                                OtherStart = ExpRange.getBegin();
                                OtherEnd = ExpRange.getEnd();
                            }
                            SourceLocation OtherEndTok =
                                Lexer::getLocForEndOfToken(OtherEnd, 0, SM, LO);
                            StringRef otherText = Lexer::getSourceText(
                                CharSourceRange::getCharRange(OtherStart,
                                                              OtherEndTok),
                                SM, LO);
                            if (otherText == "NULL" || otherText == "0" ||
                                otherText == "__null")
                                isNull = true;
                        }

                        if (isNull) {
                            std::string op =
                                (BO->getOpcode() == BO_EQ) ? " == " : " != ";
                            SourceLocation Start = BO->getBeginLoc();
                            SourceLocation End = BO->getEndLoc();
                            if (Start.isMacroID() || End.isMacroID()) {
                                auto ExpRange =
                                    SM.getExpansionRange(BO->getSourceRange());
                                Start = ExpRange.getBegin();
                                End = ExpRange.getEnd();
                            }
                            SourceLocation EndTok =
                                Lexer::getLocForEndOfToken(End, 0, SM, LO);
                            Rewrite.ReplaceText(Start,
                                                SM.getFileOffset(EndTok) -
                                                    SM.getFileOffset(Start),
                                                varName + op + "-1");
                            return true;
                        }
                    }
                }

                // if (var) -> if (var != -1)
                if (const auto *IS = dyn_cast<IfStmt>(Parent)) {
                    if (IS->getCond()->IgnoreImpCasts() == DRE) {
                        SourceLocation Start = DRE->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            DRE->getEndLoc(), 0, SM, LO);
                        Rewrite.ReplaceText(
                            Start, SM.getFileOffset(End) - SM.getFileOffset(Start),
                            varName + " != -1");
                        return true;
                    }
                }

                // var as a call argument to an unchanged callee: the
                // callee still expects the pointer, so reconstruct it.
                if (const auto *CE = dyn_cast<CallExpr>(Parent)) {
                    for (const Expr *Arg : CE->arguments()) {
                        if (Arg->IgnoreParenImpCasts() != DRE)
                            continue;
                        SourceLocation Start = DRE->getBeginLoc();
                        SourceLocation End = Lexer::getLocForEndOfToken(
                            DRE->getEndLoc(), 0, SM, LO);
                        Rewrite.ReplaceText(
                            Start, SM.getFileOffset(End) - SM.getFileOffset(Start),
                            "&" + GlobalArray + "[" + varName + "]");
                        return true;
                    }
                }

                return true;
            }
        };

        GlobalVarUsageTransformer transformer(TheRewriter, SM, LO, Ctx, VD,
                                              gi.global_array);
        transformer.TraverseStmt(const_cast<Stmt *>(EnclosingFD->getBody()));
    }
}

// ============================================================================
// Driver
// ============================================================================

void SliceRewriter::run(ASTContext &Ctx) {
    collectTU(Ctx);
    verifyTargets(Ctx);
    if (slice_targets.empty() && global_return_fns.empty())
        return;

    // Body reshaping first (matching the pre-split tool's edit order),
    // then typedefs, call sites, forward declarations, and finally the
    // return-type propagation into callers.
    for (const auto &[Canon, T] : slice_targets) {
        if (!T.body_applicable)
            continue;
        if (!T.S->singleton_param_indices.empty())
            applySingletonBody(T, Ctx);
        else if (T.S->base_param_index >= 0)
            applySliceBody(T, Ctx);
    }

    emitTypedefs(Ctx);
    rewriteCallSites(Ctx);
    rewriteForwardDeclarations(Ctx);
    fixReturnTypeChanges(Ctx);
}

} // namespace xj
