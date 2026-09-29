// EditPlan.cpp — see EditPlan.h for what this guarantees and why.

#include "EditPlan.h"

// ============================================================================
// The extent of one rewrite
// ============================================================================

// What a pointer's own accesses decide, before any pairing is considered.
static PointerFacts pointerFactsOf(const std::vector<PointerAccess> &accesses) {
    PointerFacts facts;
    for (const PointerAccess &a : accesses) {
        if (a.kind == PointerAccessKind::AssignFromAllowedFunc)
            facts.null_in_index = true;
        if (a.kind == PointerAccessKind::InitNull ||
            a.kind == PointerAccessKind::AssignNull ||
            a.kind == PointerAccessKind::AssignFromAllowedFunc)
            facts.may_be_null = true;
    }
    return facts;
}

// The variable `access` takes as its root, rewritten or not.
static const VarDecl *rootVar(const PointerAccess &access) {
    const auto *DRE = dyn_cast_or_null<DeclRefExpr>(access.root_expr);
    return DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
}

std::map<const VarDecl *, PointerFacts>
pointerFacts(const RewrittenPointers &rewritten) {
    std::map<const VarDecl *, PointerFacts> facts;
    for (const auto &[ptr, accesses] : rewritten)
        facts[ptr] = pointerFactsOf(*accesses);

    // `p = q` copies q's index as it stands, so whatever q's index can hold
    // p's can. `p = q + 1` does not: a null has no offset to land at, so the
    // source already rules one out. Copies chain — `r = p` after `p = q` —
    // so this runs until nothing changes.
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto &[ptr, accesses] : rewritten)
            for (const PointerAccess &a : *accesses) {
                if (a.isSplit())
                    continue;
                auto root = facts.find(rootVar(a));
                if (root == facts.end())
                    continue;
                PointerFacts &f = facts[ptr];
                PointerFacts merged;
                merged.may_be_null = f.may_be_null || root->second.may_be_null;
                merged.null_in_index =
                    f.null_in_index || root->second.null_in_index;
                if (merged == f)
                    continue;
                f = merged;
                changed = true;
            }
    }
    return facts;
}

// True when `access` must produce an edit. The kinds that legitimately
// produce none are listed with editedNode, in EditPlan.h.
static bool accessNeedsEdit(const PointerAccess &access,
                            const PointerFacts &facts) {
    switch (access.kind) {
    case PointerAccessKind::NoEdit:
    case PointerAccessKind::AddressOf:
    case PointerAccessKind::Unknown:
        return false;
    case PointerAccessKind::NullTest:
        return facts.null_in_index;
    case PointerAccessKind::Init:
    case PointerAccessKind::InitNull:
        // Only a split initializer is rewritten — `int *q = p + 1` keeps
        // `p` and moves the `+ 1` into q's index. An unsplit one is
        // already the base it should be, and `T *p = NULL` is unsplit: the
        // sentinel reaches the index through its declaration's
        // initializer, which is indexDeclInit's answer and not an edit.
        return access.isSplit();
    default:
        return true;
    }
}

// A wrapper turns a library function that hands back a *pointer* into one
// that hands back an index relative to `base`, with -1 for "not found".
// That is what lets `p = strchr(p, c)` move the index while leaving the
// region alone. Every name in g_allowed_funcs must appear here.
static std::string wrapperBodyFor(const std::string &func_name) {
    if (func_name == "strchr")
        return "static int strchr_index_xj(const char *base, int start, int c) {\n"
               "    const char *result = strchr(base + start, c);\n"
               "    if (!result) return -1;\n"
               "    return (int)(result - base);\n"
               "}\n\n";
    if (func_name == "strstr")
        return "static int strstr_index_xj(const char *base, int start, const char *needle) {\n"
               "    const char *result = strstr(base + start, needle);\n"
               "    if (!result) return -1;\n"
               "    return (int)(result - base);\n"
               "}\n\n";
    return "";
}

const Stmt *editedNode(const PointerAccess &access, const PointerFacts &facts) {
    if (!accessNeedsEdit(access, facts))
        return nullptr;

    switch (access.kind) {
    case PointerAccessKind::Init:
    case PointerAccessKind::InitNull:
        return access.rhs_expr;

    case PointerAccessKind::Element:
    case PointerAccessKind::Move:
    case PointerAccessKind::Assign:
    case PointerAccessKind::AssignNull:
    case PointerAccessKind::AssignFromAllowedFunc:
        return access.enclosing_stmt;

    case PointerAccessKind::ValueUse:
        return access.expr;

    // `p == NULL` / `p != NULL` compare against a pointer, so the whole
    // comparison is replaced. Every other form — `if (p)`, `!p`, `p && q` —
    // is a truth test on the reference itself.
    case PointerAccessKind::NullTest: {
        const auto *BO = dyn_cast_or_null<BinaryOperator>(access.enclosing_stmt);
        if (BO && BO->isComparisonOp())
            return BO;
        return access.expr;
    }

    default:
        return nullptr;
    }
}

bool editRangeOf(const Stmt *N, ASTContext &Ctx, FileID &file, unsigned &begin,
                 unsigned &end) {
    if (!N)
        return false;
    const SourceManager &SM = Ctx.getSourceManager();

    // A node can end inside a macro expansion without the pointer's own
    // reference being anywhere near one: `p = NULL` is ordinary file text
    // up to the `=`, and only the right-hand side expands. Refusing those
    // outright would decline every pointer that is ever set to NULL, so
    // the range is mapped onto the macro *name* token instead — the text a
    // reader would edit by hand. makeFileCharRange yields the past-the-end
    // position directly, and reports invalid for the range this cannot be
    // done to: one whose ends come from different expansions.
    CharSourceRange R = Lexer::makeFileCharRange(
        CharSourceRange::getTokenRange(N->getSourceRange()), SM, Ctx.getLangOpts());
    if (R.isInvalid())
        return false;

    auto [StartFile, StartOff] = SM.getDecomposedLoc(R.getBegin());
    auto [StopFile, StopOff] = SM.getDecomposedLoc(R.getEnd());
    if (StartFile != StopFile || StopOff <= StartOff)
        return false;

    file = StartFile;
    begin = StartOff;
    end = StopOff;
    return true;
}

// ============================================================================
// Planning
// ============================================================================

EditPlan::EditPlan(ASTContext &Ctx, const std::set<const VarDecl *> &transformed,
                   const std::map<const VarDecl *, PointerFacts> &facts)
    : Ctx(Ctx), SM(Ctx.getSourceManager()), LO(Ctx.getLangOpts()),
      transformed(transformed), facts(facts),
      violations_at_start(g_invariant_violations) {}

void EditPlan::reportViolation(const llvm::Twine &what, SourceLocation loc) {
    g_invariant_violations++;
    llvm::errs() << "[BUG] xj-prepare-pointertransform: " << what;
    if (loc.isValid())
        llvm::errs() << " at " << loc.printToString(SM);
    llvm::errs() << "\n";
}

void EditPlan::add(const VarDecl *ptr,
                   const std::vector<PointerAccess> &accesses) {
    const PointerFacts &f = facts.at(ptr);

    for (const PointerAccess &access : accesses) {
        if (pairedRoot(access))
            paired_roots.insert(access.root_expr);

        if (!accessNeedsEdit(access, f))
            continue;
        // An initializer is rewritten only to drop what its split moved into
        // the index, so one whose split is not used keeps its text.
        if ((access.kind == PointerAccessKind::Init ||
             access.kind == PointerAccessKind::InitNull) &&
            !splitStands(access))
            continue;

        PlannedEdit e;
        e.ptr = ptr;
        e.access = &access;
        e.node = editedNode(access, f);

        // Validation has already refused a pointer with an access it cannot
        // reach, and the classifier leaves none it cannot anchor, so either
        // failing here is a bug.
        if (!e.node || !editRangeOf(e.node, Ctx, e.file, e.begin, e.end)) {
            reportViolation(llvm::Twine("no usable edit range for a ") +
                                pointerAccessKindToString(access.kind) + " of '" +
                                ptr->getNameAsString() + "'",
                            access.loc);
            continue;
        }

        edits.push_back(std::move(e));
    }
}

void EditPlan::build() {
    // A paired root stays bare: its position reaches the owner through the
    // owner's index, so the root's own rewrite is dropped. Done here rather
    // than in add(), because only now has every owner been seen.
    edits.erase(std::remove_if(edits.begin(), edits.end(),
                               [&](const PlannedEdit &e) {
                                   return paired_roots.count(e.access->expr) != 0;
                               }),
                edits.end());

    // Sort by (file, start, widest first) so a containing range always
    // precedes what it contains, then sweep with a stack of open ranges.
    std::vector<size_t> order(edits.size());
    for (size_t i = 0; i < edits.size(); i++)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const PlannedEdit &A = edits[a], &B = edits[b];
        if (A.file != B.file)
            return A.file < B.file;
        if (A.begin != B.begin)
            return A.begin < B.begin;
        return A.end > B.end;
    });

    std::vector<size_t> open;
    for (size_t i : order) {
        PlannedEdit &e = edits[i];
        while (!open.empty() && (edits[open.back()].file != e.file ||
                                 edits[open.back()].end <= e.begin))
            open.pop_back();

        if (!open.empty()) {
            // Node ranges nest or are disjoint. Anything else means an
            // extent came from somewhere other than editedNode().
            PlannedEdit &parent = edits[open.back()];
            if (e.end > parent.end ||
                (e.begin == parent.begin && e.end == parent.end)) {
                reportViolation("planned edits overlap without nesting",
                                e.access->loc);
                continue;
            }
            e.parent = open.back();
            parent.children.push_back(i);
        }
        open.push_back(i);
    }
    // `order` visits by ascending start, so each child list is already in
    // source order — which is the order renderWithin splices them in.
}

// ============================================================================
// Rendering
// ============================================================================

// True unless `E`'s value is discarded: it is a statement of its own, the
// left arm of a comma, or a for-loop init/increment clause.
//
// Asked of every rewrite that moves or reseats a pointer. A discarded one
// can end at the index update; a consumed one has to hand back a pointer.
static bool valueIsUsed(const Expr *E, ASTContext &Ctx) {
    auto Parents = Ctx.getParents(*E);
    if (Parents.empty())
        return true;
    const Stmt *P = Parents[0].get<Stmt>();
    // A non-Stmt parent is a declarator: `char *q = p++;` consumes the value.
    if (!P)
        return true;
    if (isa<CompoundStmt>(P) || isa<LabelStmt>(P) || isa<CaseStmt>(P) ||
        isa<DefaultStmt>(P))
        return false;
    // Parentheses are transparent to whether the value is wanted.
    if (const auto *PE = dyn_cast<ParenExpr>(P))
        return valueIsUsed(PE, Ctx);
    // A comma discards its left arm unconditionally; the right arm inherits
    // the comma's own fate. This is what keeps the increments in
    // `for (...; ...; p++, q++)` a pair of bare index bumps. `P` is `E`'s
    // immediate parent, so the arm is compared as it stands: `E` may itself
    // be the parentheses this walk climbed through.
    if (const auto *BO = dyn_cast<BinaryOperator>(P))
        if (BO->getOpcode() == BO_Comma)
            return BO->getLHS() == E ? false : valueIsUsed(BO, Ctx);
    if (const auto *FS = dyn_cast<ForStmt>(P))
        return FS->getCond() == E;
    if (const auto *IS = dyn_cast<IfStmt>(P))
        return IS->getCond() == E;
    if (const auto *WS = dyn_cast<WhileStmt>(P))
        return WS->getCond() == E;
    if (const auto *DS = dyn_cast<DoStmt>(P))
        return DS->getCond() == E;
    if (const auto *SS = dyn_cast<SwitchStmt>(P))
        return SS->getCond() == E;
    return true;
}

// True when the assignment *is* a truth test rather than merely having its
// value read: the condition of an `if` or a loop, or an operand of `&&` or
// `||`, parenthesized or not. An index update is already an int that is
// negative exactly when the pointer test would have failed, so these can
// end at `>= 0` instead of rebuilding a pointer.
static bool assignmentIsCondition(const BinaryOperator *BO, ASTContext &Ctx) {
    const Stmt *P = skipTransparentParents(BO, Ctx);
    if (!P)
        return false;
    if (const auto *LogBO = dyn_cast<BinaryOperator>(P))
        return LogBO->getOpcode() == BO_LAnd || LogBO->getOpcode() == BO_LOr;
    const Expr *Cond = nullptr;
    if (const auto *IS = dyn_cast<IfStmt>(P))
        Cond = IS->getCond();
    else if (const auto *WS = dyn_cast<WhileStmt>(P))
        Cond = WS->getCond();
    else if (const auto *FS = dyn_cast<ForStmt>(P))
        Cond = FS->getCond();
    else if (const auto *DS = dyn_cast<DoStmt>(P))
        Cond = DS->getCond();
    // `P` was reached through any parentheses around the assignment, so the
    // condition is looked through the same way before it is compared.
    return Cond && Cond->IgnoreParenImpCasts() == BO;
}

// The pointer a rewritten reference hands back. A pointer that can hold
// the sentinel has to map it to a real null: `base + -1` is one element
// before the region and is not null, so a callee's null check on it would
// wrongly succeed.
//
// What is tested is whatever holds the null; see PointerFacts.
//
// NB: xj-prepare-slicetransform recognizes this shape — a conditional with
// a null pointer constant for one arm — and simplifies it where it can, so
// it has to stay in sync with stripNullGuard in SliceRewriter.cpp.
std::string EditPlan::pointerValue(const VarDecl *ptr) {
    const std::string name = ptr->getNameAsString();
    const std::string idx = indexNameFor(ptr);
    const std::string value = name + " + " + idx;
    auto it = facts.find(ptr);
    if (it == facts.end() || !it->second.may_be_null)
        return "(" + value + ")";
    if (it->second.null_in_index)
        return "(" + idx + " < 0 ? (void *)0 : " + value + ")";
    return "(" + name + " ? " + value + " : (void *)0)";
}

// The function whose body contains `S`, or null.
static const FunctionDecl *enclosingFunction(const Stmt *S, ASTContext &Ctx) {
    DynTypedNode N = DynTypedNode::create(*S);
    while (true) {
        auto Parents = Ctx.getParents(N);
        if (Parents.empty())
            return nullptr;
        N = Parents[0];
        if (const auto *FD = N.get<FunctionDecl>())
            return FD;
    }
}

void EditPlan::needWrapper(const std::string &func_name, const Stmt *use) {
    // The function the use is in, not the one the pointer is declared in:
    // a file-scope pointer has none.
    const FunctionDecl *FD = enclosingFunction(use, Ctx);
    if (!FD || wrapperBodyFor(func_name).empty()) {
        reportViolation("no wrapper can be defined for this use of '" +
                            func_name + "'",
                        use->getBeginLoc());
        return;
    }
    auto [it, inserted] = wrappers.insert({func_name, FD});
    if (inserted)
        return;
    // Two functions in one file want the same body. It has to precede both,
    // so the earlier one wins — and "earlier" is decided by file offset
    // rather than by planning order, which follows neither the source nor
    // anything else a reader could predict.
    auto [oldFile, oldOff] = SM.getDecomposedLoc(it->second->getBeginLoc());
    auto [newFile, newOff] = SM.getDecomposedLoc(FD->getBeginLoc());
    if (newFile == oldFile && newOff < oldOff)
        it->second = FD;
}

const VarDecl *EditPlan::pairedRoot(const PointerAccess &a) const {
    const VarDecl *RootVD = rootVar(a);
    return RootVD && transformed.count(RootVD) ? RootVD : nullptr;
}

bool EditPlan::splitStands(const PointerAccess &a) const {
    return a.isSplit() && (a.step == IndexStep::None || pairedRoot(a));
}

std::string EditPlan::renderIndexValue(const PointerAccess &a, size_t owner) {
    // A null right-hand side reseats the region, and the index goes to the
    // sentinel with it. The region is what this pass tests; the sentinel is
    // what is left to say null once xj-prepare-slicetransform has moved the
    // pointer into index space.
    if (a.kind == PointerAccessKind::InitNull ||
        a.kind == PointerAccessKind::AssignNull)
        return "-1";

    // The terms were lifted out of the right-hand side, so they are
    // rendered against the edit that removed them — a term reading through
    // a rewritten pointer carries that rewrite here with it. Where the split
    // is not used they never left, and the index counts from zero.
    std::string terms;
    if (splitStands(a))
        for (const OffsetTerm &t : a.index_terms)
            terms += (t.minus ? " - " : " + ") +
                     renderTerm(t, owner, /*as_subscript=*/false);

    if (const VarDecl *RootVD = pairedRoot(a))
        return applyStep(a.step, indexNameFor(RootVD)) + terms;
    if (terms.empty())
        return "0";
    // `q + 1` counts from 1 on its own; `q - 1` needs the zero to subtract
    // from.
    return terms.compare(0, 3, " + ") == 0 ? terms.substr(3) : "0" + terms;
}

std::string EditPlan::indexDeclInit(const VarDecl *ptr,
                                    const std::vector<PointerAccess> &accesses) {
    for (const PointerAccess &a : accesses) {
        if (a.kind != PointerAccessKind::Init &&
            a.kind != PointerAccessKind::InitNull)
            continue;
        // A split initializer is a planned edit, and its terms are that
        // edit's to render. An unsplit one moved nothing, so it has no
        // terms and nothing to render them against.
        for (size_t i = 0; i < edits.size(); i++)
            if (edits[i].access == &a)
                return renderIndexValue(a, i);
        return renderIndexValue(a, kNoParent);
    }
    return "0";
}

// How tightly an expression's own text holds together, loosest first.
enum class Binding { Loose, Additive, Multiplicative, Unary };

static Binding bindingOf(const Expr *E) {
    // An implicit conversion has no text; parentheses do, and hold.
    E = E->IgnoreImpCasts();
    if (isa<AbstractConditionalOperator>(E))
        return Binding::Loose;
    if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
        if (BO->isMultiplicativeOp())
            return Binding::Multiplicative;
        if (BO->isAdditiveOp())
            return Binding::Additive;
        return Binding::Loose;
    }
    return Binding::Unary;
}

// The signed type to read `E` as, when `E` is unsigned and wide enough to
// make the sum it joins unsigned. Null otherwise: a narrower type is
// promoted to int whatever its sign.
static QualType signedCounterpart(const Expr *E, ASTContext &Ctx) {
    QualType T = E->getType();
    if (const auto *ET = T->getAs<EnumType>())
        T = ET->getDecl()->getIntegerType();
    if (T.isNull())
        return QualType();
    T = T.getCanonicalType().getUnqualifiedType();
    if (!T->isUnsignedIntegerType() ||
        Ctx.getIntWidth(T) < Ctx.getIntWidth(Ctx.IntTy))
        return QualType();
    return Ctx.getCorrespondingSignedType(T);
}

std::string EditPlan::renderTerm(const OffsetTerm &term, size_t owner,
                                 bool as_subscript) {
    std::string text = owner == kNoParent ? getSourceText(term.expr, SM, LO)
                                          : renderWithin(term.expr, owner);
    auto held = [&](Binding at_least) {
        return bindingOf(term.expr) < at_least ? "(" + text + ")" : text;
    };

    QualType Signed =
        as_subscript ? signedCounterpart(term.expr, Ctx) : QualType();
    if (!Signed.isNull())
        return "(" + Signed.getAsString(Ctx.getPrintingPolicy()) + ")" +
               held(Binding::Unary);
    // `a + b` can follow a `+` as it is, and not a `-`.
    return held(term.minus ? Binding::Multiplicative : Binding::Additive);
}

std::string EditPlan::renderWithin(const Stmt *S, size_t owner) {
    FileID file;
    unsigned begin = 0, end = 0;
    std::string text;
    if (S)
        text = getSourceText(S->getSourceRange(), SM, LO);

    // Without a usable range there is no way to locate a child inside the
    // text, so nothing can be spliced. Any child that needed to be is left
    // unconsumed, which verify() reports precisely — more precisely than
    // this could, since it cannot tell which children were in `S`.
    if (!S || !editRangeOf(S, Ctx, file, begin, end) || text.size() != end - begin)
        return text;

    std::string out;
    unsigned cursor = begin;
    for (size_t c : edits[owner].children) {
        const PlannedEdit &child = edits[c];
        // Siblings in the forest are disjoint, so a child either sits
        // wholly inside `S` — one of the owner's other sub-expressions
        // otherwise — or the forest is malformed and verify() will say so.
        if (child.file != file || child.begin < cursor || child.end > end)
            continue;
        out.append(text, cursor - begin, child.begin - cursor);
        out += render(c);
        cursor = child.end;
    }
    out.append(text, cursor - begin, std::string::npos);
    return out;
}

std::string EditPlan::render(size_t i) {
    PlannedEdit &e = edits[i];
    const PointerAccess &a = *e.access;
    e.consumed = true;

    const std::string ptr = e.ptr->getNameAsString();
    const std::string idx = indexNameFor(e.ptr);

    switch (a.kind) {
    // ---- Element access: the index names the element ------------------
    // Every form is `p[INDEX]`, with INDEX counted from the pointer's own
    // index. An access has at most one of a step, offset terms and a
    // subscript, and only `p->field` has anything after the bracket.
    case PointerAccessKind::Element: {
        std::string index = applyStep(a.step, idx);  // *p++
        // Each term is rendered rather than copied, so an offset that
        // reads through a rewritten pointer — `*(p + strlen(p) - 1)` —
        // carries that pointer's rewrite along instead of losing it.
        for (const OffsetTerm &t : a.offset_terms)   // *(p + a - b)
            index += (t.minus ? " - " : " + ") +
                     renderTerm(t, i, /*as_subscript=*/true);
        if (a.subscript_expr) {                      // p[i]
            std::string sub = renderTerm({a.subscript_expr, /*minus=*/false}, i,
                                         /*as_subscript=*/true);
            if (sub != "0")
                index += " + " + sub;
        }
        std::string out = ptr + "[" + index + "]";
        if (!a.field_name.empty())                   // p->field
            out += "." + a.field_name;
        return out;
    }

    // ---- Position: the index moves, the base stays put ----------------
    //
    // `p++` moves the index. If the expression's value is consumed as a
    // pointer, the result is rebuilt so the type stays a pointer:
    // `unhex(++in)` becoming `unhex(++in_index_xj)` once passed an int
    // where a `char *` was expected, and c2rust turned the int into
    // literal addresses like 0x4.
    //
    // Pre-increment returns the new pointer and `(p + ++p_index_xj)`
    // increments first, so both see the new position; post-increment
    // returns the old pointer and `(p + p_index_xj++)` yields the old
    // index, so both see the old one.
    //
    // `p += n` moves the index by `n` and yields the new pointer, which
    // `(p + (p_index_xj += n))` rebuilds from the new index. The operand
    // is rendered rather than copied, so a rewrite inside it is carried
    // along.
    case PointerAccessKind::Move: {
        bool wrap = valueIsUsed(cast<Expr>(e.node), Ctx);
        if (const auto *BO = dyn_cast<BinaryOperator>(e.node)) {
            const char *op = BO->getOpcode() == BO_AddAssign ? " += " : " -= ";
            std::string bare = idx + op + renderWithin(BO->getRHS(), i);
            return wrap ? "(" + ptr + " + (" + bare + "))" : bare;
        }
        std::string bare = applyStep(a.step, idx);
        return wrap ? "(" + ptr + " + " + bare + ")" : bare;
    }

    // ---- (base, index) assignment -------------------------------------
    case PointerAccessKind::Init:
    case PointerAccessKind::InitNull:
        // A split initializer keeps only its root: `int *q = p + 1;`
        // becomes `int *q = p;` with the `+ 1` moved into q's index.
        return renderWithin(a.root_expr, i);

    case PointerAccessKind::Assign:
    case PointerAccessKind::AssignNull: {
        const auto *BO = cast<BinaryOperator>(e.node);
        bool value_used = valueIsUsed(BO, Ctx);

        if (splitStands(a)) {
            // Split: the right-hand side keeps only its root and the rest
            // becomes the index.
            //
            // The base assignment goes first. Not because the order is
            // arbitrary — because xj-prepare-baserewrite reconstructs a
            // pointer by deleting that assignment as the comma's *left*
            // arm, and only that arm (Reconstructor.cpp). Emitting the
            // pair the other way round is still correct C, but it costs
            // the pointer its reconstruction downstream.
            //
            // The exception is an offset that reads through this very
            // pointer. `p = q + (p - buf)` wants p's old value, so the
            // index has to be sequenced ahead of the base — assigning the
            // base first would hand the offset q's. Such a pointer is not
            // reconstructed, which is a worse translation but a correct
            // one; the alternative is neither.
            bool index_first = false;
            for (const OffsetTerm &t : a.index_terms)
                if (referencesAnyOf(t.expr, {e.ptr})) {
                    index_first = true;
                    break;
                }

            std::string index = renderIndexValue(a, i);
            std::string root = renderWithin(a.root_expr, i);
            std::string out = index_first
                                  ? "(" + idx + " = " + index + ", " + ptr + " = " + root
                                  : "(" + ptr + " = " + root + ", " + idx + " = " + index;
            if (value_used)
                out += ", " + pointerValue(e.ptr);
            return out + ")";
        }

        // Kept whole: the index update comes last, so a right-hand side
        // that reads through this pointer — `p = p->next` — still sees the
        // old position.
        std::string out = "(" + renderWithin(BO, i) + ", " + idx + " = " +
                          renderIndexValue(a, i);
        if (value_used)
            out += ", " + pointerValue(e.ptr);
        return out + ")";
    }

    // ---- Assignment from an allowlisted function ----------------------
    // `p = strchr(p, c)` searches inside a region that already exists, so
    // the region is left alone and only the index moves. The wrapper hands
    // back an offset from `base`, or -1 for "not found".
    case PointerAccessKind::AssignFromAllowedFunc: {
        const auto *BO = cast<BinaryOperator>(e.node);
        const auto *CE = dyn_cast<CallExpr>(BO->getRHS()->IgnoreParenImpCasts());
        // The classifier reached this kind only by finding both, and the
        // planner reached this rendering only through the classifier.
        const auto *BaseDRE = dyn_cast_or_null<DeclRefExpr>(a.root_expr);
        const auto *BaseVD = BaseDRE ? dyn_cast<VarDecl>(BaseDRE->getDecl()) : nullptr;
        const FunctionDecl *Callee = CE ? CE->getDirectCallee() : nullptr;
        if (!BaseVD || !Callee) {
            reportViolation("an allowlisted search lost its callee or its "
                            "region argument",
                            a.loc);
            return getSourceText(e.node->getSourceRange(), SM, LO);
        }
        // The library function — `strchr` — that the wrapper is named after.
        const std::string func_name = Callee->getNameAsString();

        // Argument 0 is the region searched; it becomes the wrapper's
        // `base`, and everything after it is passed through — rendered
        // rather than copied, so an argument that reads through another
        // rewritten pointer carries that rewrite with it.
        std::string other_args;
        for (unsigned n = 1; n < CE->getNumArgs(); n++)
            other_args += ", " + renderWithin(CE->getArg(n), i);

        // The search starts wherever the region argument currently points.
        // If that argument is itself a rewritten pointer its position lives
        // in its index; otherwise it is a plain pointer at its own origin.
        const std::string base_text = BaseVD->getNameAsString();
        const std::string start = pairedRoot(a) ? indexNameFor(BaseVD) : "0";
        const std::string wrapper_name = func_name + "_index_xj";

        std::string out = idx + " = " + wrapper_name + "(" + base_text + ", " +
                          start + other_args + ")";

        // `l = strchr(path, sep)` lands in path's region, not l's, so l's
        // region is reseated to path and the index counts from there. With
        // the base and the pointer one variable, the reseat is what carries
        // it.
        if (BaseVD != e.ptr)
            out = "(" + ptr + " = " + base_text + ", " + out + ")";

        // `while ((p = strchr(p, c)))` tested a pointer; the index form is
        // an int that is negative exactly when that test failed.
        //
        // Anywhere else the value is read it is read *as a pointer* —
        // `(p = strchr(p, c)) != NULL` compares against one — so the update
        // has to hand a pointer back, exactly as an ordinary Assign does.
        // Ending at `>= 0` there would leave an int compared to NULL.
        if (assignmentIsCondition(BO, Ctx))
            out = "(" + out + ") >= 0";
        else if (valueIsUsed(BO, Ctx))
            out = "(" + out + ", " + pointerValue(e.ptr) + ")";

        needWrapper(func_name, e.node);
        return out;
    }

    // ---- Null test ----------------------------------------------------
    // `(p, off)` is null when either half says so, so the faithful test is
    // `p && off >= 0`. The region half cannot be dropped: a reseat from an
    // opaque call — `p = malloc(n)` — leaves off at 0 with a region that
    // may still be null, so `off >= 0` alone would call it non-null.
    //
    // Only planned at all where an index can go negative under a live
    // region; see PointerFacts::null_in_index.
    case PointerAccessKind::NullTest: {
        const std::string non_null = "(" + ptr + " && " + idx + " >= 0)";
        const auto *BO = dyn_cast<BinaryOperator>(e.node);
        // `p == NULL` / `p != NULL` compare against a pointer, so the whole
        // comparison is replaced. Every other form — `if (p)`, `!p`,
        // `p && q` — is a truth test on the reference itself, and swapping
        // the reference for the test lets `!p` fall out as
        // `!(p && p_index_xj >= 0)` with no case of its own.
        if (BO && BO->isComparisonOp())
            return BO->getOpcode() == BO_EQ
                       ? "(!" + ptr + " || " + idx + " < 0)"
                       : non_null;
        return non_null;
    }

    // ---- Value read: the fallback that makes the rewrite total --------
    case PointerAccessKind::ValueUse:
        return pointerValue(e.ptr);

    default:
        reportViolation(llvm::Twine("no rendering for a ") +
                            pointerAccessKindToString(a.kind),
                        a.loc);
        return getSourceText(e.node->getSourceRange(), SM, LO);
    }
}

void EditPlan::appendRootEdits(std::vector<Edit> &out) {
    for (size_t i = 0; i < edits.size(); i++) {
        if (edits[i].parent != kNoParent)
            continue;
        Edit edit;
        edit.type = Edit::Replace;
        edit.offset = edits[i].begin;
        edit.start = edits[i].node->getBeginLoc();
        edit.end = Lexer::getLocForEndOfToken(edits[i].node->getEndLoc(), 0, SM, LO);
        edit.text = render(i);
        out.push_back(std::move(edit));
    }

    // After rendering, because rendering is what discovers them.
    for (const auto &[func_name, FD] : wrappers) {
        Edit edit;
        edit.type = Edit::InsertBefore;
        edit.start = FD->getBeginLoc();
        edit.offset = SM.getFileOffset(edit.start);
        edit.text = wrapperBodyFor(func_name);
        out.push_back(std::move(edit));
    }
}

bool EditPlan::verify(const std::vector<Edit> &out) {
    for (const PlannedEdit &e : edits) {
        if (e.consumed)
            continue;
        // Neither applied on its own nor spliced into the edit containing
        // it, so its reference would keep its pre-rewrite meaning inside
        // text that edit copied.
        reportViolation(llvm::Twine("a ") + pointerAccessKindToString(e.access->kind) +
                            " rewrite of '" + e.ptr->getNameAsString() +
                            "' was neither applied nor folded into the rewrite "
                            "containing it",
                        e.access->loc);
    }

    // Only forest roots are emitted, so this holds by construction — which
    // is exactly why it is worth stating where it can be checked.
    std::vector<const Edit *> replaces;
    for (const Edit &e : out)
        if (e.type == Edit::Replace)
            replaces.push_back(&e);
    std::sort(replaces.begin(), replaces.end(),
              [](const Edit *a, const Edit *b) { return a->offset < b->offset; });
    for (size_t i = 1; i < replaces.size(); i++) {
        const Edit *prev = replaces[i - 1];
        if (SM.getFileID(prev->start) == SM.getFileID(replaces[i]->start) &&
            replaces[i]->offset < SM.getFileOffset(prev->end))
            reportViolation("two replacements cover the same text",
                            replaces[i]->start);
    }

    // An index declaration is a statement-level insertion, so nothing
    // should ever place one in the middle of a rewritten expression.
    for (const Edit &e : out) {
        if (e.type == Edit::Replace)
            continue;
        for (const Edit *r : replaces) {
            if (SM.getFileID(r->start) != SM.getFileID(e.start))
                continue;
            if (r->offset < e.offset && e.offset < SM.getFileOffset(r->end)) {
                reportViolation("an insertion lands inside replaced text", e.start);
                break;
            }
        }
    }

    return g_invariant_violations == violations_at_start;
}
