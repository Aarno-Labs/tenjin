// Alloc-motion analysis + transform.
//
// Rewrites the "heap-allocate a struct, then fill its fields before first
// use" idiom into "build the value on the stack, then box it". Instead of
// minting a temporary per field, we materialize a single stack struct and
// redirect every field access `s->f` made before the first whole-pointer use
// to `_xj_s_stack.f`:
//
//     cp_state_t *s = calloc(1, sizeof *s);   ==>  cp_state_t _xj_s_stack = {0};
//     s->bits = 0;                                 _xj_s_stack.bits = 0;
//     s->count = 0;                                _xj_s_stack.count = 0;
//     for (...) s->bits |= ...;                    for (...) _xj_s_stack.bits |= ...;
//     s->count = first_bytes * 8;                  _xj_s_stack.count = first_bytes * 8;
//     ... = cp_read_bits(s, 1);                    cp_state_t *s =
//                                                      box__new(&_xj_s_stack, sizeof(_xj_s_stack));
//                                                  ... = cp_read_bits(s, 1);
//
// Because the stack struct's fields ARE the per-field temporaries (accessed
// by name), this captures repeated writes, compound assignments, array and
// nested-field writes, and field-to-field reads uniformly. For calloc the
// stack struct is `= {0}`, matching calloc's zeroing; for malloc we require
// every field to be assigned before the box (so nothing reads indeterminate
// storage we did not also leave indeterminate).
//
// The pass stays conservative: it only fires when, before the first use of
// the pointer as a whole value, `s` appears solely as the base of `s->field`
// accesses (no escape via `&s`/`&s->f`, no aliasing, no reassignment), the
// init region has no labels/gotos that could skip the boxing, and all
// locations are editable. Anything else is left untouched.
//
// See ~/tractor/Allocations.md for the high-level algorithm.

#include "AllocMotion.h"
#include "AllocMotionAction.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <string>
#include <vector>

using namespace clang;
using namespace clang::tooling;

namespace {

// ---- small AST helpers ----------------------------------------------------

// True if E (after stripping parens and implicit casts) is a reference to V.
bool isRefTo(const Expr *E, const VarDecl *V) {
    if (!E)
        return false;
    const auto *DRE = dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts());
    return DRE && DRE->getDecl() == V;
}

// Does `haystack` contain `needle` anywhere in its subtree?
bool contains(const Stmt *haystack, const Stmt *needle) {
    if (!haystack || !needle)
        return false;
    if (haystack == needle)
        return true;
    for (const Stmt *child : haystack->children())
        if (contains(child, needle))
            return true;
    return false;
}

// Does the subtree reference variable V at all?
bool subtreeRefs(const Stmt *S, const VarDecl *V) {
    if (!S)
        return false;
    if (const auto *DRE = dyn_cast<DeclRefExpr>(S))
        if (DRE->getDecl() == V)
            return true;
    for (const Stmt *child : S->children())
        if (subtreeRefs(child, V))
            return true;
    return false;
}

// Does the subtree contain a label, goto, or computed goto? Such a statement
// in the init region could let control flow skip the boxing or re-enter the
// region, so we refuse to transform across it.
bool hasLabelOrJump(const Stmt *S) {
    if (!S)
        return false;
    if (isa<LabelStmt>(S) || isa<GotoStmt>(S) || isa<IndirectGotoStmt>(S) ||
        isa<AddrLabelExpr>(S))
        return true;
    for (const Stmt *child : S->children())
        if (hasLabelOrJump(child))
            return true;
    return false;
}

// If `init` is (a cast of) a direct call to malloc/calloc, return the call.
const CallExpr *asAllocCall(const Expr *Init) {
    if (!Init)
        return nullptr;
    const auto *Call = dyn_cast<CallExpr>(Init->IgnoreParenCasts());
    if (!Call)
        return nullptr;
    const FunctionDecl *Callee = Call->getDirectCallee();
    if (!Callee)
        return nullptr;
    StringRef Name = Callee->getName();
    if (Name == "malloc" || Name == "calloc")
        return Call;
    return nullptr;
}

bool isCallocCall(const Expr *Init) {
    const CallExpr *Call = asAllocCall(Init);
    return Call && Call->getDirectCallee()->getName() == "calloc";
}

// True if E is a null pointer constant (NULL, 0, (void*)0, ...).
bool isNullConst(const Expr *E, ASTContext &Ctx) {
    return E && E->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNull) !=
                    Expr::NPCK_NotNull;
}

// Collects, across a function body, every reference to a variable `x`, every
// `x->field` access, every assignment to `x`, and whether `x`'s address (or
// the address of one of its fields) is ever taken.
class XRefVisitor : public RecursiveASTVisitor<XRefVisitor> {
  public:
    XRefVisitor(const VarDecl *X) : X(X) {}

    bool VisitDeclRefExpr(DeclRefExpr *E) {
        if (E->getDecl() == X)
            Refs.push_back(E);
        return true;
    }
    bool VisitMemberExpr(MemberExpr *ME) {
        if (ME->isArrow() && isRefTo(ME->getBase(), X)) {
            FieldMEs.push_back(ME);
            if (const auto *Base = dyn_cast<DeclRefExpr>(ME->getBase()->IgnoreParenImpCasts()))
                BaseRefs.insert(Base);
        }
        return true;
    }
    bool VisitUnaryOperator(UnaryOperator *U) {
        // &x or &x->f or &x->arr[i] -- any address that points into x's
        // storage. After boxing that storage moves to the heap, so such an
        // address would dangle; refuse to transform.
        if (U->getOpcode() == UO_AddrOf && subtreeRefs(U->getSubExpr(), X))
            SawAddrOf = true;
        return true;
    }
    bool VisitBinaryOperator(BinaryOperator *B) {
        if (B->isAssignmentOp() && isRefTo(B->getLHS(), X))
            AssignsToX.push_back(B);
        // Track plain `x->f = e` writes (used for the malloc full-init check).
        if (B->getOpcode() == BO_Assign) {
            if (const auto *ME = dyn_cast<MemberExpr>(B->getLHS()->IgnoreParenImpCasts()))
                if (ME->isArrow() && isRefTo(ME->getBase(), X))
                    AssignLHSMEs.insert(ME);
        }
        return true;
    }

    const VarDecl *X;
    std::vector<const DeclRefExpr *> Refs;
    std::vector<const MemberExpr *> FieldMEs;
    std::set<const DeclRefExpr *> BaseRefs;     // refs that are `x->...` bases
    std::set<const MemberExpr *> AssignLHSMEs;  // `x->f` that are LHS of `=`
    std::vector<const BinaryOperator *> AssignsToX;
    bool SawAddrOf = false;
};

void log(const Twine &Msg) {
    if (g_allocmotion_verbose)
        llvm::errs() << "[allocmotion] " << Msg << "\n";
}

// ---- per-function driver --------------------------------------------------

class FunctionProcessor {
  public:
    FunctionProcessor(ASTContext &Ctx, AtomicChanges &Changes, bool &HelperEmitted)
        : Ctx(Ctx), SM(Ctx.getSourceManager()), LangOpts(Ctx.getLangOpts()),
          Changes(Changes), HelperEmitted(HelperEmitted) {}

    void run(const FunctionDecl *FD) {
        const auto *Body = dyn_cast_or_null<CompoundStmt>(FD->getBody());
        if (!Body)
            return;
        if (!SM.isWrittenInMainFile(Body->getLBracLoc()))
            return;
        FuncBody = FD->getBody();
        CurFuncName = FD->getNameAsString();
        TopStmts.assign(Body->body_begin(), Body->body_end());

        for (unsigned I = 0; I < TopStmts.size(); ++I)
            tryCandidateAt(I);
    }

  private:
    // Recognize the two allocation shapes at statement index `Idx`.
    void tryCandidateAt(unsigned Idx) {
        const Stmt *S = TopStmts[Idx];

        const VarDecl *X = nullptr;
        bool DeclAtSite = false;
        bool IsCalloc = false;

        if (const auto *DS = dyn_cast<DeclStmt>(S)) {
            if (!DS->isSingleDecl())
                return;
            const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl());
            if (!VD || !VD->hasInit() || !asAllocCall(VD->getInit()))
                return;
            X = VD;
            DeclAtSite = true;
            IsCalloc = isCallocCall(VD->getInit());
        } else if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
            if (BO->getOpcode() != BO_Assign || !asAllocCall(BO->getRHS()))
                return;
            const auto *DRE = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts());
            if (!DRE)
                return;
            X = dyn_cast<VarDecl>(DRE->getDecl());
            DeclAtSite = false;
            IsCalloc = isCallocCall(BO->getRHS());
        }

        if (!X || !X->isLocalVarDecl() || !X->getType()->isPointerType())
            return;

        transformCandidate(X, S, Idx, DeclAtSite, IsCalloc);
    }

    // A struct we can place on the stack and box: a complete struct (not a
    // union), with no flexible array member and no anonymous members (whose
    // fields we cannot match through `s->member`).
    const RecordDecl *eligibleStruct(QualType Pointee) {
        const auto *RT = Pointee->getAs<RecordType>();
        if (!RT)
            return nullptr;
        const RecordDecl *RD = RT->getDecl()->getDefinition();
        if (!RD || !RD->isStruct() || RD->field_empty())
            return nullptr;
        for (const FieldDecl *FD : RD->fields()) {
            if (FD->isAnonymousStructOrUnion())
                return nullptr;
            if (FD->getType()->isIncompleteType()) // flexible array member
                return nullptr;
        }
        return RD;
    }

    const Stmt *enclosingTop(const Stmt *Needle) {
        for (const Stmt *Top : TopStmts)
            if (contains(Top, Needle))
                return Top;
        return nullptr;
    }

    unsigned topIndexOf(const Stmt *Needle) {
        for (unsigned I = 0; I < TopStmts.size(); ++I)
            if (contains(TopStmts[I], Needle))
                return I;
        return ~0u;
    }

    bool inMainNonMacro(SourceLocation Loc) {
        return Loc.isValid() && !Loc.isMacroID() && SM.isWrittenInMainFile(Loc);
    }

    void transformCandidate(const VarDecl *X, const Stmt *AllocStmt, unsigned AllocIdx,
                            bool DeclAtSite, bool IsCalloc) {
        const std::string XName = X->getName().str();
        QualType Pointee = X->getType()->getPointeeType();
        const RecordDecl *RD = eligibleStruct(Pointee);
        if (!RD) {
            log("skip '" + XName + "': pointee is not a plain struct");
            return;
        }

        XRefVisitor V(X);
        V.TraverseStmt(funcBody());

        if (V.SawAddrOf) {
            log("skip '" + XName + "': address of pointer or a field is taken");
            return;
        }
        for (const BinaryOperator *A : V.AssignsToX)
            if (A != AllocStmt) {
                log("skip '" + XName + "': pointer is reassigned");
                return;
            }

        const IfStmt *NullCheck = findNullCheck(X);

        // The boxing point is the first statement that uses `x` as a whole
        // pointer (anything other than an `x->field` base, the allocation, or
        // the null-check). Field accesses before it are redirected to the
        // stack struct.
        unsigned BoxIdx = ~0u;
        const Stmt *BoxTop = nullptr;
        for (const DeclRefExpr *R : V.Refs) {
            if (contains(AllocStmt, R))
                continue;
            if (V.BaseRefs.count(R))
                continue;
            if (NullCheck && contains(NullCheck, R))
                continue;
            const Stmt *Top = enclosingTop(R);
            unsigned TI = Top ? topIndexOf(Top) : ~0u;
            if (TI == ~0u) {
                log("skip '" + XName + "': use of pointer not at statement top level");
                return;
            }
            if (TI < BoxIdx) {
                BoxIdx = TI;
                BoxTop = Top;
            }
        }
        if (!BoxTop) {
            log("skip '" + XName + "': pointer is never used as a whole value");
            return;
        }
        if (AllocIdx >= BoxIdx) {
            log("skip '" + XName + "': allocation does not precede first use");
            return;
        }

        // Field accesses to redirect: those between the allocation and the
        // boxing point. None may occur at or before the allocation.
        std::vector<const MemberExpr *> Redirect;
        for (const MemberExpr *ME : V.FieldMEs) {
            unsigned TI = topIndexOf(ME);
            if (TI == ~0u || TI <= AllocIdx) {
                log("skip '" + XName + "': field accessed before allocation");
                return;
            }
            if (TI < BoxIdx)
                Redirect.push_back(ME);
        }

        // A written array field cannot be rebuilt as `.arr = _xj_s_arr` in a
        // designated initializer, so bail if any accessed field is an array.
        for (const MemberExpr *ME : Redirect)
            if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl()))
                if (FD->getType()->isArrayType()) {
                    log("skip '" + XName + "': array field '" + FD->getName().str() +
                        "' accessed before boxing");
                    return;
                }

        if (NullCheck) {
            unsigned NcIdx = topIndexOf(NullCheck);
            if (NcIdx == ~0u || NcIdx <= AllocIdx || NcIdx >= BoxIdx) {
                log("skip '" + XName + "': null-check is out of range");
                return;
            }
        }

        // No labels/gotos before the boxing point: otherwise control flow
        // could jump past the box or back into the init region.
        for (unsigned I = 0; I < BoxIdx; ++I)
            if (hasLabelOrJump(TopStmts[I])) {
                log("skip '" + XName + "': label or goto in the init region");
                return;
            }

        // For malloc, the stack struct is not zeroed, so every field must be
        // explicitly assigned before the box. (calloc-backed structs are
        // `= {0}`, so unwritten fields are validly zero.)
        if (!IsCalloc) {
            std::set<const FieldDecl *> Written;
            for (const MemberExpr *ME : Redirect)
                if (V.AssignLHSMEs.count(ME))
                    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl()))
                        Written.insert(FD);
            for (const FieldDecl *FD : RD->fields())
                if (!Written.count(FD)) {
                    log("skip '" + XName + "': malloc field '" + FD->getName().str() +
                        "' is not initialized");
                    return;
                }
        }

        // Every location we touch must be editable.
        if (!inMainNonMacro(AllocStmt->getBeginLoc()) ||
            !inMainNonMacro(BoxTop->getBeginLoc())) {
            log("skip '" + XName + "': allocation/use not editable (macro or header)");
            return;
        }
        for (const MemberExpr *ME : Redirect)
            if (!inMainNonMacro(ME->getBeginLoc()))
                return;
        if (NullCheck && !inMainNonMacro(NullCheck->getBeginLoc()))
            return;

        if (g_allocmotion_report) {
            PresumedLoc PL = SM.getPresumedLoc(AllocStmt->getBeginLoc());
            llvm::outs() << PL.getFilename() << ":" << PL.getLine() << ":"
                         << PL.getColumn() << ": " << CurFuncName << ": '" << XName
                         << "' -> box (" << Pointee.getAsString(Ctx.getPrintingPolicy())
                         << ", " << Redirect.size() << " field access"
                         << (Redirect.size() == 1 ? "" : "es") << ", "
                         << (IsCalloc ? "calloc" : "malloc")
                         << (NullCheck ? ", null-check removed" : "") << ", "
                         << (DeclAtSite ? "decl-at-site" : "separate-decl") << ")\n";
            return;
        }

        emitEdits(XName, Pointee, RD, AllocStmt, DeclAtSite, IsCalloc, Redirect, NullCheck,
                  BoxTop);
        log("rewrote '" + XName + "' in struct allocation");
    }

    // --- the optional null-check `if (!x)` / `if (x == NULL)` ---
    const IfStmt *findNullCheck(const VarDecl *X) {
        for (const Stmt *Top : TopStmts) {
            const auto *If = dyn_cast<IfStmt>(Top);
            if (!If || If->getElse() || !If->getCond())
                continue;
            const Expr *Cond = If->getCond()->IgnoreParenImpCasts();
            if (const auto *U = dyn_cast<UnaryOperator>(Cond))
                if (U->getOpcode() == UO_LNot && isRefTo(U->getSubExpr(), X))
                    return If;
            if (const auto *B = dyn_cast<BinaryOperator>(Cond))
                if (B->getOpcode() == BO_EQ &&
                    ((isRefTo(B->getLHS(), X) && isNullConst(B->getRHS(), Ctx)) ||
                     (isRefTo(B->getRHS(), X) && isNullConst(B->getLHS(), Ctx))))
                    return If;
        }
        return nullptr;
    }

    // --- edit emission ---
    std::string stackName(const std::string &XName) { return "_xj_" + XName + "_stack"; }
    std::string tempName(const std::string &XName, const FieldDecl *FD) {
        return "_xj_" + XName + "_" + FD->getName().str();
    }

    void emitReplace(SourceLocation Begin, CharSourceRange Range, StringRef Text) {
        AtomicChange C(SM, Begin);
        if (llvm::Error E = C.replace(SM, Range, Text)) {
            llvm::consumeError(std::move(E));
            return;
        }
        Changes.push_back(std::move(C));
    }
    void emitInsert(SourceLocation Loc, StringRef Text, bool After) {
        AtomicChange C(SM, Loc);
        if (llvm::Error E = C.insert(SM, Loc, Text, After)) {
            llvm::consumeError(std::move(E));
            return;
        }
        Changes.push_back(std::move(C));
    }

    // End location just past the trailing `;` of a statement.
    SourceLocation pastSemi(const Stmt *S) {
        SourceLocation L = Lexer::findLocationAfterToken(
            S->getEndLoc(), tok::semi, SM, LangOpts,
            /*SkipTrailingWhitespaceAndNewLine=*/false);
        if (L.isValid())
            return L;
        return Lexer::getLocForEndOfToken(S->getEndLoc(), 0, SM, LangOpts);
    }

    void emitEdits(const std::string &XName, QualType Pointee, const RecordDecl *RD,
                   const Stmt *AllocStmt, bool DeclAtSite, bool IsCalloc,
                   const std::vector<const MemberExpr *> &Redirect,
                   const IfStmt *NullCheck, const Stmt *BoxTop) {
        PrintingPolicy PP = Ctx.getPrintingPolicy();
        const std::string TypeName = Pointee.getAsString(PP);
        const std::string Stack = stackName(XName);

        ensureHelper();

        // Fields touched before the box: these get a temporary.
        std::set<const FieldDecl *> Accessed;
        for (const MemberExpr *ME : Redirect)
            if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl()))
                Accessed.insert(FD);

        // 1. Replace the allocation with a temporary per accessed field.
        //    calloc-backed temps are zeroed so reads / compound-assigns of
        //    not-yet-written fields see 0, matching calloc.
        std::string Temps;
        for (const FieldDecl *FD : RD->fields()) {
            if (!Accessed.count(FD))
                continue;
            std::string Decl;
            llvm::raw_string_ostream OS(Decl);
            FD->getType().print(OS, PP, tempName(XName, FD), /*Indentation=*/0);
            OS.flush();
            Temps += Decl + (IsCalloc ? " = {0}" : "") + "; ";
        }
        emitReplace(AllocStmt->getBeginLoc(),
                    CharSourceRange::getCharRange(AllocStmt->getBeginLoc(),
                                                  pastSemi(AllocStmt)),
                    Temps);

        // 2. Redirect each `x->f` access to its temporary `_xj_x_f`.
        for (const MemberExpr *ME : Redirect)
            if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl()))
                emitReplace(ME->getBeginLoc(),
                            CharSourceRange::getTokenRange(ME->getSourceRange()),
                            tempName(XName, FD));

        // 3. Delete the null-check, if any.
        if (NullCheck)
            emitReplace(NullCheck->getBeginLoc(),
                        CharSourceRange::getCharRange(NullCheck->getBeginLoc(),
                                                      pastSemi(NullCheck)),
                        "");

        // 4. Assemble the value in one designated initializer and box it.
        //    Accessed fields take their temporary; for calloc, untouched
        //    fields get an explicit zero (`0` scalar / `{0}` aggregate).
        std::string Inits;
        bool First = true;
        for (const FieldDecl *FD : RD->fields()) {
            std::string Val = Accessed.count(FD)
                                  ? tempName(XName, FD)
                                  : (FD->getType()->isScalarType() ? "0" : "{0}");
            Inits += (First ? "" : ", ") + ("." + FD->getName().str() + " = " + Val);
            First = false;
        }

        unsigned Col = SM.getSpellingColumnNumber(BoxTop->getBeginLoc());
        std::string Indent(Col > 0 ? Col - 1 : 0, ' ');
        std::string Box = TypeName + " " + Stack + " = { " + Inits + " };\n" + Indent;
        Box += DeclAtSite ? (TypeName + " *" + XName + " = ") : (XName + " = ");
        Box += "box__new(&" + Stack + ", sizeof(" + Stack + "));\n" + Indent;
        emitInsert(BoxTop->getBeginLoc(), Box, /*After=*/false);
    }

    // Emit the box__new helper once per translation unit.
    void ensureHelper() {
        if (HelperEmitted)
            return;
        HelperEmitted = true;
        static const char *Helper =
            "#ifndef XJ_BOX_NEW_DEFINED\n"
            "#define XJ_BOX_NEW_DEFINED\n"
            "#include <stdlib.h>\n"
            "#include <string.h>\n"
            "static inline void *box__new(const void *_xj_from, size_t _xj_size) {\n"
            "    void *_xj_p = malloc(_xj_size);\n"
            "    if (_xj_p) memcpy(_xj_p, _xj_from, _xj_size);\n"
            "    return _xj_p;\n"
            "}\n"
            "#endif\n\n";
        emitInsert(SM.getLocForStartOfFile(SM.getMainFileID()), Helper, /*After=*/false);
    }

    Stmt *funcBody() { return FuncBody; }

  private:
    ASTContext &Ctx;
    SourceManager &SM;
    const LangOptions &LangOpts;
    AtomicChanges &Changes;
    bool &HelperEmitted;

    Stmt *FuncBody = nullptr;
    std::string CurFuncName;
    std::vector<const Stmt *> TopStmts;
};

} // namespace

void runAllocMotion(ASTContext &Ctx, AtomicChanges &Changes) {
    bool HelperEmitted = false;
    for (Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
        auto *FD = dyn_cast<FunctionDecl>(D);
        if (!FD || !FD->doesThisDeclarationHaveABody())
            continue;
        FunctionProcessor P(Ctx, Changes, HelperEmitted);
        P.run(FD);
    }
}
