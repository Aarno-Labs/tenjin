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
    : Ctx(Ctx), SM(Ctx.getSourceManager()), LO(Ctx.getLangOpts()) {}

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

// True when one of `CE`'s arguments is a bare reference to `PtrVar`.
//
// The wrapper passes the retained pointer as its `base` and the index as
// its `start`, so the result is only an offset from `p`'s own region when
// `p` is what the call searched. `p = strchr(q, c)` lands in q's region and
// falls through to an ordinary assignment, which reseats as before.
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
// The one case where splitting *is* required is a root that is itself a
// tracked pointer. After the rewrite such a root holds its base, not its
// position, so `p = q + 1` left alone would silently reset p to wherever q
// started. Recording the root here is what lets the rewriter pair the
// indices instead.
static bool referencesAnyOf(const Stmt *S, const std::set<const Decl *> &bound)
{
    if (!S)
        return false;
    if (const auto *DRE = dyn_cast<DeclRefExpr>(S))
        if (bound.count(DRE->getDecl()))
            return true;
    for (const Stmt *Child : S->children())
        if (referencesAnyOf(Child, bound))
            return true;
    return false;
}

bool PointerAccessCollector::escapesForInitScope(const Stmt *S,
                                                 const VarDecl *Owner)
{
    if (!S || !Owner)
        return false;
    const DeclStmt *DS = nullptr;
    for (const DynTypedNode &P : Ctx.getParents(*Owner))
        if ((DS = P.get<DeclStmt>()))
            break;
    if (!DS || !forStmtInitializedBy(DS, Ctx))
        return false;
    std::set<const Decl *> bound(DS->decl_begin(), DS->decl_end());
    return referencesAnyOf(S, bound);
}

// A node the decomposition below descends through on its way to the base.
// The climb in pairwiseOwner has to step through exactly these: the two must
// agree about which reference is a root, and neither side reports it when
// they do not.
static bool isSplitTransparent(const Stmt *S)
{
    if (!S)
        return false;
    if (isa<ParenExpr>(S) || isa<ImplicitCastExpr>(S))
        return true;
    if (const auto *UO = dyn_cast<UnaryOperator>(S))
        return UO->isIncrementDecrementOp();
    if (const auto *BO = dyn_cast<BinaryOperator>(S))
        return BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub;
    return false;
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
        out.ok = true;
        return true;
    }

    if (const auto *UO = dyn_cast<UnaryOperator>(Core))
    {
        // q = p++ — the base is p, and what q lands at is p's position and
        // the step together. Only a tracked p has an index to step; for an
        // untracked one there is nothing to carry the increment, and taking
        // the split anyway would drop it on the floor.
        if (UO->isIncrementDecrementOp())
        {
            const auto *OpDRE =
                dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenImpCasts());
            if (!OpDRE || !OpDRE->getType()->isPointerType())
                return false;
            if (!isTracked(OpDRE->getDecl()))
                return false;
            switch (UO->getOpcode())
            {
            case UO_PostInc: out.step = RootAdjust::PostInc; break;
            case UO_PreInc: out.step = RootAdjust::PreInc; break;
            case UO_PostDec: out.step = RootAdjust::PostDec; break;
            case UO_PreDec: out.step = RootAdjust::PreDec; break;
            default: return false;
            }
            out.base = OpDRE;
            out.ok = true;
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
        out.ok = true;
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
                                                const VarDecl *Owner,
                                                bool owner_is_declared_here)
{
    pa.rhs_expr = RHS;
    pa.root_expr = nullptr;
    pa.root_adjust = RootAdjust::None;
    pa.offset_text = "0";
    pa.operand_text = "";
    pa.index_terms.clear();

    if (!RHS)
        return;

    PointerSplit split;
    if (!decomposePointer(RHS, split))
        return;

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
    if (owner_is_declared_here)
        for (const OffsetTerm &t : split.terms)
            if (escapesForInitScope(t.expr, Owner))
                return;

    // Pairing two indices only works when both pointers are rewritten in
    // the same batch: each batch decides which of its members transformed,
    // and a root belonging to the other one would be edited by that batch
    // while this one replaces the text around it. File-scope pointers are
    // their own batch, so a split never straddles the two.
    if (const auto *RootDRE = dyn_cast<DeclRefExpr>(split.base))
    {
        const auto *RootVD = dyn_cast<VarDecl>(RootDRE->getDecl());
        if (RootVD && isTracked(RootVD) && Owner &&
            RootVD->hasGlobalStorage() != Owner->hasGlobalStorage())
            return;
    }

    pa.root_expr = split.base;
    if (split.base != RHS->IgnoreParenImpCasts())
    {
        // Spellings only, for the verbose dump. Rebuilt from the terms rather
        // than sliced out of the whole expression's source text, so a
        // parenthesised root like `(q) + 1` leaves no stray `)` behind.
        std::string remainder;
        for (const OffsetTerm &t : split.terms)
            remainder += (t.minus ? " - " : " + ") + getSourceText(t.expr, SM, LO);
        // `q + 1` counts from 1 on its own; `q - 1` needs the zero to
        // subtract from. A stepped root counts from the step instead.
        std::string standalone =
            split.step != RootAdjust::None
                ? applyRootAdjust(split.step, getSourceText(split.base, SM, LO)) + remainder
            : remainder.compare(0, 3, " + ") == 0 ? remainder.substr(3)
                                                  : "0" + remainder;

        pa.root_adjust = split.step;
        pa.offset_text = standalone;
        pa.operand_text = remainder;
        pa.index_terms = std::move(split.terms);
    }
}

// The mirror of splitAssignedValue, seen from the root's own reference:
// if this reference is the root that some tracked pointer's Init or Assign
// will pair with, that owner is returned and the reference emits no edit.
const VarDecl *PointerAccessCollector::pairwiseOwner(const DeclRefExpr *DRE,
                                                     RootAdjust *step)
{
    if (step)
        *step = RootAdjust::None;

    const Stmt *Outer = DRE;
    DynTypedNode Parent;
    if (!consumerOf(DRE, Ctx, Outer, Parent))
        return nullptr;

    // `q = strchr(p, c)` — the search argument names the region the result
    // lands in, and the assignment's rewrite spells that region itself, so
    // this reference emits no edit. No climbing: the argument is the region
    // exactly when it is the call's first operand.
    if (const auto *CE = dyn_cast_or_null<CallExpr>(Parent.get<Stmt>()))
    {
        if (searchBaseArg(CE) == DRE && isAllowedSearch(CE))
        {
            const Stmt *Up = skipTransparentParents(CE, Ctx);
            if (const auto *BO = dyn_cast_or_null<BinaryOperator>(Up))
            {
                const auto *LHS =
                    dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts());
                if (BO->getOpcode() == BO_Assign && LHS && isTracked(LHS->getDecl()))
                    return cast<VarDecl>(LHS->getDecl());
            }
        }
    }

    // Climb whatever the decomposition descends through, looking for an
    // assignment or an initializer this reference might be the root of, then
    // ask splitAssignedValue whether it actually is. Deciding it with the
    // same function that the rewriter uses is the point: the two must agree,
    // or a reference is either rewritten twice or not at all.
    for (;;)
    {
        const VarDecl *Owner = nullptr;
        const Expr *RHS = nullptr;
        bool declared_here = false;

        // `q = p++ + 1` puts both an increment and an addition between the
        // reference and the assignment. Stopping short of either would leave
        // p to rewrite its own increment, which then collides with the edit
        // the owner's split is already making over the same text.
        if (const Stmt *S = Parent.get<Stmt>())
            if (isSplitTransparent(S))
            {
                if (!consumerOf(cast<Expr>(S), Ctx, Outer, Parent))
                    return nullptr;
                continue;
            }

        if (const auto *BO = dyn_cast_or_null<BinaryOperator>(Parent.get<Stmt>()))
        {
            if (BO->getOpcode() == BO_Assign)
            {
                const auto *LHS =
                    dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts());
                if (LHS && isTracked(LHS->getDecl()))
                {
                    Owner = cast<VarDecl>(LHS->getDecl());
                    RHS = BO->getRHS();
                }
            }
        }
        else if (const auto *VD = Parent.get<VarDecl>())
        {
            if (isTracked(VD) && VD->hasInit())
            {
                Owner = VD;
                RHS = VD->getInit();
                declared_here = true;
            }
        }

        if (!Owner || !RHS)
            return nullptr;

        PointerAccess probe;
        splitAssignedValue(RHS, probe, Owner, declared_here);
        if (probe.root_expr != static_cast<const Expr *>(DRE))
            return nullptr;
        if (step)
            *step = probe.root_adjust;
        return Owner;
    }
}

// ============================================================================
// Collection
// ============================================================================

// Pick up every pointer-typed VarDecl (locals and parameters) and record
// it in `tracked_pointers`. A pointer with an initializer also gets an
// Init access carrying the (root, offset) split of that initializer.
bool PointerAccessCollector::VisitVarDecl(VarDecl *VD)
{
    if (!VD->getType()->isPointerType())
        return true;
    if (SM.isInSystemHeader(VD->getLocation()))
        return true;

    PointerCandidate candidate;
    candidate.ptr_var = VD;
    candidate.is_parameter = isa<ParmVarDecl>(VD);

    std::vector<PointerAccess> access_list;
    if (VD->hasInit())
    {
        PointerAccess pa;
        pa.kind = isNullExpr(VD->getInit()) ? PointerAccessKind::InitNull
                                            : PointerAccessKind::Init;
        pa.loc = VD->getInit()->getBeginLoc();
        pa.expr = VD->getInit();
        splitAssignedValue(VD->getInit(), pa, VD, /*owner_is_declared_here=*/true);
        access_list.push_back(pa);
    }

    tracked_pointers[VD] = candidate;
    accesses[VD] = access_list;

    if (VERBOSE)
        llvm::outs() << "[Collect] Tracking pointer: " << VD->getNameAsString()
                     << (candidate.is_parameter ? " (parameter)" : " (local)") << "\n";

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

    auto it = tracked_pointers.find(VD);
    if (it == tracked_pointers.end())
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

    // The value read, and the reason this pass has no rejections beyond
    // &p: whatever the surrounding expression is, `(p + p_index_xj)` is
    // the pointer it used to see.
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

    // ---- the root of another tracked pointer's (base, index) assignment --
    RootAdjust pair_step = RootAdjust::None;
    if (const VarDecl *Owner = pairwiseOwner(DRE, &pair_step))
    {
        PointerAccess &pa =
            emit(PointerAccessKind::PairwiseRoot, DRE->getLocation());
        pa.pair_owner = Owner;
        // Remembered so that, if the owner turns out not to be rewritten, the
        // demotion can put the step back instead of dropping it.
        pa.root_adjust = pair_step;
        return;
    }

    // An initializer the owner did not take as its root — say because the
    // offset mentions another tracked pointer — is still just a read.
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
        {
            const Stmt *GP = skipTransparentParents(UO, Ctx);
            bool is_write = false;
            if (const auto *BO = dyn_cast_or_null<BinaryOperator>(GP))
                is_write = BO->isAssignmentOp() &&
                           BO->getLHS()->IgnoreParenImpCasts() == UO;
            emit(is_write ? PointerAccessKind::DerefWrite : PointerAccessKind::Deref,
                 UO->getBeginLoc());
            return;
        }
        case UO_PostInc:
        case UO_PreInc:
        case UO_PostDec:
        case UO_PreDec:
        {
            // Either standalone (`p++`) or the mutation inside `*p++`. The
            // dereferenced form is one edit over the whole `*p++`, and it
            // is valid on either side of an assignment.
            bool is_inc = UO->getOpcode() == UO_PostInc || UO->getOpcode() == UO_PreInc;
            bool is_post = UO->getOpcode() == UO_PostInc || UO->getOpcode() == UO_PostDec;
            const Stmt *GP = skipTransparentParents(UO, Ctx);
            if (const auto *GUO = dyn_cast_or_null<UnaryOperator>(GP))
            {
                if (GUO->getOpcode() == UO_Deref)
                {
                    PointerAccessKind k =
                        is_inc ? (is_post ? PointerAccessKind::DerefPostInc
                                          : PointerAccessKind::DerefPreInc)
                               : (is_post ? PointerAccessKind::DerefPostDec
                                          : PointerAccessKind::DerefPreDec);
                    emit(k, GUO->getBeginLoc());
                    return;
                }
            }
            emit(is_inc ? PointerAccessKind::Increment : PointerAccessKind::Decrement,
                 UO->getBeginLoc());
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
            // Without walking out to that last node, field_name comes back
            // empty and the rewrite leaves a dangling "gce_index_xj].".
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
            bool is_write = false;
            if (const auto *BO = dyn_cast_or_null<BinaryOperator>(Outer))
                is_write = BO->isAssignmentOp() &&
                           BO->getLHS()->IgnoreParenImpCasts() == RealME;
            if (const auto *UO2 = dyn_cast_or_null<UnaryOperator>(Outer))
                is_write = is_write || UO2->isIncrementDecrementOp();

            PointerAccess &pa =
                emit(is_write ? PointerAccessKind::ArrowWrite
                              : PointerAccessKind::ArrowAccess,
                     ME->getBeginLoc());
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
            const Stmt *GP = skipTransparentParents(ASE, Ctx);
            bool is_write = false;
            if (const auto *BO = dyn_cast_or_null<BinaryOperator>(GP))
                is_write = BO->isAssignmentOp() &&
                           BO->getLHS()->IgnoreParenImpCasts() == ASE;
            PointerAccess &pa =
                emit(is_write ? PointerAccessKind::SubscriptWrite
                              : PointerAccessKind::Subscript,
                     ASE->getBeginLoc());
            pa.subscript_expr = ASE->getIdx();
            pa.subscript_text = getSourceText(ASE->getIdx(), SM, LO);
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
            const auto *RhsCall =
                dyn_cast<CallExpr>(BO->getRHS()->IgnoreParenImpCasts());
            if (isAllowedSearch(RhsCall))
            {
                emit(PointerAccessKind::AssignFromAllowedFunc, BO->getBeginLoc(), BO)
                    .offset_text = RhsCall->getDirectCallee()->getNameAsString();
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
            PointerAccess &pa =
                emit(BO->getOpcode() == BO_AddAssign ? PointerAccessKind::PlusAssign
                                                     : PointerAccessKind::MinusAssign,
                     BO->getBeginLoc());
            pa.operand_text = getSourceText(BO->getRHS(), SM, LO);
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

                    const Stmt *DerefParent = skipTransparentParents(DerefUO, Ctx);
                    bool is_write = false;
                    if (const auto *AssignBO = dyn_cast_or_null<BinaryOperator>(DerefParent))
                        is_write = AssignBO->isAssignmentOp() &&
                                   AssignBO->getLHS()->IgnoreParenImpCasts() == DerefUO;

                    PointerAccess &pa =
                        emit(is_write ? PointerAccessKind::DerefOffsetWrite
                                      : PointerAccessKind::DerefOffset,
                             DerefUO->getBeginLoc(), DerefUO);
                    pa.offset_terms = terms;
                    for (const OffsetTerm &t : terms)
                        pa.offset_text += (t.minus ? " - " : " + ") +
                                          getSourceText(t.expr, SM, LO);
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

        // p == NULL / p != NULL — the retained pointer is null exactly when
        // it was before, so the test is left alone. Every other comparison
        // is a value read.
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
