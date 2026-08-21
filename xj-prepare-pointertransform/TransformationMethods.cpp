// TransformationMethods.cpp — emits the actual rewrites.
//
// One function rewrites every tracked pointer, local, parameter or
// file-scope alike. There is only one shape to emit:
//
//     T *p          stays exactly where it is, holding a base
//     int p_index_xj    declared beside it, holding the position
//
// so the three paths this file used to have — collapse the declaration,
// retain a parameter, duplicate the whole switch for globals — collapse
// into one. The only thing that varies between them is where the index
// declaration goes.

#include "FunctionAccessAnalyzer.h"

// ============================================================================
// Placing an index declaration alongside a for-init declaration
// ============================================================================
//
// An ordinary declaration gets its index on the next line, where
// everything the declaration binds is already in scope. A for-init has no
// position after it that accepts a statement — that slot is the loop
// condition — so its index goes before the whole loop.
//
// That is as far as it may travel: the anchor has to execute exactly as
// often as the declaration it precedes, and a pointer declared in a loop
// body is re-initialized on every iteration, so hoisting to the block or
// function top would leave a stale index on the second pass. Immediately
// before the loop, nothing runs in between.
//
// And the anchor is only a legal home for a declaration if it sits
// directly inside a compound statement. As the unbraced substatement of an
// if / else / while / for / do, or after a label or `case`, C admits a
// statement but not a declaration — `if (c) int i = 0;` does not parse —
// and a bare insertion there would also detach the loop from the construct
// that owns it. Such an anchor gets wrapped in a fresh block instead.

struct DeclAnchor
{
    const Stmt *stmt = nullptr; // insert before this; null = no legal position
    bool needs_braces = false;  // ... after wrapping it in a block
};

static DeclAnchor forInitAnchorFor(const ForStmt *FS, ASTContext &Ctx)
{
    DeclAnchor A;
    A.stmt = FS;

    auto Parents = Ctx.getParents(*A.stmt);
    if (!Parents.empty() && Parents[0].get<CompoundStmt>())
        return A;

    A.needs_braces = true;
    return A;
}

// Emit `decl` immediately before the anchor statement. Returns false if
// there is no position that can hold it, in which case the caller must
// abandon the rewrite — the edits collected so far are only applied once
// the whole pointer succeeds.
//
// Two pointers wrapping the same loop nest correctly with no coordination
// between their (separate) edit batches: InsertTextBefore places a later
// insertion *first* at a location and InsertTextAfterToken places it
// *last*, so the second pointer's braces enclose the first's.
static bool emitDeclBefore(const DeclAnchor &A, const std::string &decl,
                           const SourceManager &SM, const LangOptions &LO,
                           std::vector<Edit> &edits)
{
    if (!A.stmt)
        return false;

    SourceLocation Start = A.stmt->getBeginLoc();
    std::string indent = getIndentBeforeLoc(Start, SM).str();

    Edit open;
    open.type = Edit::InsertBefore;
    open.offset = SM.getFileOffset(Start);
    open.start = Start;
    open.text = (A.needs_braces ? "{ " : "") + decl + "\n" + indent;
    edits.push_back(open);

    if (!A.needs_braces)
        return true;

    // The closing brace goes after the statement's terminating semicolon,
    // which is not part of the statement when its last token is an
    // expression: `for (...) s += *p;` ends at `p`. Stopping there would
    // emit `s += p[i]} ;`, and in an if/else it would strand the `else`.
    SourceLocation End = A.stmt->getEndLoc();
    Token last;
    bool ends_at_terminator =
        !Lexer::getRawToken(End, last, SM, LO, /*IgnoreWhiteSpace=*/true) &&
        (last.is(tok::r_brace) || last.is(tok::semi));
    if (!ends_at_terminator)
    {
        auto next = Lexer::findNextToken(End, SM, LO);
        if (!next || !next->is(tok::semi))
            return false;
        End = next->getLocation();
    }

    Edit close;
    close.type = Edit::InsertAfterToken;
    close.offset = SM.getFileOffset(End);
    close.start = End;
    close.text = "\n" + indent + "}";
    edits.push_back(close);
    return true;
}

// ============================================================================
// Edit builders
// ============================================================================
//
// Every edit has to land on real source text. Validation rejects a pointer
// whose own reference sits inside a macro expansion, but an edit is not
// always anchored on that reference: an Init or an Assign is bracketed
// around the whole assignment, so its far end is the end of the right-hand
// side. `p = NULL;` ends inside the expansion of NULL while `p` itself is
// ordinary file text — and a macro location is neither rewritable (the
// Rewriter drops the edit, which is how `(p = NULL;` loses its closing
// paren) nor sortable, since its file offset belongs to the expansion
// buffer rather than the file.
//
// Mapping through the expansion puts the edit on the macro *name* token,
// which is the text a reader would edit by hand. A range that cannot be
// expressed in the file at all — one whose ends come from different
// expansions — yields an invalid range, and the builder reports failure so
// the caller abandons the pointer rather than emitting half a rewrite.

// The file-text span of `S`, past-the-end, or an invalid range.
static CharSourceRange fileRangeOf(const Stmt *S, const SourceManager &SM,
                                   const LangOptions &LO)
{
    return Lexer::makeFileCharRange(
        CharSourceRange::getTokenRange(S->getSourceRange()), SM, LO);
}

static bool replaceNode(std::vector<Edit> &edits, const Stmt *S,
                        const std::string &text, const SourceManager &SM,
                        const LangOptions &LO)
{
    CharSourceRange R = fileRangeOf(S, SM, LO);
    if (R.isInvalid())
        return false;

    Edit e;
    e.type = Edit::Replace;
    e.start = R.getBegin();
    e.offset = SM.getFileOffset(e.start);
    e.end = R.getEnd();
    e.text = text;
    edits.push_back(e);
    return true;
}

static bool insertBeforeNode(std::vector<Edit> &edits, const Stmt *S,
                             const std::string &text, const SourceManager &SM,
                             const LangOptions &LO)
{
    CharSourceRange R = fileRangeOf(S, SM, LO);
    if (R.isInvalid())
        return false;

    Edit e;
    e.type = Edit::InsertBefore;
    e.start = R.getBegin();
    e.offset = SM.getFileOffset(e.start);
    e.text = text;
    edits.push_back(e);
    return true;
}

static bool insertAfterNode(std::vector<Edit> &edits, const Stmt *S,
                            const std::string &text, const SourceManager &SM)
{
    // InsertAfterToken advances past the token it is given, so this one
    // wants the *start* of the last token rather than fileRangeOf's
    // past-the-end position. Keeping InsertAfterToken also keeps the
    // stacking order: a later insertion at a location goes last, which is
    // what puts a closing tail outside one already emitted there.
    SourceLocation End = S->getEndLoc();
    if (End.isMacroID())
        End = SM.getExpansionRange(End).getEnd();
    if (End.isInvalid() || !End.isFileID())
        return false;

    Edit e;
    e.type = Edit::InsertAfterToken;
    e.start = End;
    e.offset = SM.getFileOffset(End);
    e.text = text;
    edits.push_back(e);
    return true;
}

// True when the value of an assignment expression is consumed. A discarded
// assignment can end at the index update; a consumed one has to hand back a
// pointer, so it appends `p + p_index_xj` as a third comma element.
static bool assignmentValueIsUsed(const BinaryOperator *BO, ASTContext &Ctx)
{
    auto Parents = Ctx.getParents(*BO);
    if (Parents.empty())
        return true;
    const Stmt *P = Parents[0].get<Stmt>();
    if (!P)
        return true;
    if (isa<CompoundStmt>(P) || isa<LabelStmt>(P) || isa<CaseStmt>(P) ||
        isa<DefaultStmt>(P))
        return false;
    if (const auto *FS = dyn_cast<ForStmt>(P))
        return FS->getCond() == BO;
    if (const auto *IS = dyn_cast<IfStmt>(P))
        return IS->getCond() == BO;
    if (const auto *WS = dyn_cast<WhileStmt>(P))
        return WS->getCond() == BO;
    if (const auto *DS = dyn_cast<DoStmt>(P))
        return DS->getCond() == BO;
    if (const auto *SS = dyn_cast<SwitchStmt>(P))
        return SS->getCond() == BO;
    return true;
}

// The index expression an Init or Assign should install. When the root is a
// pointer this pass also rewrote, the two indices are paired — `p = q + 1`
// is `p_index_xj = q_index_xj + 1`. Otherwise the root's value is still its
// own position, so the offset counts from zero.
static std::string indexValueFor(const PointerAccess &acc,
                                 const std::set<const VarDecl *> &transformed)
{
    if (const auto *DRE = dyn_cast_or_null<DeclRefExpr>(acc.root_expr))
    {
        const auto *RootVD = dyn_cast<VarDecl>(DRE->getDecl());
        if (RootVD && transformed.count(RootVD))
            return indexNameFor(RootVD) + acc.operand_text;
    }
    return acc.offset_text;
}

// ============================================================================
// generateTransformation — rewrite one pointer, wherever it lives
// ============================================================================

bool FunctionAccessAnalyzer::generateTransformation(
    const FunctionDecl *FD,
    const VarDecl *PtrVar,
    PointerCandidate &candidate,
    std::vector<PointerAccess> &accesses,
    const std::set<const VarDecl *> &transformed,
    ASTContext &Ctx)
{

    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    const std::string ptr = PtrVar->getNameAsString();
    const std::string idx = indexNameFor(PtrVar);
    const std::string elem = ptr + "[" + idx; // prefix of every element access
    const std::string value = "(" + ptr + " + " + idx + ")";

    std::vector<Edit> edits;

    // ---- Step 1: declare the index beside the pointer --------------------
    //
    // The pointer's own declaration is left alone except for the root
    // rewrite of a split initializer. Nothing is ever deleted, so no text
    // this pass emits can name something that is no longer there.
    const PointerAccess *init = nullptr;
    for (const auto &access : accesses)
    {
        if (access.kind == PointerAccessKind::Init)
        {
            init = &access;
            break;
        }
    }

    std::string index_init = init ? indexValueFor(*init, transformed) : "0";
    std::string index_decl = "int " + idx + " = " + index_init + ";";

    if (candidate.is_parameter)
    {
        // The parameter arrives holding its base; the index starts at 0.
        const auto *CS = FD ? dyn_cast_or_null<CompoundStmt>(FD->getBody()) : nullptr;
        if (!CS)
        {
            if (VERBOSE)
                llvm::outs() << "[Error] Function body is not a CompoundStmt\n";
            return false;
        }
        SourceLocation lbrace = CS->getLBracLoc();
        std::string indent = getIndentBeforeLoc(lbrace, SM).str() + "    ";

        // InsertBefore at the position just past `{`, rather than
        // InsertAfterToken on `{` itself: pointers are rewritten in reverse
        // source order (see transformAllFunctions), and only InsertBefore
        // stacks a later insertion ahead of an earlier one, which puts the
        // declarations back in source order.
        Edit e;
        e.type = Edit::InsertBefore;
        e.start = Lexer::getLocForEndOfToken(lbrace, 0, SM, LO);
        e.offset = SM.getFileOffset(e.start);
        e.text = "\n" + indent + index_decl;
        edits.push_back(e);
    }
    else if (PtrVar->hasGlobalStorage())
    {
        // File scope: the index is a file-scope int with the same linkage.
        std::string prefix =
            PtrVar->getStorageClass() == SC_Static ? "static " : "";
        auto semi = Lexer::findNextToken(PtrVar->getEndLoc(), SM, LO);
        if (!semi || !semi->is(tok::semi))
        {
            if (VERBOSE)
                llvm::outs() << "[Error] No terminator after global " << ptr << "\n";
            return false;
        }
        Edit e;
        e.type = Edit::InsertBefore;
        e.start = Lexer::getLocForEndOfToken(semi->getLocation(), 0, SM, LO);
        e.offset = SM.getFileOffset(e.start);
        e.text = "\n" + prefix + index_decl;
        edits.push_back(e);
    }
    else
    {
        const DeclStmt *DS = FD ? findDeclStmtForVar(PtrVar, FD->getBody()) : nullptr;
        if (!DS)
        {
            if (VERBOSE)
                llvm::outs() << "[Error] Could not find DeclStmt for " << ptr << "\n";
            return false;
        }
        if (const ForStmt *FS = forStmtInitializedBy(DS, Ctx))
        {
            if (!emitDeclBefore(forInitAnchorFor(FS, Ctx), index_decl, SM, LO, edits))
            {
                if (VERBOSE)
                    llvm::outs() << "[Error] No position for the index of " << ptr << "\n";
                return false;
            }
        }
        else
        {
            // On the next line, where every name the declaration binds —
            // including a sibling declarator's index — is already in scope.
            // InsertBefore at the position past the terminator rather than
            // InsertAfterToken on it: pointers are rewritten in reverse
            // source order, and only InsertBefore stacks a later insertion
            // ahead of an earlier one, which is what puts two indices
            // sharing this anchor back in source order.
            std::string indent = getIndentBeforeLoc(DS->getBeginLoc(), SM).str();
            Edit e;
            e.type = Edit::InsertBefore;
            e.start = Lexer::getLocForEndOfToken(DS->getEndLoc(), 0, SM, LO);
            e.offset = SM.getFileOffset(e.start);
            e.text = "\n" + indent + index_decl;
            edits.push_back(e);
        }
    }

    // A split initializer keeps only its root: `int *q = p + 1;` becomes
    // `int *q = p;` with the `+ 1` moved into q's index.
    if (init && init->root_expr && init->rhs_expr &&
        init->root_expr != init->rhs_expr->IgnoreParenImpCasts())
    {
        if (!replaceNode(edits, init->rhs_expr,
                         getSourceText(init->root_expr, SM, LO), SM, LO))
        {
            if (VERBOSE)
                llvm::outs() << "[Error] Initializer of " << ptr
                             << " is not file text\n";
            return false;
        }
    }

    // ---- Step 2: rewrite each access -------------------------------------
    for (const auto &access : accesses)
    {
        auto parentOf = [&](const Expr *E)
        { return skipTransparentParents(E, Ctx); };

        // Set by every builder below. A builder that cannot place its edit
        // would leave this access in pointer form while its siblings moved
        // to the index form, so the pointer is abandoned whole.
        bool placed = true;

        switch (access.kind)
        {

        // Nothing to do. A retained pointer is null exactly when it was
        // before, so null tests stand; a pairwise root is carried by its
        // owner's assignment; sizeof never reads the value.
        case PointerAccessKind::Init:
        case PointerAccessKind::NullTest:
        case PointerAccessKind::NoEdit:
        case PointerAccessKind::PairwiseRoot:
        case PointerAccessKind::AddressOf:
        case PointerAccessKind::Unknown:
            break;

        // ---- Element access -------------------------------------------
        case PointerAccessKind::Deref:
        case PointerAccessKind::DerefWrite:
        {
            const auto *UO = dyn_cast_or_null<UnaryOperator>(parentOf(access.expr));
            if (!UO)
                break;
            placed = replaceNode(edits, UO, elem + "]", SM, LO);
            break;
        }

        case PointerAccessKind::DerefPostInc:
        case PointerAccessKind::DerefPreInc:
        case PointerAccessKind::DerefPostDec:
        case PointerAccessKind::DerefPreDec:
        {
            const Stmt *P = parentOf(access.expr);
            const auto *MutOp = dyn_cast_or_null<UnaryOperator>(P);
            if (!MutOp)
                break;
            const auto *DerefOp =
                dyn_cast_or_null<UnaryOperator>(skipTransparentParents(MutOp, Ctx));
            if (!DerefOp)
                break;
            const char *op = (access.kind == PointerAccessKind::DerefPostInc ||
                              access.kind == PointerAccessKind::DerefPreInc)
                                 ? "++"
                                 : "--";
            bool is_post = access.kind == PointerAccessKind::DerefPostInc ||
                           access.kind == PointerAccessKind::DerefPostDec;
            placed = replaceNode(edits, DerefOp,
                                 is_post ? ptr + "[" + idx + op + "]"
                                         : ptr + "[" + op + idx + "]",
                                 SM, LO);
            break;
        }

        case PointerAccessKind::DerefOffset:
        case PointerAccessKind::DerefOffsetWrite:
        {
            const auto *DerefUO =
                dyn_cast_or_null<UnaryOperator>(access.enclosing_stmt);
            if (!DerefUO)
                break;
            placed = replaceNode(edits, DerefUO, elem + access.offset_text + "]",
                                 SM, LO);
            break;
        }

        case PointerAccessKind::ArrowAccess:
        case PointerAccessKind::ArrowWrite:
        {
            const auto *ME = dyn_cast_or_null<MemberExpr>(parentOf(access.expr));
            if (!ME)
                break;
            placed = replaceNode(edits, ME, elem + "]." + access.field_name, SM, LO);
            break;
        }

        case PointerAccessKind::Subscript:
        case PointerAccessKind::SubscriptWrite:
        {
            const auto *ASE =
                dyn_cast_or_null<ArraySubscriptExpr>(parentOf(access.expr));
            if (!ASE)
                break;
            std::string text = access.subscript_text == "0"
                                   ? elem + "]"
                                   : elem + " + " + access.subscript_text + "]";
            placed = replaceNode(edits, ASE, text, SM, LO);
            break;
        }

        // ---- Position --------------------------------------------------
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
        case PointerAccessKind::Increment:
        case PointerAccessKind::Decrement:
        {
            const auto *UO = dyn_cast_or_null<UnaryOperator>(parentOf(access.expr));
            if (!UO)
                break;

            bool wrap = false;
            if (const Stmt *GP = skipTransparentParents(UO, Ctx))
            {
                if (isa<CallExpr>(GP) || isa<ReturnStmt>(GP))
                {
                    wrap = true;
                }
                else if (const auto *BO = dyn_cast<BinaryOperator>(GP))
                {
                    wrap = BO->isAssignmentOp() &&
                           BO->getRHS()->IgnoreParenImpCasts() == UO &&
                           BO->getLHS()->getType()->isPointerType();
                }
            }

            const char *op = access.kind == PointerAccessKind::Increment ? "++" : "--";
            bool is_post = UO->getOpcode() == UO_PostInc || UO->getOpcode() == UO_PostDec;
            std::string bare = is_post ? idx + op : op + idx;
            placed = replaceNode(edits, UO,
                                 wrap ? "(" + ptr + " + " + bare + ")" : bare,
                                 SM, LO);
            break;
        }

        // `p += n` — replace only the name, so an edit inside the operand
        // survives.
        case PointerAccessKind::PlusAssign:
        case PointerAccessKind::MinusAssign:
            placed = replaceNode(edits, access.expr, idx, SM, LO);
            break;

        // ---- (base, index) assignment ----------------------------------
        case PointerAccessKind::Assign:
        {
            const auto *BO = dyn_cast_or_null<BinaryOperator>(access.enclosing_stmt);
            if (!BO)
                break;

            std::string tail = ", " + idx + " = " + indexValueFor(access, transformed);
            if (assignmentValueIsUsed(BO, Ctx))
                tail += ", " + ptr + " + " + idx;
            tail += ")";

            if (access.root_expr && access.rhs_expr &&
                access.root_expr != access.rhs_expr->IgnoreParenImpCasts())
            {
                // Splitting means the right-hand side keeps only its root,
                // which is a bare name — nothing inside it needs an edit of
                // its own, so the whole assignment can be replaced at once.
                placed = replaceNode(edits, BO,
                                     "(" + ptr + " = " +
                                         getSourceText(access.root_expr, SM, LO) +
                                         tail,
                                     SM, LO);
            }
            else
            {
                // The right-hand side is kept whole and may contain
                // rewrites of its own, so bracket it with insertions
                // instead of replacing it. The index update comes last, so
                // an RHS that reads through this pointer — `p = p->next` —
                // still sees the old position.
                placed = insertBeforeNode(edits, BO, "(", SM, LO) &&
                         insertAfterNode(edits, BO, tail, SM);
            }
            break;
        }

        // ---- Value read -------------------------------------------------
        // The fallback, and the reason nothing else has to be rejected.
        case PointerAccessKind::ValueUse:
            placed = replaceNode(edits, access.expr, value, SM, LO);
            break;
        }

        if (!placed)
        {
            if (VERBOSE)
                llvm::outs() << "[Error] No file position for an access of "
                             << ptr << " at "
                             << access.loc.printToString(SM) << "\n";
            return false;
        }
    }

    if (edits.empty())
    {
        if (VERBOSE)
            llvm::outs() << "[Warning] No edits generated for " << ptr << "\n";
        return false;
    }

    if (VERBOSE)
        llvm::outs() << "[Transform] Applying " << edits.size() << " edits for "
                     << ptr << " -> " << idx << "\n";

    applyEdits(edits, SM);
    return true;
}
