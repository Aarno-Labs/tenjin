// PointerAccessCollector.cpp — see PointerAccessCollector.h for the
// high-level role. This file is mostly classifyAccess(), a pattern-match
// over the syntactic context of each pointer reference.
//
// Every reference lands in one of four buckets:
//
//   element access   the index selects an element of the base
//   position         the index moves; the base does not
//   (base, index)    an assignment, split into a root and an offset
//   value read       the pointer is rebuilt in place as (p + p_index_xj)
//
// The last bucket is the fallback, and it is why the rewrite is total.

#include "PointerAccessCollector.h"

PointerAccessCollector::PointerAccessCollector(ASTContext &Ctx)
    : Ctx(Ctx), SM(Ctx.getSourceManager()) {}

bool PointerAccessCollector::isTracked(const Decl *D) const
{
    const auto *VD = dyn_cast_or_null<VarDecl>(D);
    return VD && tracked_pointers.count(VD) != 0;
}

// True if `E` is one of the recognized null-pointer spellings: a 0
// literal, the GNU __null builtin, or a cast wrapping one of those
// (e.g. ((void*)0)).
bool PointerAccessCollector::isNullExpr(const Expr *E)
{
    E = E->IgnoreParenImpCasts();
    if (const IntegerLiteral *IL = dyn_cast<IntegerLiteral>(E))
        return IL->getValue() == 0;
    if (isa<GNUNullExpr>(E))
        return true;
    if (const CStyleCastExpr *CE = dyn_cast<CStyleCastExpr>(E))
        return isNullExpr(CE->getSubExpr());
    return false;
}

// The region an allowlisted search runs over: argument 0 for every function
// in the allowlist, and what the wrapper takes as its `base`.
//
// It has to be a bare variable. The rewrite names the region twice — once to
// reseat the assigned pointer, once as the wrapper's base — so an argument
// with side effects could not be duplicated safely.
static const DeclRefExpr *searchBaseArg(const CallExpr *CE)
{
    if (!CE || CE->getNumArgs() == 0)
        return nullptr;
    const auto *DRE = dyn_cast<DeclRefExpr>(CE->getArg(0)->IgnoreParenImpCasts());
    return DRE && isa<VarDecl>(DRE->getDecl()) ? DRE : nullptr;
}

// True when `CE` is an allowlisted search whose result is an offset into the
// region named by its first argument.
static bool isAllowedSearch(const CallExpr *CE)
{
    if (!CE || !CE->getType()->isPointerType())
        return false;
    const FunctionDecl *Callee = CE->getDirectCallee();
    if (!Callee || !g_allowed_funcs.count(Callee->getNameAsString()))
        return false;
    return searchBaseArg(CE) != nullptr;
}

// What a pointer or an array steps over: the pointee, or the element.
static QualType elementTypeOf(QualType T, ASTContext &Ctx)
{
    if (T->isPointerType())
        return T->getPointeeType();
    if (const auto *AT = Ctx.getAsArrayType(T))
        return AT->getElementType();
    return QualType();
}

// True when `Root` and `Owner` step over the same type, so that an offset
// counted from one lands at the same address counted from the other.
//
// An implicit conversion does not keep that: `void *raw = v + 1` with an
// `int *v` is four bytes on, and `raw_index_xj = 1` is one.
static bool stepsAlike(const VarDecl *Owner, const Expr *Root, ASTContext &Ctx)
{
    QualType OwnerElem = elementTypeOf(Owner->getType(), Ctx);
    QualType RootElem = elementTypeOf(Root->getType(), Ctx);
    return !OwnerElem.isNull() && !RootElem.isNull() &&
           Ctx.hasSameUnqualifiedType(OwnerElem, RootElem);
}

// ============================================================================
// Parent walking
// ============================================================================

// Step up through ImplicitCastExpr / ParenExpr parents and return the
// first "real" parent, which may be a Decl: an initializer's parent is the
// VarDecl that owns it. Also reports `outermost` — the topmost transparent
// wrapper around `S` itself — so callers comparing AST nodes can match
// either the bare node or its wrapped form.
static bool consumerOf(const Stmt *S, ASTContext &Ctx,
                       const Stmt *&outermost, DynTypedNode &out)
{
    outermost = S;
    const Stmt *Current = S;
    while (true)
    {
        auto Parents = Ctx.getParents(*Current);
        if (Parents.empty())
            return false;
        const Stmt *P = Parents[0].get<Stmt>();
        if (P && (isa<ImplicitCastExpr>(P) || isa<ParenExpr>(P)))
        {
            outermost = P;
            Current = P;
            continue;
        }
        out = Parents[0];
        return true;
    }
}

// ============================================================================
// Splitting an assigned pointer value into (root, offset)
// ============================================================================
//
// Three shapes split, and all three are decided by looking at the
// expression alone:
//
//   q              root q,   offset ""          index := q's index, or 0
//   q + 1 - k      root q,   offset " + 1 - k"
//   &arr[i]        root arr, offset " + i"
//
// Anything else is taken whole: the right-hand side becomes the new base
// verbatim and the index starts at 0. That fallback is always correct,
// which is what makes the rewrite total — splitting only ever improves
// what the base *is*, it is never required for the rewrite to be sound.
//
// The offset terms are recorded as expressions, not as text. They are
// lifted out of the right-hand side and re-emitted in the index, and a
// term can read through a pointer this pass is rewriting — so where they
// land they are rendered by the edit plan, exactly like the offset of an
// element access.
//
// A root that is itself a rewritten pointer is *paired*: `p = q + 1` becomes
// `p = q` with `p_index_xj = q_index_xj + 1`, so p shares q's base instead of
// starting a new one inside it. Whether q is rewritten is not known here, so
// the root is only recorded; the edit plan decides (EditPlan::pairedRoot).
// The root's own reference is classified like any other in the meantime.

bool PointerAccessCollector::escapesForInitScope(const Stmt *S,
                                                 const VarDecl *Owner)
{
    if (!S || !Owner)
        return false;
    const DeclStmt *DS = declStmtOf(Owner, Ctx);
    if (!DS || !forStmtInitializedBy(DS, Ctx))
        return false;
    std::set<const Decl *> bound(DS->decl_begin(), DS->decl_end());
    return referencesAnyOf(S, bound);
}

bool PointerAccessCollector::crossesSibling(const PointerSplit &split,
                                            const VarDecl *Owner)
{
    const DeclStmt *DS = declStmtOf(Owner, Ctx);
    if (!DS || DS->isSingleDecl())
        return false;

    bool terms_have_effects = false;
    for (const OffsetTerm &t : split.terms)
        terms_have_effects |= t.expr->HasSideEffects(Ctx);
    const auto *RootDRE = dyn_cast<DeclRefExpr>(split.base);
    const Decl *Stepped =
        RootDRE && split.step != IndexStep::None ? RootDRE->getDecl() : nullptr;

    // Hoisted ahead of the loop, the initializer crosses the declarators
    // before its own; placed after the statement, the ones after.
    bool crosses_earlier = forStmtInitializedBy(DS, Ctx) != nullptr;
    bool earlier = true;
    for (const Decl *D : DS->decls())
    {
        if (D == Owner)
        {
            earlier = false;
            continue;
        }
        const auto *Sibling = dyn_cast<VarDecl>(D);
        const Expr *Init = Sibling ? Sibling->getInit() : nullptr;
        if (!Init || earlier != crosses_earlier)
            continue;
        if (terms_have_effects || Init->HasSideEffects(Ctx))
            return true;
        if (Stepped && referencesAnyOf(Init, {Stepped}))
            return true;
    }
    return false;
}

// The step a `++` or `--` applies.
static IndexStep stepOf(const UnaryOperator *UO)
{
    switch (UO->getOpcode())
    {
    case UO_PostInc:
        return IndexStep::PostInc;
    case UO_PreInc:
        return IndexStep::PreInc;
    case UO_PostDec:
        return IndexStep::PostDec;
    case UO_PreDec:
        return IndexStep::PreDec;
    default:
        return IndexStep::None;
    }
}

bool PointerAccessCollector::decomposePointer(const Expr *E, PointerSplit &out)
{
    if (!E)
        return false;
    const Expr *Core = E->IgnoreParenImpCasts();

    // q — the expression is its own base, at offset 0. Still a decomposition
    // and not a refusal: the root is recorded and paired, so `int *q = p;`
    // inherits p's position, even though no text moves.
    if (const auto *DRE = dyn_cast<DeclRefExpr>(Core))
    {
        QualType T = DRE->getType();
        if (!T->isPointerType() && !T->isArrayType())
            return false;
        out.base = DRE;
        return true;
    }

    if (const auto *UO = dyn_cast<UnaryOperator>(Core))
    {
        // q = p++ — the base is p, and what q lands at is p's position and
        // the step together. Only a tracked p can have an index to step; for
        // an untracked one there is nothing to carry the increment, and
        // taking the split anyway would drop it on the floor. Tracked is not
        // yet rewritten, so the edit plan uses this split only if p turns out
        // to be (EditPlan::splitStands).
        if (UO->isIncrementDecrementOp())
        {
            const auto *OpDRE =
                dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
            if (!OpDRE || !OpDRE->getType()->isPointerType())
                return false;
            if (!isTracked(OpDRE->getDecl()))
                return false;
            out.step = stepOf(UO);
            out.base = OpDRE;
            return true;
        }

        // &arr[i] — the address of an element already names its own base.
        // A tracked base is excluded: its reference is rewritten to
        // arr[arr_index_xj + i] in place, and replacing the whole
        // right-hand side would drop that edit on the floor.
        if (UO->getOpcode() != UO_AddrOf)
            return false;
        const auto *ASE =
            dyn_cast<ArraySubscriptExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
        if (!ASE)
            return false;
        const auto *BaseDRE =
            dyn_cast<DeclRefExpr>(ASE->getBase()->IgnoreParenImpCasts());
        if (!BaseDRE || isTracked(BaseDRE->getDecl()))
            return false;
        out.base = BaseDRE;
        out.terms.push_back({ASE->getIdx(), /*minus=*/false});
        return true;
    }

    // e ± k — decompose the pointer-valued side and keep the rest as a term.
    // Recursing rather than walking the spine is what lets `q = p++ + 1`
    // split with no rule of its own. Terms accumulate on the way back up, so
    // they come out in source order without a reversal.
    //
    // A C-style cast is deliberately not stepped through, here or above:
    // dropping it would change the assignment's type, and the cast operand
    // is reachable as an ordinary value read instead.
    if (const auto *BO = dyn_cast<BinaryOperator>(Core))
    {
        if (BO->getOpcode() != BO_Add && BO->getOpcode() != BO_Sub)
            return false;
        if (!decomposePointer(BO->getLHS(), out))
            return false;
        out.terms.push_back({BO->getRHS(), BO->getOpcode() == BO_Sub});
        return true;
    }

    return false;
}

void PointerAccessCollector::splitAssignedValue(const Expr *RHS,
                                                PointerAccess &pa,
                                                const VarDecl *Owner)
{
    pa.rhs_expr = RHS;
    pa.root_expr = nullptr;
    pa.step = IndexStep::None;
    pa.index_terms.clear();

    if (!RHS)
        return;

    PointerSplit split;
    if (!decomposePointer(RHS, split))
        return;

    // An assignment sets the index in the same expression as the pointer,
    // so the offset is evaluated where it was written. An initializer's
    // index gets a declaration of its own, and the offset moves there with
    // it. The two checks that follow are about that move.
    bool rhs_is_initializer = Owner && RHS == Owner->getInit();

    // An offset that names another tracked pointer is fine: the term is an
    // expression, and wherever it lands the edit plan rewrites what is
    // inside it. Scope is the one thing rendering cannot fix — an index
    // hoisted out of a for-init sits *before* the loop, where the names
    // that same for-init binds do not exist yet.
    //
    // Declining is always safe. The right-hand side is then kept whole and
    // the index starts at zero, which is the same pointer by a different
    // route — and any tracked pointer inside it is rewritten in place as an
    // ordinary value read.
    if (rhs_is_initializer)
        for (const OffsetTerm &t : split.terms)
            if (escapesForInitScope(t.expr, Owner))
                return;

    // Nor can it fix order: the index is initialized in its own
    // declaration, which is not where the pointer's initializer was
    // evaluated.
    if (rhs_is_initializer && crossesSibling(split, Owner))
        return;

    // The index counts the owner's elements, and the offset was written in
    // the root's.
    if (Owner && !stepsAlike(Owner, split.base, Ctx))
        return;

    pa.root_expr = split.base;
    if (pa.isSplit())
    {
        pa.step = split.step;
        pa.index_terms = std::move(split.terms);
    }
}

// ============================================================================
// Collection
// ============================================================================

// Pick up a pointer-typed VarDecl — a local or a parameter during
// traversal, a file-scope pointer when FunctionAccessAnalyzer hands one
// over — and record it in `tracked_pointers`. A pointer with an initializer
// also gets an Init or InitNull access carrying the (root, offset) split of
// that initializer.
bool PointerAccessCollector::VisitVarDecl(VarDecl *VD)
{
    if (!VD->getType()->isPointerType())
        return true;
    if (SM.isInSystemHeader(VD->getLocation()))
        return true;
    // A block-scope `extern T *p;` declares no pointer of its own. It names
    // one at file scope, and that is where the pointer is considered.
    if (VD->hasExternalStorage())
        return true;

    std::vector<PointerAccess> access_list;
    if (VD->hasInit())
    {
        PointerAccess pa;
        pa.kind = isNullExpr(VD->getInit()) ? PointerAccessKind::InitNull
                                            : PointerAccessKind::Init;
        pa.loc = VD->getInit()->getBeginLoc();
        pa.expr = VD->getInit();
        splitAssignedValue(VD->getInit(), pa, VD);
        access_list.push_back(pa);
    }

    tracked_pointers.insert(VD);
    accesses[VD] = access_list;

    if (VERBOSE)
        llvm::outs() << "[Collect] Tracking pointer: " << VD->getNameAsString()
                     << (isa<ParmVarDecl>(VD)     ? " (parameter)"
                         : VD->hasGlobalStorage() ? " (global)"
                                                  : " (local)")
                     << "\n";

    return true;
}

// Every reference to a tracked pointer flows through here. We skip the
// reference inside the pointer's own initializer (already handled in
// VisitVarDecl) and forward everything else to classifyAccess.
bool PointerAccessCollector::VisitDeclRefExpr(DeclRefExpr *DRE)
{
    const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (!VD)
        return true;

    if (!tracked_pointers.count(VD))
        return true;

    if (VD->hasInit())
    {
        SourceRange initRange = VD->getInit()->getSourceRange();
        if (SM.isBeforeInTranslationUnit(DRE->getLocation(), initRange.getEnd()) &&
            !SM.isBeforeInTranslationUnit(DRE->getLocation(), initRange.getBegin()))
        {
            return true;
        }
    }

    classifyAccess(DRE, VD, accesses[VD]);
    return true;
}

// ============================================================================
// classifyAccess
// ============================================================================

void PointerAccessCollector::classifyAccess(DeclRefExpr *DRE,
                                            const VarDecl *PtrVar,
                                            std::vector<PointerAccess> &access_list)
{
    auto emit = [&](PointerAccessKind kind, SourceLocation loc,
                    const Stmt *enclosing = nullptr) -> PointerAccess &
    {
        PointerAccess pa;
        pa.kind = kind;
        pa.loc = loc;
        pa.expr = DRE;
        pa.enclosing_stmt = enclosing;
        access_list.push_back(pa);
        return access_list.back();
    };

    // The value read, and what makes the classification total: whatever
    // the surrounding expression is, `(p + p_index_xj)` is the pointer it
    // saw before.
    auto emitValueUse = [&]()
    { emit(PointerAccessKind::ValueUse, DRE->getLocation()); };

    // The consumer may be a Decl rather than a Stmt — an initializer's
    // parent is the variable it initializes — so `Parent` is null for a
    // shape that is perfectly ordinary, not for one that is unrecognized.
    const Stmt *OutermostDRE = DRE;
    DynTypedNode Consumer;
    if (!consumerOf(DRE, Ctx, OutermostDRE, Consumer))
    {
        emit(PointerAccessKind::Unknown, DRE->getLocation());
        return;
    }
    const Stmt *Parent = Consumer.get<Stmt>();

    auto isSelf = [&](const Expr *E)
    {
        return E && (E->IgnoreParenImpCasts() == DRE ||
                     E->IgnoreParenImpCasts() == OutermostDRE);
    };

    // ---- &p — the pointer's storage is observable ------------------------
    if (const auto *UO = dyn_cast_or_null<UnaryOperator>(Parent))
    {
        if (UO->getOpcode() == UO_AddrOf)
        {
            emit(PointerAccessKind::AddressOf, UO->getBeginLoc());
            return;
        }
    }

    // ---- sizeof p — the value is never read ------------------------------
    if (Parent && isa<UnaryExprOrTypeTraitExpr>(Parent))
    {
        emit(PointerAccessKind::NoEdit, DRE->getLocation());
        return;
    }

    // A reference that is the root of another pointer's assignment gets no
    // kind of its own. It is classified below as what it is by itself — a
    // read, or a move for `q = p++` — and the edit plan drops that rewrite
    // if it pairs the two indices instead.

    // An initializer's consumer is the variable it initializes, which is not
    // a statement. The reference is a read of the pointer's value.
    if (!Parent)
    {
        emitValueUse();
        return;
    }

    // ---- UnaryOperator: *p, p++, p--, !p ---------------------------------
    if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(Parent))
    {
        switch (UO->getOpcode())
        {
        case UO_Deref:
            emit(PointerAccessKind::Element, UO->getBeginLoc(), UO);
            return;
        case UO_PostInc:
        case UO_PreInc:
        case UO_PostDec:
        case UO_PreDec:
        {
            // Either standalone (`p++`) or the mutation inside `*p++`. The
            // dereferenced form is one edit over the whole `*p++`, and it
            // is valid on either side of an assignment.
            const auto *Deref =
                dyn_cast_or_null<UnaryOperator>(skipTransparentParents(UO, Ctx));
            bool is_element = Deref && Deref->getOpcode() == UO_Deref;
            PointerAccess &pa =
                is_element
                    ? emit(PointerAccessKind::Element, Deref->getBeginLoc(), Deref)
                    : emit(PointerAccessKind::Move, UO->getBeginLoc(), UO);
            pa.step = stepOf(UO);
            return;
        }
        case UO_LNot:
            emit(PointerAccessKind::NullTest, UO->getBeginLoc());
            return;
        default:
            break;
        }
    }

    // ---- MemberExpr: p->field --------------------------------------------
    if (const MemberExpr *ME = dyn_cast<MemberExpr>(Parent))
    {
        if (ME->isArrow())
        {
            // When `field` lives inside an anonymous struct/union, Clang
            // represents `gce->offset` as a chain of MemberExprs sharing
            // one source range: an inner node — always `ME` here, since its
            // base is the tracked pointer — whose member is the anonymous
            // aggregate itself (an unnamed FieldDecl), and one outer node
            // per enclosing anonymous level, ending at the real field.
            // The name to emit is the last node's; the inner ones have
            // none.
            const MemberExpr *RealME = ME;
            const Stmt *Outer = skipTransparentParents(ME, Ctx);
            while (true)
            {
                const auto *AnonFD = dyn_cast<FieldDecl>(RealME->getMemberDecl());
                if (!AnonFD || !AnonFD->isAnonymousStructOrUnion())
                    break;
                const auto *OuterME = dyn_cast_or_null<MemberExpr>(Outer);
                if (!OuterME)
                    break;
                RealME = OuterME;
                Outer = skipTransparentParents(OuterME, Ctx);
            }
            PointerAccess &pa =
                emit(PointerAccessKind::Element, ME->getBeginLoc(), ME);
            pa.field_name = RealME->getMemberDecl()->getNameAsString();
            return;
        }
    }

    // ---- ArraySubscriptExpr: p[i] ----------------------------------------
    if (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(Parent))
    {
        // Only when p is the base of the subscript, not the index.
        if (isSelf(ASE->getBase()) || isSelf(ASE->getLHS()))
        {
            PointerAccess &pa =
                emit(PointerAccessKind::Element, ASE->getBeginLoc(), ASE);
            pa.subscript_expr = ASE->getIdx();
            return;
        }
    }

    // ---- BinaryOperator ---------------------------------------------------
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(Parent))
    {
        // p = strchr(...) — the region is unchanged, so the whole assignment
        // collapses to an index update through a generated wrapper.
        if (BO->getOpcode() == BO_Assign && isSelf(BO->getLHS()))
        {
            // The wrapper hands back an offset in chars from its first
            // argument, so both pointers have to step over chars for it to
            // be this one's index.
            const auto *RhsCall =
                dyn_cast<CallExpr>(BO->getRHS()->IgnoreParenImpCasts());
            if (isAllowedSearch(RhsCall) &&
                PtrVar->getType()->getPointeeType()->isCharType() &&
                stepsAlike(PtrVar, searchBaseArg(RhsCall), Ctx))
            {
                PointerAccess &pa = emit(PointerAccessKind::AssignFromAllowedFunc,
                                         BO->getBeginLoc(), BO);
                pa.root_expr = searchBaseArg(RhsCall);
                return;
            }
        }

        // p = RHS — the (base, index) pair is reassigned together.
        if (BO->getOpcode() == BO_Assign && isSelf(BO->getLHS()))
        {
            PointerAccess &pa =
                emit(isNullExpr(BO->getRHS()) ? PointerAccessKind::AssignNull
                                              : PointerAccessKind::Assign,
                     BO->getBeginLoc(), BO);
            splitAssignedValue(BO->getRHS(), pa, PtrVar);
            return;
        }

        // p += n / p -= n — the index moves.
        if ((BO->getOpcode() == BO_AddAssign || BO->getOpcode() == BO_SubAssign) &&
            isSelf(BO->getLHS()))
        {
            emit(PointerAccessKind::Move, BO->getBeginLoc(), BO);
            return;
        }

        // *(p ± expr) — a dereference of a pointer-arithmetic chain is an
        // element access, so it indexes rather than rebuilding the pointer.
        if ((BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub) &&
            isSelf(BO->getLHS()))
        {
            // Collect the offset terms while climbing, innermost first —
            // which is left-to-right in the source, and so the order they
            // have to be re-emitted in. Each term is kept as its own node,
            // not as a slice of the chain's text: a term may itself contain
            // a reference this pass has to rewrite, and only a node can be
            // addressed by the edit plan.
            //
            // The climb also insists the chain stay on its left spine. The
            // pointer is the leftmost leaf, so `*(k + (p + 1))` reaches this
            // code with `p + 1` as a right operand — a shape that has no
            // offset to lift out, and falls through to the value read.
            const Stmt *Current = BO;
            std::vector<OffsetTerm> terms{{BO->getRHS(), BO->getOpcode() == BO_Sub}};
            while (true)
            {
                const Stmt *Up = skipTransparentParents(Current, Ctx);
                if (!Up)
                    break;
                if (const auto *DerefUO = dyn_cast<UnaryOperator>(Up))
                {
                    if (DerefUO->getOpcode() != UO_Deref)
                        break;

                    PointerAccess &pa = emit(PointerAccessKind::Element,
                                             DerefUO->getBeginLoc(), DerefUO);
                    pa.offset_terms = terms;
                    return;
                }
                if (const auto *UpBO = dyn_cast<BinaryOperator>(Up))
                {
                    if ((UpBO->getOpcode() == BO_Add || UpBO->getOpcode() == BO_Sub) &&
                        UpBO->getLHS()->IgnoreParenImpCasts() == Current)
                    {
                        terms.push_back({UpBO->getRHS(), UpBO->getOpcode() == BO_Sub});
                        Current = UpBO;
                        continue;
                    }
                }
                break;
            }
            emitValueUse();
            return;
        }

        // p == NULL / p != NULL — a null test, which is left as written or
        // rewritten by the pointer's facts (see PointerFacts). Every other
        // comparison is a value read.
        if (BO->isComparisonOp())
        {
            const Expr *Other = isSelf(BO->getLHS()) ? BO->getRHS() : BO->getLHS();
            if ((BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE) &&
                isNullExpr(Other))
            {
                emit(PointerAccessKind::NullTest, BO->getBeginLoc(), BO);
                return;
            }
            emitValueUse();
            return;
        }

        // p && q, p || q — boolean context.
        if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr)
        {
            emit(PointerAccessKind::NullTest, DRE->getLocation());
            return;
        }
    }

    // ---- Boolean context: if/while/for/do/?: condition --------------------
    if (isa<IfStmt>(Parent) || isa<WhileStmt>(Parent) ||
        isa<ForStmt>(Parent) || isa<DoStmt>(Parent))
    {
        emit(PointerAccessKind::NullTest, DRE->getLocation());
        return;
    }
    if (const auto *CO = dyn_cast<ConditionalOperator>(Parent))
    {
        // Only the condition slot is a truth test; the value slots forward
        // the pointer to whatever encloses the `?:`.
        if (OutermostDRE != CO->getTrueExpr() && OutermostDRE != CO->getFalseExpr())
        {
            emit(PointerAccessKind::NullTest, DRE->getLocation());
            return;
        }
    }

    // ---- Everything else is a read of the pointer's value -----------------
    emitValueUse();
}
