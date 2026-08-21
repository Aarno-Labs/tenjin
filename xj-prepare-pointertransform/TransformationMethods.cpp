// TransformationMethods.cpp — where a pointer's index declaration goes.
//
// There is only one shape to emit:
//
//     T *p            stays exactly where it is, holding a base
//     int p_index_xj  declared beside it, holding the position
//
// so locals, parameters and file-scope pointers differ in nothing but the
// position of that second line. How each *use* of the pointer is rewritten
// is not decided here — that is EditPlan, which has to see every rewrite in
// the translation unit at once to keep them from colliding.
//
// Finding the position and writing the declaration are separate steps, and
// deliberately so. A pointer whose index has nowhere legal to live is not
// rewritten at all, and that has to be settled *before* any other pointer
// pairs its index with this one's — otherwise `q_index_xj = p_index_xj + 1`
// can name a declaration that was never emitted.

#include "FunctionAccessAnalyzer.h"

#include "EditPlan.h"

// ============================================================================
// Placing an index declaration
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

// The whole of a for-init index's placement: insert before the loop, and
// close a brace after it when the loop had to be wrapped. Returns false if
// there is no position that can hold the declaration.
//
// Two pointers wrapping the same loop nest correctly with no coordination:
// InsertTextBefore places a later insertion *first* at a location and
// InsertTextAfterToken places it *last*, so the second pointer's braces
// enclose the first's.
static bool forInitSite(const ForStmt *FS, ASTContext &Ctx, IndexDeclSite &site) {
    const SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    auto Parents = Ctx.getParents(*FS);
    bool needs_braces = Parents.empty() || !Parents[0].get<CompoundStmt>();

    SourceLocation Start = FS->getBeginLoc();
    std::string indent = getIndentBeforeLoc(Start, SM).str();

    site.at = Start;
    site.prefix = needs_braces ? "{ " : "";
    site.suffix = "\n" + indent;

    if (!needs_braces) {
        site.valid = true;
        return true;
    }

    // The closing brace goes after the statement's terminating semicolon,
    // which is not part of the statement when its last token is an
    // expression: `for (...) s += *p;` ends at `p`. Stopping there would
    // emit `s += p[i]} ;`, and in an if/else it would strand the `else`.
    SourceLocation End = FS->getEndLoc();
    Token last;
    bool ends_at_terminator =
        !Lexer::getRawToken(End, last, SM, LO, /*IgnoreWhiteSpace=*/true) &&
        (last.is(tok::r_brace) || last.is(tok::semi));
    if (!ends_at_terminator) {
        auto next = Lexer::findNextToken(End, SM, LO);
        if (!next || !next->is(tok::semi))
            return false;
        End = next->getLocation();
    }

    site.brace_at = End;
    site.brace_text = "\n" + indent + "}";
    site.valid = true;
    return true;
}

bool findIndexDeclSite(const FunctionDecl *FD, const VarDecl *PtrVar,
                       const PointerCandidate &candidate, ASTContext &Ctx,
                       IndexDeclSite &site) {
    const SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    if (candidate.is_parameter) {
        // The parameter arrives holding its base; the index starts at 0.
        const auto *CS = FD ? dyn_cast_or_null<CompoundStmt>(FD->getBody()) : nullptr;
        if (!CS) {
            if (VERBOSE)
                llvm::outs() << "[Error] Function body is not a CompoundStmt\n";
            return false;
        }
        SourceLocation lbrace = CS->getLBracLoc();
        // InsertBefore at the position just past `{`, rather than
        // InsertAfterToken on `{` itself: pointers are planned in reverse
        // source order (see collectCandidates), and only InsertBefore
        // stacks a later insertion ahead of an earlier one, which puts the
        // declarations back in source order.
        site.at = Lexer::getLocForEndOfToken(lbrace, 0, SM, LO);
        site.prefix = "\n" + getIndentBeforeLoc(lbrace, SM).str() + "    ";
        site.valid = true;
        return true;
    }

    if (PtrVar->hasGlobalStorage()) {
        // File scope: the index is a file-scope int with the same linkage.
        auto semi = Lexer::findNextToken(PtrVar->getEndLoc(), SM, LO);
        if (!semi || !semi->is(tok::semi)) {
            if (VERBOSE)
                llvm::outs() << "[Error] No terminator after global "
                             << PtrVar->getNameAsString() << "\n";
            return false;
        }
        site.at = Lexer::getLocForEndOfToken(semi->getLocation(), 0, SM, LO);
        site.prefix =
            std::string("\n") + (PtrVar->getStorageClass() == SC_Static ? "static " : "");
        site.valid = true;
        return true;
    }

    const DeclStmt *DS = FD ? findDeclStmtForVar(PtrVar, FD->getBody()) : nullptr;
    if (!DS) {
        if (VERBOSE)
            llvm::outs() << "[Error] Could not find DeclStmt for "
                         << PtrVar->getNameAsString() << "\n";
        return false;
    }

    if (const ForStmt *FS = forStmtInitializedBy(DS, Ctx)) {
        if (!forInitSite(FS, Ctx, site)) {
            if (VERBOSE)
                llvm::outs() << "[Error] No position for the index of "
                             << PtrVar->getNameAsString() << "\n";
            return false;
        }
        return true;
    }

    // On the next line, where every name the declaration binds — including
    // a sibling declarator's index — is already in scope. InsertBefore at
    // the position past the terminator rather than InsertAfterToken on it:
    // pointers are planned in reverse source order, and only InsertBefore
    // stacks a later insertion ahead of an earlier one, which is what puts
    // two indices sharing this anchor back in source order.
    site.at = Lexer::getLocForEndOfToken(DS->getEndLoc(), 0, SM, LO);
    site.prefix = "\n" + getIndentBeforeLoc(DS->getBeginLoc(), SM).str();
    site.valid = true;
    return true;
}

// ============================================================================
// Writing the declaration
// ============================================================================

void emitIndexDecl(const IndexDeclSite &site, const VarDecl *PtrVar,
                   const std::string &index_init, const SourceManager &SM,
                   std::vector<Edit> &edits) {
    std::string decl = "int " + indexNameFor(PtrVar) + " = " + index_init + ";";

    Edit open;
    open.type = Edit::InsertBefore;
    open.offset = SM.getFileOffset(site.at);
    open.start = site.at;
    open.text = site.prefix + decl + site.suffix;
    edits.push_back(open);

    if (site.brace_at.isInvalid())
        return;

    Edit close;
    close.type = Edit::InsertAfterToken;
    close.offset = SM.getFileOffset(site.brace_at);
    close.start = site.brace_at;
    close.text = site.brace_text;
    edits.push_back(close);
}
