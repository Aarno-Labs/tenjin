// PointerAccessCollector.cpp — see PointerAccessCollector.h for the
// high-level role. This file is mostly classifyAccess(), a long
// pattern-match over the syntactic context of each pointer reference.

#include "PointerAccessCollector.h"

PointerAccessCollector::PointerAccessCollector(ASTContext &Ctx)
    : Ctx(Ctx), SM(Ctx.getSourceManager()), LO(Ctx.getLangOpts()) {}

// True if `E` is one of the recognized null-pointer spellings: a 0
// literal, the GNU __null builtin, or a cast wrapping one of those
// (e.g. ((void*)0)).
bool PointerAccessCollector::isNullExpr(const Expr *E) {
    E = E->IgnoreParenImpCasts();
    if (const IntegerLiteral *IL = dyn_cast<IntegerLiteral>(E))
        return IL->getValue() == 0;
    if (const GNUNullExpr *GNE = dyn_cast<GNUNullExpr>(E))
        return true;
    if (const CStyleCastExpr *CE = dyn_cast<CStyleCastExpr>(E))
        return isNullExpr(CE->getSubExpr());
    return false;
}

// Check whether a base expression is unsafe to capture as the
// pointer's base array.
//
// The rewriter substitutes the base's source text at every access
// site, so two properties must hold for that substitution to be sound:
//
//  1. No side effects in the expression. If the base contains `++`,
//     `--`, a function call, or any assignment, those side effects
//     run once in the original (at the pointer's declaration) but
//     would run once *per access* after the rewrite. e.g.
//     `host = argv[n++]; ... host[0]` runs `n++` twice after rewrite.
//
//  2. For globals: every operand must reference a name visible
//     everywhere the pointer is used. If the base mentions a local
//     variable or parameter, pasting it into other functions emits
//     code that references an out-of-scope identifier.
//
// Returns true if the expression is unsafe; the caller should emit
// PointerAccessKind::Unknown instead of capturing the base.
static bool baseIsUnsafe(const Expr *E, bool is_global) {
    if (!E) return false;

    struct Walker : public RecursiveASTVisitor<Walker> {
        bool is_global;
        bool unsafe = false;

        explicit Walker(bool g) : is_global(g) {}

        bool VisitUnaryOperator(UnaryOperator *UO) {
            if (UO->isIncrementDecrementOp()) {
                unsafe = true;
                return false;
            }
            return true;
        }
        bool VisitBinaryOperator(BinaryOperator *BO) {
            if (BO->isAssignmentOp() || BO->getOpcode() == BO_Comma) {
                unsafe = true;
                return false;
            }
            return true;
        }
        bool VisitCallExpr(CallExpr *) {
            unsafe = true;
            return false;
        }
        bool VisitDeclRefExpr(DeclRefExpr *DRE) {
            if (!is_global) return true;
            if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                if (!VD->hasGlobalStorage()) {
                    unsafe = true;
                    return false;
                }
            }
            return true;
        }
    };

    Walker w(is_global);
    w.TraverseStmt(const_cast<Expr *>(E));
    return w.unsafe;
}

// Match the `&arr[i]` pattern and pull the base array text and the
// index expression out separately. Used when classifying initializers
// like `int *p = &buf[3];`.
bool PointerAccessCollector::isAddrOfSubscript(const Expr *E,
                                                std::string &base_text,
                                                std::string &index_text) {
    E = E->IgnoreParenImpCasts();
    const UnaryOperator *UO = dyn_cast<UnaryOperator>(E);
    if (!UO || UO->getOpcode() != UO_AddrOf)
        return false;

    const Expr *Sub = UO->getSubExpr()->IgnoreParenImpCasts();
    if (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(Sub)) {
        base_text = getSourceText(ASE->getBase()->IgnoreParenImpCasts(), SM, LO);
        index_text = getSourceText(ASE->getIdx(), SM, LO);
        return true;
    }
    return false;
}

// Inspect a pointer initializer (or, when called from classifyAccess,
// an assignment RHS) to determine:
//   - which Init*/Assign* PointerAccessKind to emit, and
//   - what the pointer's base array is (if it can be inferred here).
//
// The four supported shapes are NULL, `&arr[i]`, `arr + n`, and a bare
// pointer/array reference. Anything else falls through to Unknown,
// which makes the pointer unsafe and will be rejected by validation.
void PointerAccessCollector::analyzePointerInit(const Expr *Init,
                                                 const VarDecl *PtrVar,
                                                 PointerCandidate &candidate,
                                                 std::vector<PointerAccess> &access_list) {
    if (!Init)
        return;

    Init = Init->IgnoreParenImpCasts();

    // Drop explicit C-style casts (e.g. `(unsigned char *)dest`). The
    // cast is only meaningful for pointer arithmetic; after we switch
    // to array-subscript indexing into the base, the original element
    // type is what matters.
    while (const auto *CSCE = dyn_cast<CStyleCastExpr>(Init))
        Init = CSCE->getSubExpr()->IgnoreParenImpCasts();

    // Case 1: NULL — no base array yet, will be encoded as -1.
    if (isNullExpr(Init)) {
        PointerAccess pa;
        pa.kind = PointerAccessKind::InitNull;
        pa.loc = Init->getBeginLoc();
        pa.expr = Init;
        pa.enclosing_stmt = nullptr;
        access_list.push_back(pa);
        return;
    }

    bool is_global = PtrVar && PtrVar->hasGlobalStorage();

    auto emitUnknown = [&](const Expr *E) {
        PointerAccess pa;
        pa.kind = PointerAccessKind::Unknown;
        pa.loc = E->getBeginLoc();
        pa.expr = E;
        pa.enclosing_stmt = nullptr;
        access_list.push_back(pa);
    };

    // Case 2: &arr[i] — base is arr, initial index is i.
    std::string base_text, index_text;
    if (isAddrOfSubscript(Init, base_text, index_text)) {
        const auto *UO = cast<UnaryOperator>(Init);
        const auto *ASE = cast<ArraySubscriptExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
        if (baseIsUnsafe(ASE->getBase()->IgnoreParenImpCasts(), is_global) ||
            baseIsUnsafe(ASE->getIdx(), is_global)) {
            emitUnknown(Init);
            return;
        }
        candidate.base_array = Init;
        candidate.base_array_text = base_text;

        PointerAccess pa;
        pa.kind = PointerAccessKind::InitArrayOffset;
        pa.loc = Init->getBeginLoc();
        pa.expr = Init;
        pa.enclosing_stmt = nullptr;
        pa.offset_text = index_text;
        access_list.push_back(pa);
        return;
    }

    // Case 3: arr + offset — pointer/array on the LHS of a +.
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(Init)) {
        if (BO->getOpcode() == BO_Add) {
            const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
            const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

            if (LHS->getType()->isPointerType() || LHS->getType()->isArrayType()) {
                if (baseIsUnsafe(LHS, is_global) || baseIsUnsafe(RHS, is_global)) {
                    emitUnknown(Init);
                    return;
                }
                candidate.base_array = LHS;
                candidate.base_array_text = getSourceText(LHS, SM, LO);

                PointerAccess pa;
                pa.kind = PointerAccessKind::InitArrayOffset;
                pa.loc = Init->getBeginLoc();
                pa.expr = Init;
                pa.enclosing_stmt = nullptr;
                pa.offset_text = getSourceText(BO->getRHS(), SM, LO);
                access_list.push_back(pa);
                return;
            }
        }
    }

    // Case 4: bare pointer/array reference, e.g. `int *p = arr;` or
    // `char *p = bs->buf;`. The whole RHS becomes the base text.
    if (Init->getType()->isPointerType() || Init->getType()->isArrayType()) {
        if (baseIsUnsafe(Init, is_global)) {
            emitUnknown(Init);
            return;
        }
        candidate.base_array = Init;
        candidate.base_array_text = getSourceText(Init, SM, LO);

        PointerAccess pa;
        pa.kind = PointerAccessKind::InitArray;
        pa.loc = Init->getBeginLoc();
        pa.expr = Init;
        pa.enclosing_stmt = nullptr;
        access_list.push_back(pa);
        return;
    }

    // Anything else — the initializer is some shape we don't understand.
    PointerAccess pa;
    pa.kind = PointerAccessKind::Unknown;
    pa.loc = Init->getBeginLoc();
    pa.expr = Init;
    pa.enclosing_stmt = nullptr;
    access_list.push_back(pa);
}

// Pick up every pointer-typed VarDecl (locals and parameters), record
// it in `tracked_pointers`, and run the initializer (if any) through
// analyzePointerInit. Uninitialized pointers are still tracked because
// their base array may be set later by an assignment like `p = arr`.
bool PointerAccessCollector::VisitVarDecl(VarDecl *VD) {
    if (!VD->getType()->isPointerType())
        return true;
    if (SM.isInSystemHeader(VD->getLocation()))
        return true;

    bool is_param = isa<ParmVarDecl>(VD);

    PointerCandidate candidate;
    candidate.ptr_var = VD;
    candidate.base_array = nullptr;
    candidate.base_array_text = "";
    candidate.is_parameter = is_param;

    std::vector<PointerAccess> access_list;

    if (VD->hasInit()) {
        analyzePointerInit(VD->getInit(), VD, candidate, access_list);
    }

    tracked_pointers[VD] = candidate;
    accesses[VD] = access_list;

    if (VERBOSE)
        llvm::outs() << "[Collect] Tracking pointer: " << VD->getNameAsString()
                      << (is_param ? " (parameter)" : " (local)")
                      << " base=" << candidate.base_array_text << "\n";

    return true;
}

// Every reference to a tracked pointer flows through here. We skip the
// reference inside the pointer's own initializer (already handled in
// VisitVarDecl) and forward everything else to classifyAccess.
bool PointerAccessCollector::VisitDeclRefExpr(DeclRefExpr *DRE) {
    const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (!VD)
        return true;

    auto it = tracked_pointers.find(VD);
    if (it == tracked_pointers.end())
        return true;

    if (VD->hasInit()) {
        SourceRange initRange = VD->getInit()->getSourceRange();
        if (SM.isBeforeInTranslationUnit(DRE->getLocation(), initRange.getEnd()) &&
            !SM.isBeforeInTranslationUnit(DRE->getLocation(), initRange.getBegin())) {
            return true;
        }
    }

    classifyAccess(DRE, VD, accesses[VD], it->second);
    return true;
}

// Step up through ImplicitCastExpr / ParenExpr parents and return the
// first "real" parent. Also reports `outermost` — the topmost transparent
// wrapper around `S` itself — so callers comparing AST nodes can match
// either the bare DRE or its wrapped form.
static const Stmt *skipTransparentParentsOf(const Stmt *S, ASTContext &Ctx,
                                             const Stmt *&outermost) {
    outermost = S;
    const Stmt *Current = S;
    while (true) {
        auto Parents = Ctx.getParents(*Current);
        if (Parents.empty())
            return nullptr;
        const Stmt *P = Parents[0].get<Stmt>();
        if (!P)
            return nullptr;
        if (isa<ImplicitCastExpr>(P) || isa<ParenExpr>(P)) {
            outermost = P;
            Current = P;
            continue;
        }
        return P;
    }
}

// Walks up from a tracked-pointer DeclRefExpr and decides which
// PointerAccessKind matches the surrounding syntax. Each `if` block
// below corresponds to one parent shape (UnaryOperator, MemberExpr,
// ArraySubscriptExpr, BinaryOperator, CallExpr, ReturnStmt, ...). The
// final fallback emits Unknown, which causes validation to reject the
// pointer.
void PointerAccessCollector::classifyAccess(DeclRefExpr *DRE,
                                             const VarDecl *PtrVar,
                                             std::vector<PointerAccess> &access_list,
                                             PointerCandidate &candidate) {
    const Stmt *OutermostDRE = DRE; // top of the transparent wrapper chain over DRE
    const Stmt *Parent = skipTransparentParentsOf(DRE, Ctx, OutermostDRE);
    if (!Parent) {
        access_list.push_back({PointerAccessKind::Unknown, DRE->getLocation(),
                               DRE, nullptr, "", "", "", ""});
        return;
    }

    // A `?:` in a value-producing slot just forwards the pointer to whatever
    // encloses the conditional (return, assignment, function call, ...). The
    // pointer's real consumer — and thus the right context to classify the
    // access against — is past the `?:`. Walk up through any chain of
    // value-forwarding `?:`s so the dispatch below sees the actual consumer.
    //
    // Only the cond slot is genuine boolean context; leave Parent alone there
    // so it falls through to the boolean-context branch below.
    //
    // Bug this guards against: in `return (p != start) ? p : NULL;`, the `p`
    // in the true slot used to be classified as BoolTrue (because its parent
    // is a ConditionalOperator), which hid the fact that p escapes via
    // return. The pass would then convert p to an index, c2rust would emit a
    // bool→int→pointer cast chain, and the translated Rust would SIGSEGV.
    while (const auto *CO = dyn_cast<ConditionalOperator>(Parent)) {
        if (OutermostDRE != CO->getTrueExpr() &&
            OutermostDRE != CO->getFalseExpr())
            break; // OutermostDRE is the cond — leave Parent as the `?:`
        OutermostDRE = CO;
        Parent = skipTransparentParentsOf(CO, Ctx, OutermostDRE);
        if (!Parent) {
            access_list.push_back({PointerAccessKind::Unknown, DRE->getLocation(),
                                   DRE, nullptr, "", "", "", ""});
            return;
        }
    }

    // ---- UnaryOperator: *p, p++, p--, &p, !p ---------------------------
    if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(Parent)) {
        switch (UO->getOpcode()) {
        case UO_Deref: {
            // *p — distinguish read vs write by checking whether the
            // deref is the LHS of an assignment.
            const Stmt *GP = skipTransparentParents(UO, Ctx);
            bool is_write = false;
            if (GP) {
                if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(GP)) {
                    if (BO->isAssignmentOp() && BO->getLHS()->IgnoreParenImpCasts() == UO)
                        is_write = true;
                }
            }
            access_list.push_back({is_write ? PointerAccessKind::DerefWrite
                                            : PointerAccessKind::Deref,
                                   UO->getBeginLoc(), DRE, nullptr,
                                   "", "", "", ""});
            return;
        }
        case UO_PostInc: {
            // Either standalone `p++` or the `p++` inside `*p++`. The
            // dereferenced form maps to DerefPostInc regardless of
            // read/write context: arr[p_index++] is valid on either side.
            const Stmt *GP = skipTransparentParents(UO, Ctx);
            if (GP) {
                if (const UnaryOperator *GUO = dyn_cast<UnaryOperator>(GP)) {
                    if (GUO->getOpcode() == UO_Deref) {
                        access_list.push_back({PointerAccessKind::DerefPostInc,
                                               GUO->getBeginLoc(), DRE, nullptr,
                                               "", "", "", ""});
                        return;
                    }
                }
            }
            access_list.push_back({PointerAccessKind::Increment,
                                   UO->getBeginLoc(), DRE, nullptr,
                                   "", "", "", ""});
            return;
        }
        case UO_PreInc: {
            // ++p, possibly inside *++p.
            const Stmt *GP = skipTransparentParents(UO, Ctx);
            if (GP) {
                if (const UnaryOperator *GUO = dyn_cast<UnaryOperator>(GP)) {
                    if (GUO->getOpcode() == UO_Deref) {
                        access_list.push_back({PointerAccessKind::DerefPreInc,
                                               GUO->getBeginLoc(), DRE, nullptr,
                                               "", "", "", ""});
                        return;
                    }
                }
            }
            access_list.push_back({PointerAccessKind::Increment,
                                   UO->getBeginLoc(), DRE, nullptr,
                                   "", "", "", ""});
            return;
        }
        case UO_PostDec: {
            const Stmt *GP = skipTransparentParents(UO, Ctx);
            if (GP) {
                if (const UnaryOperator *GUO = dyn_cast<UnaryOperator>(GP)) {
                    if (GUO->getOpcode() == UO_Deref) {
                        access_list.push_back({PointerAccessKind::DerefPostDec,
                                               GUO->getBeginLoc(), DRE, nullptr,
                                               "", "", "", ""});
                        return;
                    }
                }
            }
            access_list.push_back({PointerAccessKind::Decrement,
                                   UO->getBeginLoc(), DRE, nullptr,
                                   "", "", "", ""});
            return;
        }
        case UO_PreDec: {
            const Stmt *GP = skipTransparentParents(UO, Ctx);
            if (GP) {
                if (const UnaryOperator *GUO = dyn_cast<UnaryOperator>(GP)) {
                    if (GUO->getOpcode() == UO_Deref) {
                        access_list.push_back({PointerAccessKind::DerefPreDec,
                                               GUO->getBeginLoc(), DRE, nullptr,
                                               "", "", "", ""});
                        return;
                    }
                }
            }
            access_list.push_back({PointerAccessKind::Decrement,
                                   UO->getBeginLoc(), DRE, nullptr,
                                   "", "", "", ""});
            return;
        }
        case UO_AddrOf:
            // &p — the pointer's address has been taken, so it can't
            // safely become a plain int. Validation rejects this.
            access_list.push_back({PointerAccessKind::AddressOf,
                                   UO->getBeginLoc(), DRE, nullptr,
                                   "", "", "", ""});
            return;
        case UO_LNot:
            // !p — null check via negation.
            access_list.push_back({PointerAccessKind::BoolFalse,
                                   UO->getBeginLoc(), DRE, nullptr,
                                   "", "", "", ""});
            return;
        default:
            break;
        }
    }

    // ---- MemberExpr: p->field (arrow access) --------------------------
    if (const MemberExpr *ME = dyn_cast<MemberExpr>(Parent)) {
        if (ME->isArrow()) {
            std::string field_name = ME->getMemberDecl()->getNameAsString();
            // Read vs write: write if either an assignment LHS or an
            // increment/decrement target.
            const Stmt *GP = skipTransparentParents(ME, Ctx);
            bool is_write = false;
            if (GP) {
                if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(GP)) {
                    if (BO->isAssignmentOp() && BO->getLHS()->IgnoreParenImpCasts() == ME)
                        is_write = true;
                }
                if (const UnaryOperator *UO2 = dyn_cast<UnaryOperator>(GP)) {
                    if (UO2->isIncrementDecrementOp())
                        is_write = true;
                }
            }
            access_list.push_back({is_write ? PointerAccessKind::ArrowWrite
                                            : PointerAccessKind::ArrowAccess,
                                   ME->getBeginLoc(), DRE, nullptr,
                                   "", field_name, "", ""});
            return;
        }
    }

    // ---- ArraySubscriptExpr: p[i] -------------------------------------
    if (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(Parent)) {
        // Make sure p is the base of the subscript, not the index, so
        // we don't fire on patterns like `arr[p]`.
        if (ASE->getBase()->IgnoreParenImpCasts() == DRE ||
            ASE->getLHS()->IgnoreParenImpCasts() == DRE) {
            std::string sub_text = getSourceText(ASE->getIdx(), SM, LO);
            const Stmt *GP = skipTransparentParents(ASE, Ctx);
            bool is_write = false;
            if (GP) {
                if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(GP)) {
                    if (BO->isAssignmentOp() && BO->getLHS()->IgnoreParenImpCasts() == ASE)
                        is_write = true;
                }
            }
            access_list.push_back({is_write ? PointerAccessKind::SubscriptWrite
                                            : PointerAccessKind::Subscript,
                                   ASE->getBeginLoc(), DRE, nullptr,
                                   "", "", sub_text, ""});
            return;
        }
    }

    // ---- BinaryOperator: comparisons, assignments, compound assigns,
    //                       pointer arithmetic --------------------------
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(Parent)) {
        // Comparison: p == NULL, p < end, p < arr + n, p >= arr, ...
        // We try several shapes in order of specificity, falling back
        // to the unresolvable "Comparison" kind if none of them apply.
        if (BO->isComparisonOp()) {
            // Figure out which operand is our pointer and normalize the
            // operator so the pointer is conceptually on the LHS.
            const Expr *OtherSide;
            bool ptr_is_lhs;
            if (BO->getLHS()->IgnoreParenImpCasts() == DRE ||
                BO->getLHS()->IgnoreParenImpCasts() == OutermostDRE) {
                OtherSide = BO->getRHS()->IgnoreParenImpCasts();
                ptr_is_lhs = true;
            } else {
                OtherSide = BO->getLHS()->IgnoreParenImpCasts();
                ptr_is_lhs = false;
            }

            std::string op_text;
            switch (BO->getOpcode()) {
            case BO_LT:  op_text = ptr_is_lhs ? "<"  : ">";  break;
            case BO_GT:  op_text = ptr_is_lhs ? ">"  : "<";  break;
            case BO_LE:  op_text = ptr_is_lhs ? "<=" : ">="; break;
            case BO_GE:  op_text = ptr_is_lhs ? ">=" : "<="; break;
            case BO_EQ:  op_text = "=="; break;
            case BO_NE:  op_text = "!="; break;
            default:     op_text = "??"; break;
            }

            // Shape 1: p ?= NULL
            if (isNullExpr(OtherSide)) {
                access_list.push_back({PointerAccessKind::ComparisonNull,
                                       BO->getBeginLoc(), DRE, nullptr,
                                       op_text, "", "", "-1"});
                return;
            }

            // Shape 2: p ?= arr + n  →  index ?= n
            if (const BinaryOperator *AddBO = dyn_cast<BinaryOperator>(OtherSide)) {
                if (AddBO->getOpcode() == BO_Add) {
                    const Expr *AddLHS = AddBO->getLHS()->IgnoreParenImpCasts();
                    if (AddLHS->getType()->isPointerType() || AddLHS->getType()->isArrayType()) {
                        std::string offset = getSourceText(AddBO->getRHS(), SM, LO);
                        access_list.push_back({PointerAccessKind::ComparisonExpr,
                                               BO->getBeginLoc(), DRE, nullptr,
                                               op_text, "", "", offset});
                        return;
                    }
                }
            }

            // Shape 3 / 4: comparing to another pointer expression.
            //   3) the same base array         →  index ?= 0
            //   4) some other pointer `end`    →  index ?= (end - base)
            if (OtherSide->getType()->isPointerType() || OtherSide->getType()->isArrayType()) {
                std::string other_text = getSourceText(OtherSide, SM, LO);
                if (other_text == candidate.base_array_text) {
                    access_list.push_back({PointerAccessKind::ComparisonExpr,
                                           BO->getBeginLoc(), DRE, nullptr,
                                           op_text, "", "", "0"});
                    return;
                }

                if (!candidate.base_array_text.empty()) {
                    std::string resolved = "(" + other_text + " - " +
                                           candidate.base_array_text + ")";
                    access_list.push_back({PointerAccessKind::ComparisonExpr,
                                           BO->getBeginLoc(), DRE, nullptr,
                                           op_text, "", "", resolved});
                    return;
                }
            }

            // Shape 5: a parameter pointer with no base yet (still
            // generic). Rebuild the pointer at the call as
            // `(base + index)` once detection picks a base later.
            if (candidate.is_parameter &&
                OtherSide->getType()->isPointerType()) {
                std::string param_name = candidate.ptr_var->getNameAsString();
                std::string other_text = getSourceText(OtherSide, SM, LO);
                access_list.push_back({PointerAccessKind::ComparisonExpr,
                                       BO->getBeginLoc(), DRE, nullptr,
                                       op_text, param_name, "", other_text});
                return;
            }

            // Unresolvable — validation will reject.
            access_list.push_back({PointerAccessKind::Comparison,
                                   BO->getBeginLoc(), DRE, nullptr,
                                   "", "", "", ""});
            return;
        }

        // Plain assignment: p = ...
        // Mirrors analyzePointerInit but for assignments after the
        // declaration, and additionally enforces base-array consistency:
        // a second base that doesn't match the first → Unknown (reject).
        if (BO->getOpcode() == BO_Assign) {
            if (BO->getLHS()->IgnoreParenImpCasts() == DRE ||
                BO->getLHS()->IgnoreParenImpCasts() == OutermostDRE) {
                // Reject `(p = &arr[i])->field`: after rewriting the
                // assignment becomes an int, and `->` no longer applies.
                const Stmt *AssignParent = skipTransparentParents(BO, Ctx);
                if (AssignParent) {
                    if (const MemberExpr *ME = dyn_cast<MemberExpr>(AssignParent)) {
                        if (ME->isArrow()) {
                            access_list.push_back({PointerAccessKind::Unknown,
                                                   BO->getBeginLoc(), DRE, nullptr,
                                                   "", "", "", ""});
                            return;
                        }
                    }
                }
                const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
                while (const auto *CSCE = dyn_cast<CStyleCastExpr>(RHS))
                    RHS = CSCE->getSubExpr()->IgnoreParenImpCasts();

                // p = NULL
                if (isNullExpr(RHS)) {
                    access_list.push_back({PointerAccessKind::AssignNull,
                                           BO->getBeginLoc(), DRE, nullptr,
                                           "", "", "", ""});
                    return;
                }

                bool is_global = PtrVar && PtrVar->hasGlobalStorage();

                // p = &arr[i]
                std::string base_text, index_text;
                if (isAddrOfSubscript(RHS, base_text, index_text)) {
                    const auto *UO = cast<UnaryOperator>(RHS);
                    const auto *ASE = cast<ArraySubscriptExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
                    if (baseIsUnsafe(ASE->getBase()->IgnoreParenImpCasts(), is_global) ||
                        baseIsUnsafe(ASE->getIdx(), is_global)) {
                        access_list.push_back({PointerAccessKind::Unknown,
                                               BO->getBeginLoc(), DRE, nullptr,
                                               "", "", "", ""});
                        return;
                    }
                    if (candidate.base_array_text.empty()) {
                        candidate.base_array_text = base_text;
                        candidate.base_array = RHS;
                    } else if (base_text != candidate.base_array_text) {
                        access_list.push_back({PointerAccessKind::Unknown,
                                               BO->getBeginLoc(), DRE, nullptr,
                                               "", "", "", ""});
                        return;
                    }
                    access_list.push_back({PointerAccessKind::AssignAddrOf,
                                           BO->getBeginLoc(), DRE, nullptr,
                                           index_text, "", "", ""});
                    return;
                }

                // p = arr + offset
                if (const BinaryOperator *AddBO = dyn_cast<BinaryOperator>(RHS)) {
                    if (AddBO->getOpcode() == BO_Add &&
                        AddBO->getLHS()->IgnoreParenImpCasts()->getType()->isPointerType()) {
                        const Expr *AddLHS = AddBO->getLHS()->IgnoreParenImpCasts();
                        if (baseIsUnsafe(AddLHS, is_global) ||
                            baseIsUnsafe(AddBO->getRHS(), is_global)) {
                            access_list.push_back({PointerAccessKind::Unknown,
                                                   BO->getBeginLoc(), DRE, nullptr,
                                                   "", "", "", ""});
                            return;
                        }
                        std::string rhs_base = getSourceText(AddLHS, SM, LO);
                        std::string rhs_offset = getSourceText(AddBO->getRHS(), SM, LO);
                        if (candidate.base_array_text.empty()) {
                            candidate.base_array_text = rhs_base;
                            candidate.base_array = AddLHS;
                        } else if (rhs_base != candidate.base_array_text) {
                            access_list.push_back({PointerAccessKind::Unknown,
                                                   BO->getBeginLoc(), DRE, nullptr,
                                                   "", "", "", ""});
                            return;
                        }
                        access_list.push_back({PointerAccessKind::AssignArrayOffset,
                                               BO->getBeginLoc(), DRE, nullptr,
                                               rhs_offset, "", "", ""});
                        return;
                    }
                }

                // p = strchr(...) — record so we can emit a wrapper. We
                // also collect the non-pointer arguments into operand_text
                // for the rewriter. The wrapper takes (base, start, c, ...),
                // so skip both the tracked pointer (passed as `start`) and
                // any arg matching the candidate's base (passed as `base`)
                // — otherwise the rewritten call ends up with a duplicated
                // base argument that fails to compile.
                if (const CallExpr *RhsCall = dyn_cast<CallExpr>(RHS)) {
                    if (const FunctionDecl *Callee = RhsCall->getDirectCallee()) {
                        std::string func_name = Callee->getNameAsString();
                        if (g_allowed_funcs.count(func_name) &&
                            RhsCall->getType()->isPointerType()) {
                            std::string other_args;
                            for (unsigned i = 0; i < RhsCall->getNumArgs(); i++) {
                                const Expr *Arg = RhsCall->getArg(i)->IgnoreParenImpCasts();
                                if (const DeclRefExpr *ArgDRE = dyn_cast<DeclRefExpr>(Arg)) {
                                    if (ArgDRE->getDecl() == PtrVar)
                                        continue;  // the pointer is the wrapper's `start` param
                                    // The base may be passed as the first strchr arg
                                    // (e.g. Lua's `l = strchr(path, sep)` where `path`
                                    // is l's base). It's already covered by the wrapper's
                                    // `base` param — skip it here too.
                                    if (const VarDecl *ArgVD = dyn_cast<VarDecl>(ArgDRE->getDecl())) {
                                        if (ArgVD->getNameAsString() == candidate.base_array_text)
                                            continue;
                                    }
                                }
                                // Fallback: source-text match against base, in case the
                                // base isn't a bare DRE (e.g. `arr.field` or a cast).
                                std::string arg_text = getSourceText(RhsCall->getArg(i), SM, LO);
                                if (!candidate.base_array_text.empty() &&
                                    arg_text == candidate.base_array_text)
                                    continue;
                                if (!other_args.empty()) other_args += ", ";
                                other_args += arg_text;
                            }
                            access_list.push_back({PointerAccessKind::AssignFromAllowedFunc,
                                                   BO->getBeginLoc(), DRE, RhsCall,
                                                   func_name, "", "", other_args});
                            return;
                        }
                    }
                }

                // p = arr (or another pointer/array expression that
                // matches the existing base).
                if (RHS->getType()->isPointerType() || RHS->getType()->isArrayType()) {
                    if (baseIsUnsafe(RHS, is_global)) {
                        // RHS has a side effect (e.g. `p = argv[n++]`) or,
                        // for a global pointer, references a local — pasting
                        // it at every access site would duplicate the side
                        // effect or paste an out-of-scope name.
                        access_list.push_back({PointerAccessKind::Unknown,
                                               BO->getBeginLoc(), DRE, nullptr,
                                               "", "", "", ""});
                        return;
                    }
                    std::string rhs_text = getSourceText(RHS, SM, LO);
                    if (candidate.base_array_text.empty()) {
                        candidate.base_array_text = rhs_text;
                        candidate.base_array = RHS;
                    } else if (rhs_text != candidate.base_array_text) {
                        // Different source — the pointer is being reseated
                        // (e.g. a linked-list walk like `p = p->next`).
                        // We can't represent that as a single index.
                        access_list.push_back({PointerAccessKind::Unknown,
                                               BO->getBeginLoc(), DRE, nullptr,
                                               "", "", "", ""});
                        return;
                    }
                    access_list.push_back({PointerAccessKind::AssignArray,
                                           BO->getBeginLoc(), DRE, nullptr,
                                           "", "", "", ""});
                    return;
                }

                // Anything else on the RHS — give up.
                access_list.push_back({PointerAccessKind::Unknown,
                                       BO->getBeginLoc(), DRE, nullptr,
                                       "", "", "", ""});
                return;
            }
        }

        // p += n / p -= n
        if (BO->getOpcode() == BO_AddAssign &&
            (BO->getLHS()->IgnoreParenImpCasts() == DRE ||
             BO->getLHS()->IgnoreParenImpCasts() == OutermostDRE)) {
            std::string operand = getSourceText(BO->getRHS(), SM, LO);
            access_list.push_back({PointerAccessKind::PlusAssign,
                                   BO->getBeginLoc(), DRE, nullptr,
                                   "", "", "", operand});
            return;
        }
        if (BO->getOpcode() == BO_SubAssign &&
            (BO->getLHS()->IgnoreParenImpCasts() == DRE ||
             BO->getLHS()->IgnoreParenImpCasts() == OutermostDRE)) {
            std::string operand = getSourceText(BO->getRHS(), SM, LO);
            access_list.push_back({PointerAccessKind::MinusAssign,
                                   BO->getBeginLoc(), DRE, nullptr,
                                   "", "", "", operand});
            return;
        }

        // Pointer + expression — could be dereferenced (*(p + n)),
        // passed to a function (func(p + n)), or just escape. Walk up
        // through nested +/- until we hit a non-arithmetic parent.
        if ((BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub) &&
            (BO->getLHS()->IgnoreParenImpCasts() == DRE ||
             BO->getLHS()->IgnoreParenImpCasts() == OutermostDRE)) {
            const Stmt *Current = BO;
            while (true) {
                const Stmt *Up = skipTransparentParents(Current, Ctx);
                if (!Up) break;

                // *(p ± expr) — dereference of a pointer-arithmetic chain.
                if (const UnaryOperator *DerefUO = dyn_cast<UnaryOperator>(Up)) {
                    if (DerefUO->getOpcode() == UO_Deref) {
                        // Pull out the offset text by stripping the
                        // pointer name from the start of the arithmetic
                        // expression. Using the outermost BO (`Current`)
                        // avoids parens dropped by IgnoreParenImpCasts.
                        std::string arith_text = getSourceText(
                            cast<Expr>(Current)->getSourceRange(), SM, LO);
                        std::string ptr_text = getSourceText(DRE, SM, LO);
                        std::string offset_str;
                        size_t pos = arith_text.find(ptr_text);
                        if (pos != std::string::npos)
                            offset_str = arith_text.substr(pos + ptr_text.length());

                        // Try to evaluate the entire +/- chain as a
                        // compile-time constant. If we can, fold it
                        // into the candidate's min/max offset bounds
                        // (used later by RustSlice lookback/lookahead).
                        // If any term is non-constant, mark the
                        // candidate as having a variable offset, which
                        // disqualifies it during validation.
                        bool is_const_offset = true;
                        int const_offset = 0;

                        const BinaryOperator *CurBO = BO;
                        const Stmt *CurNode = BO;
                        while (CurBO) {
                            const Expr *RHS = CurBO->getRHS()->IgnoreParenImpCasts();
                            Expr::EvalResult evalResult;
                            if (RHS->EvaluateAsInt(evalResult, Ctx)) {
                                int ival = (int)evalResult.Val.getInt().getExtValue();
                                if (CurBO->getOpcode() == BO_Add)
                                    const_offset += ival;
                                else if (CurBO->getOpcode() == BO_Sub)
                                    const_offset -= ival;
                            } else {
                                is_const_offset = false;
                            }
                            const Stmt *NextUp = skipTransparentParents(CurNode, Ctx);
                            CurBO = NextUp ? dyn_cast<BinaryOperator>(NextUp) : nullptr;
                            if (CurBO && (CurBO->getOpcode() == BO_Add ||
                                          CurBO->getOpcode() == BO_Sub))
                                CurNode = CurBO;
                            else
                                CurBO = nullptr;
                        }

                        if (is_const_offset) {
                            if (const_offset < candidate.min_relative_offset)
                                candidate.min_relative_offset = const_offset;
                            if (const_offset > candidate.max_relative_offset)
                                candidate.max_relative_offset = const_offset;
                        } else {
                            candidate.constant_offsets = false;
                        }

                        // Read or write of *(p ± expr)?
                        const Stmt *DerefParent = skipTransparentParents(DerefUO, Ctx);
                        bool is_write = false;
                        if (DerefParent) {
                            if (const BinaryOperator *AssignBO =
                                    dyn_cast<BinaryOperator>(DerefParent)) {
                                if (AssignBO->isAssignmentOp() &&
                                    AssignBO->getLHS()->IgnoreParenImpCasts() == DerefUO)
                                    is_write = true;
                            }
                        }

                        access_list.push_back(
                            {is_write ? PointerAccessKind::DerefOffsetWrite
                                      : PointerAccessKind::DerefOffset,
                             DerefUO->getBeginLoc(), DRE, DerefUO,
                             offset_str, "", "", ""});
                        return;
                    }
                    break;
                }

                // Keep walking through chained additions/subtractions.
                if (const BinaryOperator *UpBO = dyn_cast<BinaryOperator>(Up)) {
                    if (UpBO->getOpcode() == BO_Add || UpBO->getOpcode() == BO_Sub) {
                        Current = UpBO;
                        continue;
                    }
                }

                // Pointer arithmetic that ends up as a function argument,
                // e.g. `func(p + 1)`. Same handling as the bare pointer
                // case below — recorded as PassedTo[Allowed]Func.
                if (const CallExpr *CE = dyn_cast<CallExpr>(Up)) {
                    if (const FunctionDecl *Callee = CE->getDirectCallee()) {
                        std::string func_name = Callee->getNameAsString();
                        if (g_allowed_funcs.count(func_name)) {
                            access_list.push_back({PointerAccessKind::PassedToAllowedFunc,
                                                   DRE->getLocation(), DRE, CE,
                                                   func_name, "", "", ""});
                            return;
                        }
                    }
                    access_list.push_back({PointerAccessKind::PassedToFunc,
                                           DRE->getLocation(), DRE, CE,
                                           "", "", "", ""});
                    return;
                }
                break;
            }
        }
    }

    // ---- Logical context: p && q, p || q -------------------------------
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(Parent)) {
        if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr) {
            access_list.push_back({PointerAccessKind::BoolTrue,
                                   DRE->getLocation(), DRE, nullptr,
                                   "", "", "", ""});
            return;
        }
    }

    // ---- Boolean context: if/while/for/do/?: condition -----------------
    if (isa<IfStmt>(Parent) || isa<WhileStmt>(Parent) ||
        isa<ForStmt>(Parent) || isa<DoStmt>(Parent) ||
        isa<ConditionalOperator>(Parent)) {
        access_list.push_back({PointerAccessKind::BoolTrue,
                               DRE->getLocation(), DRE, nullptr,
                               "", "", "", ""});
        return;
    }

    // ---- Direct call argument: func(p) ---------------------------------
    if (const CallExpr *CE = dyn_cast<CallExpr>(Parent)) {
        if (const FunctionDecl *Callee = CE->getDirectCallee()) {
            std::string func_name = Callee->getNameAsString();
            if (g_allowed_funcs.count(func_name)) {
                access_list.push_back({PointerAccessKind::PassedToAllowedFunc,
                                       DRE->getLocation(), DRE, CE,
                                       func_name, "", "", ""});
                return;
            }
        }
        access_list.push_back({PointerAccessKind::PassedToFunc,
                               DRE->getLocation(), DRE, CE,
                               "", "", "", ""});
        return;
    }

    // ---- return p ------------------------------------------------------
    if (isa<ReturnStmt>(Parent)) {
        access_list.push_back({PointerAccessKind::ReturnPtr,
                               DRE->getLocation(), DRE, Parent,
                               "", "", "", ""});
        return;
    }

    // ---- Anything we don't recognize -> reject -------------------------
    if (VERBOSE)
        llvm::outs() << "[Collect] Unknown access to " << PtrVar->getNameAsString()
                      << " at " << DRE->getLocation().printToString(SM) << "\n";
    access_list.push_back({PointerAccessKind::Unknown, DRE->getLocation(),
                           DRE, nullptr, "", "", "", ""});
}
