// TransformationMethods.cpp — emits the actual rewrites.
//
// generateTransformation:        rewrite a single local pointer's
//                                  declaration + every use.
// generateGlobalTransformation:  same, but for a file-scope pointer
//                                  (uses the original variable name as
//                                  the index name, no _index suffix).
// applyEdits:                    apply a vector<Edit> via the Rewriter.
//
// RustSlice signature reshaping (singleton / pointer-pair / root slice
// functions) lives in xj-prepare-slicetransform; this tool only records
// the detection results into the metadata side-file.
//
// The case statements inside generateTransformation /
// generateGlobalTransformation are organized by PointerAccessKind and
// each produces one or more Edit records. Collected edits are applied
// in reverse-offset order so earlier offsets stay valid.

#include "FunctionAccessAnalyzer.h"

// Wrap a base-array text in parens if naive concatenation of "[idx]"
// onto it would parse incorrectly:
//   "data + 2"      -> "(data + 2)"     (top-level + binds looser than [])
//   "(short *) buf" -> "((short *) buf)"  ([] binds tighter than a cast)
//
// If neither hazard applies, the original text is returned unchanged.
static std::string safeBase(const std::string &base) {
    if (base.empty()) return base;

    int depth = 0;
    for (char c : base) {
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') depth--;
        else if (depth == 0 && (c == '+' || c == '-'))
            return "(" + base + ")";
    }

    // Cast detection: if the first paren group ends before the end of
    // the string, what follows is the cast target — and [] would bind
    // to it instead of the whole cast.
    if (base[0] == '(') {
        int d = 0;
        for (size_t i = 0; i < base.size(); i++) {
            if (base[i] == '(') d++;
            else if (base[i] == ')') {
                d--;
                if (d == 0 && i + 1 < base.size()) {
                    for (size_t j = i + 1; j < base.size(); j++) {
                        if (!isspace((unsigned char)base[j]))
                            return "(" + base + ")";
                    }
                    break;
                }
            }
        }
    }

    return base;
}

// ============================================================================
// generateTransformation — rewrite one local pointer (the bread-and-butter
// case). Walks the access list, builds a vector<Edit>, applies them.
// ============================================================================

bool FunctionAccessAnalyzer::generateTransformation(
    const FunctionDecl *FD,
    const VarDecl *PtrVar,
    PointerCandidate &candidate,
    std::vector<PointerAccess> &accesses,
    ASTContext &Ctx) {

    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();
    Stmt *Body = FD->getBody();

    std::string ptr_name = PtrVar->getNameAsString();
    std::string index_name = ptr_name + "_index_xj";
    std::string base_array = safeBase(candidate.base_array_text);

    // Note: even when this function was detected as a RustSlice candidate
    // (g_transformed_functions), the body is rewritten here in *plain*
    // form — base params kept, indices counted from the original base,
    // comparisons against the original len/end params. The signature-level
    // reshaping (and the corresponding body touch-ups) is performed by
    // xj-prepare-slicetransform, driven by the metadata side-file.

    std::vector<Edit> edits;

    // ---- Step 1: rewrite the pointer's declaration --------------------
    // `T *p = init;` becomes `int p_index = init;` (with init mapped
    // to the appropriate integer form: 0 for InitArray, n for
    // InitArrayOffset, -1 for InitNull). Parameter pointers are
    // handled separately in the signature-rewrite paths.
    if (!candidate.is_parameter) {
        const DeclStmt *DS = findDeclStmtForVar(PtrVar, Body);
        if (!DS) {
            if (VERBOSE)
                llvm::outs() << "[Error] Could not find DeclStmt for " << ptr_name << "\n";
            return false;
        }

        // Build the replacement declaration
        std::string init_value;
        bool found_init = false;
        for (const auto &access : accesses) {
            switch (access.kind) {
            case PointerAccessKind::InitNull:
                init_value = "-1";
                found_init = true;
                break;
            case PointerAccessKind::InitArray:
                init_value = "0";
                found_init = true;
                break;
            case PointerAccessKind::InitArrayOffset:
                init_value = access.offset_text;
                found_init = true;
                break;
            default:
                break;
            }
            if (found_init) break;
        }

        if (!found_init)
            init_value = "0";

        // Check if this DeclStmt has multiple declarators
        bool multi_decl = false;
        int decl_count = 0;
        for (const Decl *D : DS->decls()) {
            decl_count++;
            if (decl_count > 1) {
                multi_decl = true;
                break;
            }
        }

        std::string replacement = "int " + index_name + " = " + init_value + ";";

        if (multi_decl) {
            // Multi-declarator: keep the DeclStmt intact (PtrVar becomes unused)
            // and insert the index declaration on a new line after it
            SourceLocation DeclEnd = DS->getEndLoc();
            std::string indent = getIndentBeforeLoc(DS->getBeginLoc(), SM).str();

            Edit e;
            e.type = Edit::InsertAfterToken;
            e.offset = SM.getFileOffset(DeclEnd);
            e.start = DeclEnd;
            e.text = "\n" + indent + replacement;
            edits.push_back(e);
        } else {
            SourceLocation DeclStart = DS->getBeginLoc();
            SourceLocation DeclEnd = Lexer::getLocForEndOfToken(
                DS->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(DeclStart);
            e.start = DeclStart;
            e.end = DeclEnd;
            e.text = replacement;
            edits.push_back(e);
        }

    } else {
        // Parameter: insert index variable at top of function body
        // Keep the parameter itself, add int p_index = 0; after opening brace
        const CompoundStmt *CS = dyn_cast<CompoundStmt>(Body);
        if (!CS) {
            if (VERBOSE)
                llvm::outs() << "[Error] Function body is not a CompoundStmt\n";
            return false;
        }

        SourceLocation lbrace = CS->getLBracLoc();
        std::string indent = getIndentBeforeLoc(lbrace, SM).str() + "    ";
        std::string insertion = "\n" + indent + "int " + index_name + " = 0;";

        Edit e;
        e.type = Edit::InsertAfterToken;
        e.offset = SM.getFileOffset(lbrace);
        e.start = lbrace;
        e.text = insertion;
        edits.push_back(e);

        // The transformation assumes that the base array of a parameter is the parameter itself
        assert((candidate.base_array_text.empty() ||
                candidate.base_array_text == ptr_name) &&
               "parameter base override would discard a live distinct base");
        base_array = ptr_name;
    }

    // When the pointer variable is itself retained as the base array (a
    // parameter used as its own base), the offset index starts at 0 and only
    // ever advances, so its -1 sentinel never models a NULL pointer. The
    // pointer's null-ness still lives in the retained pointer variable, so
    // NULL tests (!p, p == NULL, p = NULL, if (p)) must stay on the original
    // pointer rather than be rewritten against the index. (The -1 sentinel
    // only encodes NULL in the full-replacement case where the pointer
    // variable is removed and the index becomes the whole pointer value.)
    bool ptr_retained = candidate.is_parameter && (base_array == ptr_name);

    // ---- Step 2: rewrite each access ---------------------------------
    // One case per PointerAccessKind. Each case looks up the relevant
    // AST nodes from the access record, builds the replacement text,
    // and pushes an Edit covering the right source range. Init kinds
    // were already handled by the declaration rewrite above and are
    // skipped here.
    for (const auto &access : accesses) {
        if (access.kind == PointerAccessKind::InitNull ||
            access.kind == PointerAccessKind::InitArray ||
            access.kind == PointerAccessKind::InitArrayOffset)
            continue;

        auto findParent = [&](const Expr *E) -> const Stmt * {
            return skipTransparentParents(E, Ctx);
        };
        auto findGrandParent = [&](const Stmt *P) -> const Stmt * {
            return skipTransparentParents(P, Ctx);
        };

        switch (access.kind) {

        // ---- Dereference: *p -> arr[p_index] ----
        case PointerAccessKind::Deref:
        case PointerAccessKind::DerefWrite: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *UO = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!UO) break;

            SourceLocation StarLoc = UO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                UO->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StarLoc);
            e.start = StarLoc;
            e.end = EndLoc;
            e.text = base_array + "[" + index_name + "]";
            edits.push_back(e);
            break;
        }

        // ---- Deref with post-increment: *p++ -> arr[p_index++] ----
        case PointerAccessKind::DerefPostInc: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *IncOp = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!IncOp) break;
            const Stmt *GP = findGrandParent(IncOp);
            const UnaryOperator *DerefOp = GP ? dyn_cast<UnaryOperator>(GP) : nullptr;
            if (!DerefOp) break;

            SourceLocation StartLoc = DerefOp->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                DerefOp->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = base_array + "[" + index_name + "++]";
            edits.push_back(e);
            break;
        }

        // ---- Deref with pre-increment: *++p -> arr[++p_index] ----
        case PointerAccessKind::DerefPreInc: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *IncOp = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!IncOp) break;
            const Stmt *GP = findGrandParent(IncOp);
            const UnaryOperator *DerefOp = GP ? dyn_cast<UnaryOperator>(GP) : nullptr;
            if (!DerefOp) break;

            SourceLocation StartLoc = DerefOp->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                DerefOp->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = base_array + "[++" + index_name + "]";
            edits.push_back(e);
            break;
        }

        // ---- Deref with post-decrement: *p-- -> arr[p_index--] ----
        case PointerAccessKind::DerefPostDec: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *DecOp = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!DecOp) break;
            const Stmt *GP = findGrandParent(DecOp);
            const UnaryOperator *DerefOp = GP ? dyn_cast<UnaryOperator>(GP) : nullptr;
            if (!DerefOp) break;

            SourceLocation StartLoc = DerefOp->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                DerefOp->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = base_array + "[" + index_name + "--]";
            edits.push_back(e);
            break;
        }

        // ---- Deref with pre-decrement: *--p -> arr[--p_index] ----
        case PointerAccessKind::DerefPreDec: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *DecOp = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!DecOp) break;
            const Stmt *GP = findGrandParent(DecOp);
            const UnaryOperator *DerefOp = GP ? dyn_cast<UnaryOperator>(GP) : nullptr;
            if (!DerefOp) break;

            SourceLocation StartLoc = DerefOp->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                DerefOp->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = base_array + "[--" + index_name + "]";
            edits.push_back(e);
            break;
        }

        // ---- Deref with offset: *(p + expr) -> arr[p_index + expr] ----
        case PointerAccessKind::DerefOffset:
        case PointerAccessKind::DerefOffsetWrite: {
            // enclosing_stmt holds the UO_Deref node
            const UnaryOperator *DerefUO = access.enclosing_stmt
                ? dyn_cast<UnaryOperator>(access.enclosing_stmt) : nullptr;
            if (!DerefUO) break;

            SourceLocation StartLoc = DerefUO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                DerefUO->getEndLoc(), 0, SM, LO);

            // offset_text has the arithmetic after the pointer name (e.g. " - 1")
            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = base_array + "[" + index_name + access.offset_text + "]";
            edits.push_back(e);
            break;
        }

        // ---- Arrow access: p->field -> arr[p_index].field ----
        case PointerAccessKind::ArrowAccess:
        case PointerAccessKind::ArrowWrite: {
            const Stmt *P = findParent(access.expr);
            const MemberExpr *ME = P ? dyn_cast<MemberExpr>(P) : nullptr;
            if (!ME) break;

            SourceLocation StartLoc = ME->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                ME->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = base_array + "[" + index_name + "]." + access.field_name;
            edits.push_back(e);
            break;
        }

        // ---- Subscript: p[i] -> arr[p_index + i] ----
        case PointerAccessKind::Subscript:
        case PointerAccessKind::SubscriptWrite: {
            const Stmt *P = findParent(access.expr);
            const ArraySubscriptExpr *ASE = P ? dyn_cast<ArraySubscriptExpr>(P) : nullptr;
            if (!ASE) break;

            SourceLocation StartLoc = ASE->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                ASE->getEndLoc(), 0, SM, LO);

            std::string index_expr;
            if (access.subscript_text == "0") {
                index_expr = index_name;
            } else {
                index_expr = index_name + " + " + access.subscript_text;
            }

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = base_array + "[" + index_expr + "]";
            edits.push_back(e);
            break;
        }

        // ---- Standalone increment: p++ -> p_index++ ----
        // If the increment expression's value is consumed as a pointer
        // (passed to a function, returned, assigned to a pointer var),
        // wrap the rewrite so the result type stays a pointer:
        //   func(++p) -> func(base + ++p_index)
        //   func(p++) -> func(base + p_index++)
        // Otherwise (statement context, e.g. just `p++;`) keep the
        // integer-only form. Same logic applies to Decrement below.
        //
        // Pre-increment semantics: `++p` returns the new pointer, and
        // `(base + ++p_index)` evaluates `++p_index` first (yielding
        // the new index), then adds base — same value.
        // Post-increment semantics: `p++` returns the OLD pointer,
        // and `(base + p_index++)` evaluates `p_index++` first
        // (yielding the OLD index), then adds base — same value.
        // Bug this guards against: in url.h's decode_percent,
        // `unhex(++in)` was rewritten to `unhex(++in_index)` — passing
        // an integer where a `char *` was expected. c2rust then cast
        // the int to a pointer, producing literal addresses like 0x4
        // and SIGSEGV'ing on dereference.
        case PointerAccessKind::Increment: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *UO = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!UO) break;

            bool wrap = false;
            const Stmt *GP = findGrandParent(UO);
            if (GP) {
                if (isa<CallExpr>(GP) || isa<ReturnStmt>(GP)) {
                    wrap = true;
                } else if (const auto *BO = dyn_cast<BinaryOperator>(GP)) {
                    if (BO->isAssignmentOp() &&
                        BO->getRHS()->IgnoreParenImpCasts() == UO &&
                        BO->getLHS()->getType()->isPointerType()) {
                        wrap = true;
                    }
                }
            }

            SourceLocation StartLoc = UO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                UO->getEndLoc(), 0, SM, LO);

            std::string replacement;
            if (UO->getOpcode() == UO_PostInc)
                replacement = wrap
                    ? ("(" + base_array + " + " + index_name + "++)")
                    : (index_name + "++");
            else // UO_PreInc
                replacement = wrap
                    ? ("(" + base_array + " + ++" + index_name + ")")
                    : ("++" + index_name);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = replacement;
            edits.push_back(e);
            break;
        }

        // ---- Standalone decrement: p-- -> p_index-- ----
        // Same context-aware wrapping as Increment above; see comment there.
        case PointerAccessKind::Decrement: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *UO = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!UO) break;

            bool wrap = false;
            const Stmt *GP = findGrandParent(UO);
            if (GP) {
                if (isa<CallExpr>(GP) || isa<ReturnStmt>(GP)) {
                    wrap = true;
                } else if (const auto *BO = dyn_cast<BinaryOperator>(GP)) {
                    if (BO->isAssignmentOp() &&
                        BO->getRHS()->IgnoreParenImpCasts() == UO &&
                        BO->getLHS()->getType()->isPointerType()) {
                        wrap = true;
                    }
                }
            }

            SourceLocation StartLoc = UO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                UO->getEndLoc(), 0, SM, LO);

            std::string replacement;
            if (UO->getOpcode() == UO_PostDec)
                replacement = wrap
                    ? ("(" + base_array + " + " + index_name + "--)")
                    : (index_name + "--");
            else // UO_PreDec
                replacement = wrap
                    ? ("(" + base_array + " + --" + index_name + ")")
                    : ("--" + index_name);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = replacement;
            edits.push_back(e);
            break;
        }

        // ---- Plus assign: p += n -> p_index += n ----
        case PointerAccessKind::PlusAssign: {
            // Replace just the LHS identifier (p -> p_index), leaving
            // the += and RHS intact so inner edits (e.g., function args)
            // don't overlap with this edit.
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = index_name;
            edits.push_back(e);
            break;
        }

        // ---- Minus assign: p -= n -> p_index -= n ----
        case PointerAccessKind::MinusAssign: {
            // Replace just the LHS identifier, same as PlusAssign.
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = index_name;
            edits.push_back(e);
            break;
        }

        // ---- Assign null: p = NULL -> p_index = -1 ----
        case PointerAccessKind::AssignNull: {
            // Retained pointer: `p = NULL` keeps acting on the live pointer.
            if (ptr_retained) break;
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;

            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                BO->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = index_name + " = -1";
            edits.push_back(e);
            break;
        }

        // ---- Assign &arr[i]: p = &arr[i] -> p_index = i ----
        case PointerAccessKind::AssignAddrOf: {
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;

            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                BO->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = index_name + " = " + access.offset_text;
            edits.push_back(e);
            break;
        }

        // ---- Assign arr: p = arr -> p_index = 0 ----
        case PointerAccessKind::AssignArray: {
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;

            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                BO->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = index_name + " = 0";
            edits.push_back(e);
            break;
        }

        // ---- Assign arr + offset: p = arr + off -> p_index = off ----
        case PointerAccessKind::AssignArrayOffset: {
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;

            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                BO->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = index_name + " = " + access.offset_text;
            edits.push_back(e);
            break;
        }

        // ---- Comparison null: p == NULL -> p_index == -1 ----
        // ---- Comparison expr: p < arr+n -> p_index < n ----
        case PointerAccessKind::ComparisonNull:
        case PointerAccessKind::ComparisonExpr: {
            // Retained pointer: `p == NULL` / `p != NULL` keeps testing the
            // live pointer. (ComparisonExpr is still an index comparison and
            // is rewritten as usual.)
            if (ptr_retained && access.kind == PointerAccessKind::ComparisonNull)
                break;
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;

            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                BO->getEndLoc(), 0, SM, LO);

            // offset_text has the operator, operand_text has the RHS value
            // base_text non-empty means reconstruct pointer: (base + index) <op> other
            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            if (!access.field_name.empty())
                e.text = access.field_name + " + " + index_name + " " + access.offset_text + " " + access.operand_text;
            else
                e.text = index_name + " " + access.offset_text + " " + access.operand_text;
            edits.push_back(e);
            break;
        }

        // ---- Bool true: if (p), p && ... -> p_index != -1 ----
        case PointerAccessKind::BoolTrue: {
            // Retained pointer: `if (p)` keeps testing the live pointer.
            if (ptr_retained) break;
            // Replace just the pointer DRE (source range = the pointer name token)
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = index_name + " != -1";
            edits.push_back(e);
            break;
        }

        // ---- Bool false: !p -> p_index == -1 ----
        case PointerAccessKind::BoolFalse: {
            // Retained pointer: `!p` keeps testing the live pointer.
            if (ptr_retained) break;
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *UO = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!UO) break;

            SourceLocation StartLoc = UO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                UO->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = index_name + " == -1";
            edits.push_back(e);
            break;
        }

        // ---- PassedToAllowedFunc: sscanf(p, ...) -> sscanf(base + p_index, ...) ----
        case PointerAccessKind::PassedToAllowedFunc: {
            // Check if this same CallExpr is already covered by an AssignFromAllowedFunc
            // (e.g., s = strchr(s, c) — the assignment handler covers the whole call)
            const CallExpr *CE = dyn_cast<CallExpr>(access.enclosing_stmt);
            if (CE) {
                bool covered = false;
                for (const auto &other : accesses) {
                    if (other.kind == PointerAccessKind::AssignFromAllowedFunc &&
                        other.enclosing_stmt == CE) {
                        covered = true;
                        break;
                    }
                }
                if (covered)
                    break;
            }

            // Replace the pointer arg with base + index
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = base_array + " + " + index_name;
            edits.push_back(e);
            break;
        }

        // ---- AssignFromAllowedFunc: s = strchr(s, c) -> s_index = strchr_index(base, s_index, c) ----
        case PointerAccessKind::AssignFromAllowedFunc: {
            std::string func_name = access.offset_text;  // stored in offset_text field
            std::string wrapper_name = func_name + "_index_xj";

            // Re-walk the original call's args using the now-known base.
            // The collector recorded operand_text at collect time, when
            // the candidate's base may not have been inferred yet (e.g.
            // an uninitialized local `const char *l;` whose base is only
            // learned later from `l = path + strlen(path)`). Any arg
            // that resolves to the tracked pointer or the base is
            // already passed implicitly via the wrapper's `start` and
            // `base` params — including it here would emit a duplicate
            // arg and a "too many arguments to function call" C error.
            std::string other_args;
            const CallExpr *CE = dyn_cast_or_null<CallExpr>(access.enclosing_stmt);
            if (CE) {
                for (unsigned i = 0; i < CE->getNumArgs(); i++) {
                    const Expr *Arg = CE->getArg(i)->IgnoreParenImpCasts();
                    if (const DeclRefExpr *ArgDRE = dyn_cast<DeclRefExpr>(Arg)) {
                        if (ArgDRE->getDecl() == PtrVar)
                            continue; // already passed as `start`
                        if (const VarDecl *AVD = dyn_cast<VarDecl>(ArgDRE->getDecl())) {
                            if (AVD->getNameAsString() == candidate.base_array_text)
                                continue; // already passed as `base`
                        }
                    }
                    std::string arg_text = getSourceText(CE->getArg(i), SM, LO);
                    if (!candidate.base_array_text.empty() &&
                        arg_text == candidate.base_array_text)
                        continue; // text-level fallback
                    if (!other_args.empty()) other_args += ", ";
                    other_args += arg_text;
                }
            } else {
                other_args = access.operand_text; // fallback
            }

            // Find the enclosing assignment: p = func(...)
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;

            // Build replacement: p_index = func_index(base, p_index, other_args)
            std::string replacement = index_name + " = " + wrapper_name + "(" +
                                      base_array + ", " + index_name;
            if (!other_args.empty())
                replacement += ", " + other_args;
            replacement += ")";

            // Check if the assignment is in a boolean context (for/while/if condition)
            // e.g., (s = strchr(s, c)) in a for-loop condition
            // In that case, append >= 0
            bool in_bool_context = false;
            const Stmt *AssignParent = skipTransparentParents(BO, Ctx);
            if (AssignParent) {
                if (isa<IfStmt>(AssignParent) || isa<WhileStmt>(AssignParent) ||
                    isa<ForStmt>(AssignParent) || isa<DoStmt>(AssignParent)) {
                    in_bool_context = true;
                }
                // Also check for logical operators: (s = strchr(...)) && ...
                if (const BinaryOperator *LogBO = dyn_cast<BinaryOperator>(AssignParent)) {
                    if (LogBO->getOpcode() == BO_LAnd || LogBO->getOpcode() == BO_LOr)
                        in_bool_context = true;
                }
            }

            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                BO->getEndLoc(), 0, SM, LO);

            if (in_bool_context)
                replacement = "(" + replacement + ") >= 0";

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = replacement;
            edits.push_back(e);

            // Emit wrapper function if not already emitted
            if (g_emitted_wrappers.find(wrapper_name) == g_emitted_wrappers.end()) {
                g_emitted_wrappers.insert(wrapper_name);

                // Get the pointee type from the pointer
                QualType pointeeType = PtrVar->getType()->getPointeeType();
                std::string type_str = pointeeType.getAsString();

                std::string wrapper;
                if (func_name == "strchr") {
                    wrapper = "static int strchr_index_xj(const char *base, int start, int c) {\n"
                              "    const char *result = strchr(base + start, c);\n"
                              "    if (!result) return -1;\n"
                              "    return (int)(result - base);\n"
                              "}\n\n";
                } else if (func_name == "strstr") {
                    wrapper = "static int strstr_index_xj(const char *base, int start, const char *needle) {\n"
                              "    const char *result = strstr(base + start, needle);\n"
                              "    if (!result) return -1;\n"
                              "    return (int)(result - base);\n"
                              "}\n\n";
                }

                if (!wrapper.empty()) {
                    // Insert wrapper before the function definition
                    SourceLocation FuncStart = FD->getBeginLoc();
                    Edit we;
                    we.type = Edit::InsertBefore;
                    we.offset = SM.getFileOffset(FuncStart);
                    we.start = FuncStart;
                    we.text = wrapper;
                    edits.push_back(we);
                }
            }
            break;
        }

        // ---- PassedToFunc: func(p) -> func(base + p_index) ----
        // The argument is rewritten in pointer form even when the callee
        // is a RustSlice candidate: the slice pass reads the (base +
        // index) shape back out of the AST when it rebuilds the call
        // site, and the intermediate C stays valid either way.
        case PointerAccessKind::PassedToFunc: {
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = base_array + " + " + index_name;
            edits.push_back(e);
            break;
        }

        // ---- ReturnPtr: return p -> return base + index ----
        // Always the pointer form; if the slice pass later retypes the
        // function's return as int, it also shrinks this to a bare index.
        case PointerAccessKind::ReturnPtr: {
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            e.text = base_array + " + " + index_name;
            edits.push_back(e);
            break;
        }

        default:
            break;
        }
    }

    if (edits.empty()) {
        if (VERBOSE)
            llvm::outs() << "[Warning] No edits generated for " << ptr_name << "\n";
        return false;
    }

    if (VERBOSE)
        llvm::outs() << "[Transform] Applying " << edits.size() << " edits for "
                      << ptr_name << " -> " << index_name
                      << " (base: " << base_array << ")\n";

    applyEdits(edits, SM);
    return true;
}

// ============================================================================
// generateGlobalTransformation — file-scope pointers
// ============================================================================
//
// Same shape as generateTransformation but for a global pointer:
//   - The declaration line lives at file scope, so we patch the
//     VarDecl directly rather than locating a DeclStmt.
//   - There's no enclosing function for RustSlice detection to fire.
//   - All accesses in the TU may be in different functions, but they
//     were already merged into state.accesses by run().

bool FunctionAccessAnalyzer::generateGlobalTransformation(
    const VarDecl *PtrVar,
    PointerCandidate &candidate,
    std::vector<PointerAccess> &accesses,
    ASTContext &Ctx) {

    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    std::string ptr_name = PtrVar->getNameAsString();
    std::string index_name = ptr_name + "_index_xj";
    std::string base_array = safeBase(candidate.base_array_text);

    std::vector<Edit> edits;

    // ========================================================================
    // Step 1: Replace the global pointer declaration with an index declaration
    // ========================================================================

    // Determine init value from the first init access
    std::string init_value = "0";
    for (const auto &access : accesses) {
        if (access.kind == PointerAccessKind::InitNull) {
            init_value = "-1";
            break;
        } else if (access.kind == PointerAccessKind::InitArray) {
            init_value = "0";
            break;
        } else if (access.kind == PointerAccessKind::InitArrayOffset) {
            init_value = access.offset_text;
            break;
        }
    }

    // Build replacement: preserve static/extern qualifiers
    std::string prefix;
    if (PtrVar->getStorageClass() == SC_Static)
        prefix = "static ";

    std::string replacement = prefix + "int " + index_name + " = " + init_value;

    SourceLocation DeclStart = PtrVar->getBeginLoc();
    SourceLocation DeclEnd = Lexer::getLocForEndOfToken(
        PtrVar->getEndLoc(), 0, SM, LO);

    Edit decl_edit;
    decl_edit.type = Edit::Replace;
    decl_edit.offset = SM.getFileOffset(DeclStart);
    decl_edit.start = DeclStart;
    decl_edit.end = DeclEnd;
    decl_edit.text = replacement;
    edits.push_back(decl_edit);

    // ========================================================================
    // Step 2: Replace all accesses (same logic as local pointer transformation)
    // ========================================================================

    for (const auto &access : accesses) {
        if (access.kind == PointerAccessKind::InitNull ||
            access.kind == PointerAccessKind::InitArray ||
            access.kind == PointerAccessKind::InitArrayOffset)
            continue;

        auto findParent = [&](const Expr *E) -> const Stmt * {
            return skipTransparentParents(E, Ctx);
        };
        auto findGrandParent = [&](const Stmt *P) -> const Stmt * {
            return skipTransparentParents(P, Ctx);
        };

        switch (access.kind) {

        case PointerAccessKind::Deref:
        case PointerAccessKind::DerefWrite: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *UO = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!UO) break;
            SourceLocation StartLoc = UO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(UO->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + "[" + index_name + "]"});
            break;
        }

        case PointerAccessKind::DerefPostInc: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *IncOp = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!IncOp) break;
            const Stmt *GP = findGrandParent(IncOp);
            const UnaryOperator *DerefOp = GP ? dyn_cast<UnaryOperator>(GP) : nullptr;
            if (!DerefOp) break;
            SourceLocation StartLoc = DerefOp->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(DerefOp->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + "[" + index_name + "++]"});
            break;
        }

        case PointerAccessKind::DerefPreInc: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *IncOp = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!IncOp) break;
            const Stmt *GP = findGrandParent(IncOp);
            const UnaryOperator *DerefOp = GP ? dyn_cast<UnaryOperator>(GP) : nullptr;
            if (!DerefOp) break;
            SourceLocation StartLoc = DerefOp->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(DerefOp->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + "[++" + index_name + "]"});
            break;
        }

        case PointerAccessKind::DerefPostDec: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *DecOp = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!DecOp) break;
            const Stmt *GP = findGrandParent(DecOp);
            const UnaryOperator *DerefOp = GP ? dyn_cast<UnaryOperator>(GP) : nullptr;
            if (!DerefOp) break;
            SourceLocation StartLoc = DerefOp->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(DerefOp->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + "[" + index_name + "--]"});
            break;
        }

        case PointerAccessKind::DerefPreDec: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *DecOp = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!DecOp) break;
            const Stmt *GP = findGrandParent(DecOp);
            const UnaryOperator *DerefOp = GP ? dyn_cast<UnaryOperator>(GP) : nullptr;
            if (!DerefOp) break;
            SourceLocation StartLoc = DerefOp->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(DerefOp->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + "[--" + index_name + "]"});
            break;
        }

        case PointerAccessKind::DerefOffset:
        case PointerAccessKind::DerefOffsetWrite: {
            const UnaryOperator *DerefUO = access.enclosing_stmt
                ? dyn_cast<UnaryOperator>(access.enclosing_stmt) : nullptr;
            if (!DerefUO) break;
            SourceLocation StartLoc = DerefUO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                DerefUO->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + "[" + index_name + access.offset_text + "]"});
            break;
        }

        case PointerAccessKind::ArrowAccess:
        case PointerAccessKind::ArrowWrite: {
            const Stmt *P = findParent(access.expr);
            const MemberExpr *ME = P ? dyn_cast<MemberExpr>(P) : nullptr;
            if (!ME) break;
            SourceLocation StartLoc = ME->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(ME->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + "[" + index_name + "]." + access.field_name});
            break;
        }

        case PointerAccessKind::Subscript:
        case PointerAccessKind::SubscriptWrite: {
            const Stmt *P = findParent(access.expr);
            const ArraySubscriptExpr *ASE = P ? dyn_cast<ArraySubscriptExpr>(P) : nullptr;
            if (!ASE) break;
            SourceLocation StartLoc = ASE->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(ASE->getEndLoc(), 0, SM, LO);
            std::string index_expr = (access.subscript_text == "0")
                ? index_name : index_name + " + " + access.subscript_text;
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + "[" + index_expr + "]"});
            break;
        }

        case PointerAccessKind::Increment: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *UO = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!UO) break;
            // See the longer comment on the Increment case in the
            // earlier transformer pass: when the result of `++p`/`p++`
            // is consumed as a pointer (call arg, return, assignment
            // to pointer var), wrap the rewrite as `(base + ...)` so
            // the value type stays a pointer.
            bool wrap = false;
            const Stmt *GP = findGrandParent(UO);
            if (GP) {
                if (isa<CallExpr>(GP) || isa<ReturnStmt>(GP)) {
                    wrap = true;
                } else if (const auto *BO = dyn_cast<BinaryOperator>(GP)) {
                    if (BO->isAssignmentOp() &&
                        BO->getRHS()->IgnoreParenImpCasts() == UO &&
                        BO->getLHS()->getType()->isPointerType()) {
                        wrap = true;
                    }
                }
            }
            SourceLocation StartLoc = UO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(UO->getEndLoc(), 0, SM, LO);
            std::string repl;
            if (UO->getOpcode() == UO_PostInc)
                repl = wrap
                    ? ("(" + base_array + " + " + index_name + "++)")
                    : (index_name + "++");
            else
                repl = wrap
                    ? ("(" + base_array + " + ++" + index_name + ")")
                    : ("++" + index_name);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc, repl});
            break;
        }

        case PointerAccessKind::Decrement: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *UO = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!UO) break;
            bool wrap = false;
            const Stmt *GP = findGrandParent(UO);
            if (GP) {
                if (isa<CallExpr>(GP) || isa<ReturnStmt>(GP)) {
                    wrap = true;
                } else if (const auto *BO = dyn_cast<BinaryOperator>(GP)) {
                    if (BO->isAssignmentOp() &&
                        BO->getRHS()->IgnoreParenImpCasts() == UO &&
                        BO->getLHS()->getType()->isPointerType()) {
                        wrap = true;
                    }
                }
            }
            SourceLocation StartLoc = UO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(UO->getEndLoc(), 0, SM, LO);
            std::string repl;
            if (UO->getOpcode() == UO_PostDec)
                repl = wrap
                    ? ("(" + base_array + " + " + index_name + "--)")
                    : (index_name + "--");
            else
                repl = wrap
                    ? ("(" + base_array + " + --" + index_name + ")")
                    : ("--" + index_name);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc, repl});
            break;
        }

        case PointerAccessKind::PlusAssign: {
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(access.expr->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc, index_name});
            break;
        }

        case PointerAccessKind::MinusAssign: {
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(access.expr->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc, index_name});
            break;
        }

        case PointerAccessKind::AssignNull: {
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;
            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(BO->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             index_name + " = -1"});
            break;
        }

        case PointerAccessKind::AssignAddrOf: {
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;
            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(BO->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             index_name + " = " + access.offset_text});
            break;
        }

        case PointerAccessKind::AssignArray: {
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;
            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(BO->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             index_name + " = 0"});
            break;
        }

        case PointerAccessKind::AssignArrayOffset: {
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;
            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(BO->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             index_name + " = " + access.offset_text});
            break;
        }

        case PointerAccessKind::ComparisonNull:
        case PointerAccessKind::ComparisonExpr: {
            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;
            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(BO->getEndLoc(), 0, SM, LO);
            if (!access.field_name.empty())
                edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                                 StartLoc, EndLoc,
                                 access.field_name + " + " + index_name + " " + access.offset_text + " " + access.operand_text});
            else
                edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                                 StartLoc, EndLoc,
                                 index_name + " " + access.offset_text + " " + access.operand_text});
            break;
        }

        case PointerAccessKind::BoolTrue: {
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             index_name + " != -1"});
            break;
        }

        case PointerAccessKind::BoolFalse: {
            const Stmt *P = findParent(access.expr);
            const UnaryOperator *UO = P ? dyn_cast<UnaryOperator>(P) : nullptr;
            if (!UO) break;
            SourceLocation StartLoc = UO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                UO->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             index_name + " == -1"});
            break;
        }

        case PointerAccessKind::PassedToAllowedFunc: {
            const CallExpr *CE = dyn_cast<CallExpr>(access.enclosing_stmt);
            if (CE) {
                bool covered = false;
                for (const auto &other : accesses) {
                    if (other.kind == PointerAccessKind::AssignFromAllowedFunc &&
                        other.enclosing_stmt == CE) {
                        covered = true;
                        break;
                    }
                }
                if (covered)
                    break;
            }
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + " + " + index_name});
            break;
        }

        case PointerAccessKind::AssignFromAllowedFunc: {
            std::string func_name = access.offset_text;
            std::string wrapper_name = func_name + "_index_xj";
            std::string other_args = access.operand_text;

            const Stmt *P = findParent(access.expr);
            const BinaryOperator *BO = P ? dyn_cast<BinaryOperator>(P) : nullptr;
            if (!BO) break;

            std::string replacement = index_name + " = " + wrapper_name + "(" +
                                      base_array + ", " + index_name;
            if (!other_args.empty())
                replacement += ", " + other_args;
            replacement += ")";

            bool in_bool_context = false;
            const Stmt *AssignParent = skipTransparentParents(BO, Ctx);
            if (AssignParent) {
                if (isa<IfStmt>(AssignParent) || isa<WhileStmt>(AssignParent) ||
                    isa<ForStmt>(AssignParent) || isa<DoStmt>(AssignParent))
                    in_bool_context = true;
                if (const BinaryOperator *LogBO = dyn_cast<BinaryOperator>(AssignParent)) {
                    if (LogBO->getOpcode() == BO_LAnd || LogBO->getOpcode() == BO_LOr)
                        in_bool_context = true;
                }
            }

            SourceLocation StartLoc = BO->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                BO->getEndLoc(), 0, SM, LO);

            if (in_bool_context)
                replacement = "(" + replacement + ") >= 0";

            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc, replacement});
            break;
        }

        case PointerAccessKind::PassedToFunc: {
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + " + " + index_name});
            break;
        }

        case PointerAccessKind::ReturnPtr: {
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);
            edits.push_back({Edit::Replace, SM.getFileOffset(StartLoc),
                             StartLoc, EndLoc,
                             base_array + " + " + index_name});
            break;
        }

        default:
            break;
        }
    }

    if (edits.empty()) {
        if (VERBOSE)
            llvm::outs() << "[Warning] No edits generated for global " << ptr_name << "\n";
        return false;
    }

    if (VERBOSE)
        llvm::outs() << "[Transform] Applying " << edits.size()
                      << " edits for global " << ptr_name << " -> " << index_name
                      << " (base: " << base_array << ")\n";

    applyEdits(edits, SM);
    return true;
}
