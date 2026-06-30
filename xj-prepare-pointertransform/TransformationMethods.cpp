// TransformationMethods.cpp — emits the actual rewrites.
//
// generateTransformation:        rewrite a single local pointer's
//                                  declaration + every use.
// generateGlobalTransformation:  same, but for a file-scope pointer
//                                  (uses the original variable name as
//                                  the index name, no _index suffix).
// generateSingletonTransformation:  rewrite a swap-style function whose
//                                  pointer params don't iterate.
// generatePointerPairTransformation: rewrite a (lo, hi) function whose
//                                  iteration is recursive.
// applyEdits:                    apply a vector<Edit> via the Rewriter.
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
    std::string original_base_array = candidate.base_array_text; // before any RustSlice rewrite

    // If this function has already been registered as a RustSlice
    // transform and the local pointer's base happens to be the now-
    // removed base parameter, retarget the base to arr.ptr (with
    // lookback adjustment if any).
    {
        auto rs_check = g_transformed_functions.find(FD->getCanonicalDecl());
        if (rs_check != g_transformed_functions.end()) {
            const RustSliceInfo &rs = rs_check->second;
            if (rs.base_param_index >= 0 &&
                (int)FD->getNumParams() > rs.base_param_index) {
                const ParmVarDecl *base_pd =
                    FD->getParamDecl(rs.base_param_index);
                if (base_pd->getNameAsString() == base_array) {
                    if (rs.lookback > 0)
                        base_array = rs.slice_param_name + ".ptr + " +
                                     std::to_string(rs.lookback);
                    else
                        base_array = rs.slice_param_name + ".ptr";
                }
            }
        }
    }

    std::vector<Edit> edits;

    // ---- Re-derive the RustSlice info for this pointer ---------------
    // Only enter this branch if detection already approved the
    // function. detectAllTransformations is the source of truth for
    // whether a RustSlice rewrite is safe (it rejects functions that
    // would produce broken output, e.g. ones with non-rewriteable
    // pointers referencing the removed params). If we re-detected
    // here we'd produce a half-rewrite: the signature gets changed
    // but emitTypedefs / rewriteCallSites / replaceRemovedParams all
    // gate on g_transformed_functions and would skip the function.
    bool is_rust_slice = false;
    RustSliceInfo slice_info;

    if (g_transformed_functions.count(FD->getCanonicalDecl()) &&
        !candidate.is_parameter && FD->getNumParams() > 0) {
        // Check if original base_array matches a function parameter
        for (unsigned i = 0; i < FD->getNumParams(); i++) {
            const ParmVarDecl *P = FD->getParamDecl(i);
            if (P->getNameAsString() == original_base_array && P->getType()->isPointerType()) {
                slice_info.base_param_index = i;
                QualType pointeeType = P->getType()->getPointeeType();
                slice_info.pointee_type = pointeeType.getUnqualifiedType().getAsString();
                break;
            }
        }

        if (slice_info.base_param_index >= 0) {
            // Find the length source from ComparisonExpr accesses
            for (const auto &access : accesses) {
                if (access.kind == PointerAccessKind::ComparisonExpr) {
                    std::string op_text = access.operand_text;

                    // Case 1: pointer pair "(end - base)"
                    std::string pair_suffix = " - " + original_base_array + ")";
                    if (op_text.size() > pair_suffix.size() &&
                        op_text.front() == '(' &&
                        op_text.substr(op_text.size() - pair_suffix.size()) == pair_suffix) {
                        std::string end_name = op_text.substr(1, op_text.size() - pair_suffix.size() - 1);
                        for (unsigned i = 0; i < FD->getNumParams(); i++) {
                            if (FD->getParamDecl(i)->getNameAsString() == end_name) {
                                slice_info.end_param_index = i;
                                is_rust_slice = true;
                                break;
                            }
                        }
                    }
                    // Case 2: simple param name (e.g., "n")
                    else {
                        for (unsigned i = 0; i < FD->getNumParams(); i++) {
                            if (FD->getParamDecl(i)->getNameAsString() == op_text &&
                                !FD->getParamDecl(i)->getType()->isPointerType()) {
                                slice_info.len_param_index = i;
                                is_rust_slice = true;
                                break;
                            }
                        }
                    }
                    break; // only need first ComparisonExpr
                }
            }
        }

        if (is_rust_slice) {
            slice_info.slice_param_name = "arr";
            slice_info.slice_type = makeSliceTypeName(slice_info.pointee_type);
            slice_info.lookback = -candidate.min_relative_offset;
            slice_info.lookahead = candidate.max_relative_offset;

            // Pull `inclusive_end` from any earlier detection so the
            // ComparisonExpr rewrite below uses the right adjustment.
            {
                auto tf_it = g_transformed_functions.find(FD->getCanonicalDecl());
                if (tf_it != g_transformed_functions.end())
                    slice_info.inclusive_end = tf_it->second.inclusive_end;
            }

            // The base now lives in arr.ptr; comparisons that bounded
            // the pointer against `n` or `(end - base)` become bounds
            // against arr.len. Inclusive end semantics (hi is
            // dereferenced) shift the bound down by 1; lookahead
            // shifts it down further so the slice still covers
            // *(p + lookahead).
            base_array = slice_info.slice_param_name + ".ptr";

            for (auto &access : accesses) {
                if (access.kind == PointerAccessKind::ComparisonExpr) {
                    int adjustment = slice_info.lookahead;
                    if (slice_info.inclusive_end)
                        adjustment += 1;
                    if (adjustment > 0)
                        access.operand_text = slice_info.slice_param_name + ".len - " +
                                              std::to_string(adjustment);
                    else
                        access.operand_text = slice_info.slice_param_name + ".len";
                }
            }

            if (VERBOSE)
                llvm::outs() << "[RustSlice] Detected for " << ptr_name
                             << " -> " << slice_info.slice_type
                             << " (lookback=" << slice_info.lookback
                             << ", lookahead=" << slice_info.lookahead << ")\n";
        }
    }

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

        // Translate param references in init_value for pointer-pair functions
        // e.g., "(hi - lo) / 2" → "arr.len / 2"
        {
            auto tf_it = g_transformed_functions.find(FD->getCanonicalDecl());
            if (tf_it != g_transformed_functions.end()) {
                const RustSliceInfo &rs = tf_it->second;
                if (rs.base_param_index >= 0 && rs.end_param_index >= 0) {
                    std::string base_name = FD->getParamDecl(rs.base_param_index)->getNameAsString();
                    std::string end_name = FD->getParamDecl(rs.end_param_index)->getNameAsString();
                    std::string slice = rs.slice_param_name;

                    // Replace "end - base" pattern with ".len" (or ".len - 1" for inclusive)
                    std::string sub_pat = end_name + " - " + base_name;
                    std::string sub_repl = slice + ".len";
                    if (rs.inclusive_end)
                        sub_repl = "(" + slice + ".len - 1)";
                    size_t pos = init_value.find(sub_pat);
                    while (pos != std::string::npos) {
                        // Check if surrounded by parens that can be removed
                        bool has_parens = pos > 0 && init_value[pos - 1] == '(' &&
                                         pos + sub_pat.size() < init_value.size() &&
                                         init_value[pos + sub_pat.size()] == ')';
                        if (has_parens) {
                            init_value.replace(pos - 1, sub_pat.size() + 2, sub_repl);
                            pos = init_value.find(sub_pat, pos);
                        } else {
                            init_value.replace(pos, sub_pat.size(), sub_repl);
                            pos = init_value.find(sub_pat, pos + sub_repl.size());
                        }
                    }

                    // Replace remaining individual param references
                    // base_param → slice.ptr (should not normally remain after sub pattern)
                    // end_param → slice.len
                    auto replaceWord = [](std::string &s, const std::string &word,
                                          const std::string &replacement) {
                        size_t pos = 0;
                        while ((pos = s.find(word, pos)) != std::string::npos) {
                            // Check word boundaries
                            bool before_ok = (pos == 0 || !isalnum(s[pos-1]) && s[pos-1] != '_');
                            bool after_ok = (pos + word.size() >= s.size() ||
                                            (!isalnum(s[pos + word.size()]) && s[pos + word.size()] != '_'));
                            if (before_ok && after_ok) {
                                s.replace(pos, word.size(), replacement);
                                pos += replacement.size();
                            } else {
                                pos += word.size();
                            }
                        }
                    };
                    replaceWord(init_value, base_name, slice + ".ptr");
                    replaceWord(init_value, end_name, slice + ".len");
                }
            }
        }

        // Adjust init value for lookback
        if (is_rust_slice && slice_info.lookback > 0) {
            if (init_value == "0")
                init_value = std::to_string(slice_info.lookback);
            else
                init_value = init_value + " + " + std::to_string(slice_info.lookback);
        }

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

        // For parameters, the base array is the parameter name itself
        // Accesses become param[param_index] style
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
        // Skip if the callee is a transformed function (handled by rewriteCallSites)
        case PointerAccessKind::PassedToFunc: {
            // Check if the callee is in g_transformed_functions
            bool callee_transformed = false;
            if (access.enclosing_stmt) {
                if (const CallExpr *CE = dyn_cast<CallExpr>(access.enclosing_stmt)) {
                    if (const FunctionDecl *Callee = CE->getDirectCallee()) {
                        if (g_transformed_functions.count(Callee->getCanonicalDecl()))
                            callee_transformed = true;
                    }
                }
            }
            if (callee_transformed)
                break; // rewriteCallSites will handle this

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

        // ---- ReturnPtr: return p -> return index or base + index ----
        case PointerAccessKind::ReturnPtr: {
            SourceLocation StartLoc = access.expr->getBeginLoc();
            SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                access.expr->getEndLoc(), 0, SM, LO);

            Edit e;
            e.type = Edit::Replace;
            e.offset = SM.getFileOffset(StartLoc);
            e.start = StartLoc;
            e.end = EndLoc;
            // If return type is changing from pointer to int, just return the index
            auto rs_it = g_transformed_functions.find(FD->getCanonicalDecl());
            if (rs_it != g_transformed_functions.end() && rs_it->second.return_type_changed)
                e.text = index_name;
            else
                e.text = base_array + " + " + index_name;
            edits.push_back(e);
            break;
        }

        default:
            break;
        }
    }

    // ---- Step 3: RustSlice signature rewrite -------------------------
    // For root-RustSlice functions, replace the entire parameter list
    // with `slice_type slice_param_name, <other params>` and, if
    // detection saw `return p`, also retype the return as int.
    if (is_rust_slice) {
        // The typedef itself is emitted later by emitTypedefs().
        {
            auto tf_it = g_transformed_functions.find(FD->getCanonicalDecl());
            if (tf_it != g_transformed_functions.end())
                slice_info.inclusive_end = tf_it->second.inclusive_end;
        }

        std::string new_params = slice_info.slice_type + " " + slice_info.slice_param_name;
        for (unsigned i = 0; i < FD->getNumParams(); i++) {
            if ((int)i == slice_info.base_param_index) continue;
            if ((int)i == slice_info.end_param_index) continue;
            if ((int)i == slice_info.len_param_index) continue;
            const ParmVarDecl *P = FD->getParamDecl(i);
            new_params += ", " + P->getType().getAsString() + " " + P->getNameAsString();
        }

        SourceLocation FirstParamLoc = FD->getParamDecl(0)->getBeginLoc();
        unsigned LastIdx = FD->getNumParams() - 1;
        SourceLocation LastParamEnd = Lexer::getLocForEndOfToken(
            FD->getParamDecl(LastIdx)->getEndLoc(), 0, SM, LO);

        Edit sig;
        sig.type = Edit::Replace;
        sig.offset = SM.getFileOffset(FirstParamLoc);
        sig.start = FirstParamLoc;
        sig.end = LastParamEnd;
        sig.text = new_params;
        edits.push_back(sig);

        // Return type rewrite, if detection flagged it.
        {
            auto tf_it = g_transformed_functions.find(FD->getCanonicalDecl());
            if (tf_it != g_transformed_functions.end())
                slice_info.return_type_changed = tf_it->second.return_type_changed;
        }
        if (slice_info.return_type_changed) {
            SourceLocation RetStart = FD->getBeginLoc();
            SourceLocation FuncNameLoc = FD->getLocation();
            Edit ret_edit;
            ret_edit.type = Edit::Replace;
            ret_edit.offset = SM.getFileOffset(RetStart);
            ret_edit.start = RetStart;
            ret_edit.end = FuncNameLoc;
            ret_edit.text = "int ";
            edits.push_back(ret_edit);
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

// ============================================================================
// generateSingletonTransformation — swap-style functions
// ============================================================================
//
// For functions whose pointer parameters are dereferenced but never
// iterated (e.g. swap(int *a, int *b)), we add a shared RustSlice
// param and turn each pointer param into an int index. Inside the
// body, every `*param` becomes `arr.ptr[param]`.
//
//   void swap(int *a, int *b)
//     → void swap(RustSlice_int arr, int a, int b)
//        with body: tmp = arr.ptr[a]; arr.ptr[a] = arr.ptr[b]; ...

bool FunctionAccessAnalyzer::generateSingletonTransformation(
    const FunctionDecl *FD,
    const RustSliceInfo &slice_info,
    FunctionAnalysis &analysis,
    ASTContext &Ctx) {

    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    std::vector<Edit> edits;
    std::string arr_name = slice_info.slice_param_name;

    // Walk every dereference of a singleton param and rewrite it.
    for (int pi : slice_info.singleton_param_indices) {
        const ParmVarDecl *P = FD->getParamDecl(pi);
        auto acc_it = analysis.accesses.find(P);
        if (acc_it == analysis.accesses.end())
            continue;

        std::string param_name = P->getNameAsString();

        for (const auto &access : acc_it->second) {
            auto findParent = [&](const Expr *E) -> const Stmt * {
                return skipTransparentParents(E, Ctx);
            };

            switch (access.kind) {
            case PointerAccessKind::Deref:
            case PointerAccessKind::DerefWrite: {
                const Stmt *Par = findParent(access.expr);
                const UnaryOperator *UO = Par ? dyn_cast<UnaryOperator>(Par) : nullptr;
                if (!UO) break;

                SourceLocation StartLoc = UO->getBeginLoc();
                SourceLocation EndLoc = Lexer::getLocForEndOfToken(
                    UO->getEndLoc(), 0, SM, LO);

                Edit e;
                e.type = Edit::Replace;
                e.offset = SM.getFileOffset(StartLoc);
                e.start = StartLoc;
                e.end = EndLoc;
                e.text = arr_name + ".ptr[" + param_name + "]";
                edits.push_back(e);
                break;
            }
            default:
                break;
            }
        }
    }

    // Rewrite the parameter list: prepend the slice, swap each
    // singleton T* for an int with the same name, leave other params
    // alone. The typedef is emitted later by emitTypedefs().
    std::string new_params = slice_info.slice_type + " " + arr_name;
    for (unsigned i = 0; i < FD->getNumParams(); i++) {
        const ParmVarDecl *P = FD->getParamDecl(i);
        if (std::find(slice_info.singleton_param_indices.begin(),
                      slice_info.singleton_param_indices.end(),
                      (int)i) != slice_info.singleton_param_indices.end()) {
            new_params += ", int " + P->getNameAsString();
        } else {
            new_params += ", " + P->getType().getAsString() + " " + P->getNameAsString();
        }
    }

    SourceLocation FirstParamLoc = FD->getParamDecl(0)->getBeginLoc();
    unsigned LastIdx = FD->getNumParams() - 1;
    SourceLocation LastParamEnd = Lexer::getLocForEndOfToken(
        FD->getParamDecl(LastIdx)->getEndLoc(), 0, SM, LO);

    Edit sig;
    sig.type = Edit::Replace;
    sig.offset = SM.getFileOffset(FirstParamLoc);
    sig.start = FirstParamLoc;
    sig.end = LastParamEnd;
    sig.text = new_params;
    edits.push_back(sig);

    if (edits.empty())
        return false;

    if (VERBOSE)
        llvm::outs() << "[Transform] Applying " << edits.size()
                      << " singleton edits for " << FD->getNameAsString() << "\n";

    applyEdits(edits, SM);
    return true;
}

// ============================================================================
// generatePointerPairTransformation — non-iterating (lo, hi) functions
// ============================================================================
//
// quick_sort(int *lo, int *hi) → quick_sort(RustSlice_int arr)
//
// These functions don't have a local iterating pointer (so
// generateTransformation isn't the right fit) — their iteration is
// recursive or via callees. We just need to:
//   - Replace the two pointer params with a single slice.
//   - Optionally retype the return as int.
//   - Patch up any locals that take their value from a return-type-
//     changed callee (e.g. `int *p = partition(lo, hi)` becomes
//     `int p = partition(arr)`).

bool FunctionAccessAnalyzer::generatePointerPairTransformation(
    const FunctionDecl *FD,
    FunctionAnalysis &analysis,
    ASTContext &Ctx) {

    SourceManager &SM = Ctx.getSourceManager();
    const LangOptions &LO = Ctx.getLangOpts();

    // Find the first two pointer parameters; they're the (lo, hi) pair.
    int base_idx = -1, end_idx = -1;
    std::string pointee_type;

    for (unsigned i = 0; i < FD->getNumParams(); i++) {
        const ParmVarDecl *P = FD->getParamDecl(i);
        if (P->getType()->isPointerType()) {
            QualType pt = P->getType()->getPointeeType();
            if (base_idx < 0) {
                base_idx = i;
                pointee_type = pt.getUnqualifiedType().getAsString();
            } else if (end_idx < 0) {
                end_idx = i;
            }
        }
    }

    if (base_idx < 0 || end_idx < 0)
        return false;

    // If the end pointer is dereferenced anywhere in the body, it's
    // an inclusive endpoint (e.g. `[lo, hi]`), so the slice length is
    // hi - lo + 1 rather than hi - lo.
    const ParmVarDecl *EndParam = FD->getParamDecl(end_idx);
    bool inclusive_end = false;
    auto end_it = analysis.accesses.find(EndParam);
    if (end_it != analysis.accesses.end()) {
        for (const auto &acc : end_it->second) {
            if (acc.kind == PointerAccessKind::Deref ||
                acc.kind == PointerAccessKind::DerefWrite) {
                inclusive_end = true;
                break;
            }
        }
    }

    std::string slice_type = makeSliceTypeName(pointee_type);
    std::string arr_name = "arr";

    auto tf_it = g_transformed_functions.find(FD->getCanonicalDecl());
    bool return_changed = tf_it != g_transformed_functions.end() &&
                           tf_it->second.return_type_changed;

    std::vector<Edit> edits;

    // Replace the parameter list. The typedef is emitted separately
    // by emitTypedefs().
    std::string new_params = slice_type + " " + arr_name;
    for (unsigned i = 0; i < FD->getNumParams(); i++) {
        if ((int)i == base_idx || (int)i == end_idx)
            continue;
        const ParmVarDecl *P = FD->getParamDecl(i);
        new_params += ", " + P->getType().getAsString() + " " + P->getNameAsString();
    }

    SourceLocation FirstParamLoc = FD->getParamDecl(0)->getBeginLoc();
    unsigned LastIdx = FD->getNumParams() - 1;
    SourceLocation LastParamEnd = Lexer::getLocForEndOfToken(
        FD->getParamDecl(LastIdx)->getEndLoc(), 0, SM, LO);

    Edit sig;
    sig.type = Edit::Replace;
    sig.offset = SM.getFileOffset(FirstParamLoc);
    sig.start = FirstParamLoc;
    sig.end = LastParamEnd;
    sig.text = new_params;
    edits.push_back(sig);

    if (return_changed) {
        SourceLocation RetStart = FD->getBeginLoc();
        SourceLocation FuncNameLoc = FD->getLocation();
        Edit ret_edit;
        ret_edit.type = Edit::Replace;
        ret_edit.offset = SM.getFileOffset(RetStart);
        ret_edit.start = RetStart;
        ret_edit.end = FuncNameLoc;
        ret_edit.text = "int ";
        edits.push_back(ret_edit);
    }

    applyEdits(edits, SM);

    // Mark the parameter range as already edited so the body-rewrite
    // pass below doesn't try to touch it again.
    m_edited_ranges.clear();
    m_edited_ranges.push_back({SM.getFileOffset(FirstParamLoc),
                                SM.getFileOffset(LastParamEnd)});

    // Hunt for locals that take their value from a callee whose
    // return type was rewritten to int. Those locals stop being
    // pointers — `int *p = partition(lo, hi)` becomes
    // `int p = partition(arr)` — so we mark them in g_index_return_vars
    // and skip them in the normal local-pointer rewrite below.
    std::set<const VarDecl *> skip_vars;
    for (auto &[PtrVar, accesses] : analysis.accesses) {
        auto &candidate = analysis.tracked_pointers[PtrVar];
        if (candidate.is_parameter)
            continue;
        if (!PtrVar->getType()->isPointerType())
            continue;

        // Check if initialized from a call to a return-type-changed function
        if (PtrVar->hasInit()) {
            const Expr *Init = PtrVar->getInit()->IgnoreImpCasts();
            if (const CallExpr *CE = dyn_cast<CallExpr>(Init)) {
                if (const FunctionDecl *Callee = CE->getDirectCallee()) {
                    auto ci = g_transformed_functions.find(Callee->getCanonicalDecl());
                    if (ci != g_transformed_functions.end() &&
                        ci->second.return_type_changed) {
                        // Change declaration: int *p → int p
                        const DeclStmt *DS = findDeclStmtForVar(PtrVar, FD->getBody());
                        if (DS) {
                            SourceLocation TypeStart = DS->getBeginLoc();
                            SourceLocation NameEnd = Lexer::getLocForEndOfToken(
                                PtrVar->getLocation(), 0, SM, LO);
                            Edit type_edit;
                            type_edit.type = Edit::Replace;
                            type_edit.offset = SM.getFileOffset(TypeStart);
                            type_edit.start = TypeStart;
                            type_edit.end = NameEnd;
                            type_edit.text = "int " + PtrVar->getNameAsString();
                            std::vector<Edit> type_edits = {type_edit};
                            applyEdits(type_edits, SM);
                        }
                        g_index_return_vars.insert(PtrVar);
                        skip_vars.insert(PtrVar);
                    }
                }
            }
        }
    }

    for (auto &[PtrVar, accesses] : analysis.accesses) {
        auto &candidate = analysis.tracked_pointers[PtrVar];
        if (candidate.is_parameter)
            continue;
        if (skip_vars.count(PtrVar))
            continue;

        if (candidate.base_array_text ==
            FD->getParamDecl(base_idx)->getNameAsString()) {
            candidate.base_array_text = arr_name + ".ptr";
        }

        // Update ComparisonExpr operand_text: (end - base) → arr.len
        {
            std::string base_pname = FD->getParamDecl(base_idx)->getNameAsString();
            std::string end_pname = FD->getParamDecl(end_idx)->getNameAsString();
            std::string sub_pat = "(" + end_pname + " - " + base_pname + ")";
            bool incl = (tf_it != g_transformed_functions.end())
                            ? tf_it->second.inclusive_end
                            : inclusive_end;
            for (auto &acc : accesses) {
                if (acc.kind == PointerAccessKind::ComparisonExpr &&
                    acc.operand_text == sub_pat) {
                    int adj = candidate.max_relative_offset;
                    if (incl)
                        adj += 1;
                    if (adj > 0)
                        acc.operand_text =
                            arr_name + ".len - " + std::to_string(adj);
                    else
                        acc.operand_text = arr_name + ".len";
                }
            }
        }

        transformPointerVar(FD, PtrVar, candidate, accesses, Ctx);
    }

    // Replace remaining param references
    replaceRemovedParams(FD, Ctx);

    // Source-text fallback: replace any remaining param names that the AST-based
    // approach missed (e.g., when NULL is undefined and Clang drops AST nodes)
    {
        auto tf_it = g_transformed_functions.find(FD->getCanonicalDecl());
        if (tf_it != g_transformed_functions.end()) {
            const RustSliceInfo &rs = tf_it->second;
            std::string base_name = FD->getParamDecl(base_idx)->getNameAsString();
            std::string end_name = FD->getParamDecl(end_idx)->getNameAsString();

            SourceLocation BodyStart = FD->getBody()->getBeginLoc();
            SourceLocation BodyEnd = FD->getBody()->getEndLoc();
            unsigned bodyStartOff = SM.getFileOffset(BodyStart);
            unsigned bodyEndOff = SM.getFileOffset(BodyEnd);

            // Get the current rewritten text
            const RewriteBuffer *RB = TheRewriter.getRewriteBufferFor(
                SM.getFileID(BodyStart));
            if (RB) {
                std::string rewritten = std::string(RB->begin(), RB->end());
                // Only look at the body region (approximately)
                // Find remaining param names that need replacement
                auto replaceInRewriter = [&](const std::string &param,
                                              const std::string &replacement) {
                    // Scan the original source for the param name, check if
                    // the rewriter still has it at that position
                    StringRef origBuf = SM.getBufferData(SM.getFileID(BodyStart));
                    size_t pos = bodyStartOff;
                    while (pos < bodyEndOff) {
                        pos = origBuf.find(param, pos);
                        if (pos == StringRef::npos || pos >= bodyEndOff) break;
                        // Word boundary check
                        bool before_ok = (pos == 0 ||
                            (!isalnum(origBuf[pos-1]) && origBuf[pos-1] != '_'));
                        bool after_ok = (pos + param.size() >= origBuf.size() ||
                            (!isalnum(origBuf[pos + param.size()]) &&
                             origBuf[pos + param.size()] != '_'));
                        if (before_ok && after_ok) {
                            // Check if this offset is NOT in m_edited_ranges
                            bool in_edited = false;
                            for (auto &[rs, re] : m_edited_ranges) {
                                if (pos >= rs && pos < re) { in_edited = true; break; }
                            }
                            if (!in_edited) {
                                SourceLocation loc = SM.getLocForStartOfFile(
                                    SM.getFileID(BodyStart)).getLocWithOffset(pos);
                                TheRewriter.ReplaceText(loc, param.size(), replacement);
                            }
                        }
                        pos += param.size();
                    }
                };

                // First handle comparison patterns between params
                // e.g., "lo > hi" → "arr.len == 0", "start >= end" → "arr.len <= 0"
                auto replaceComparisonInRewriter = [&](const std::string &lhs,
                    const std::string &rhs, const std::string &replacement) {
                    StringRef origBuf = SM.getBufferData(SM.getFileID(BodyStart));
                    // Search for patterns like "lhs > rhs", "lhs >= rhs", etc.
                    for (const char *op : {" > ", " >= ", " < ", " <= ", " == ", " != "}) {
                        std::string pat = lhs + op + rhs;
                        size_t pos = bodyStartOff;
                        while ((pos = origBuf.find(pat, pos)) != StringRef::npos && pos < bodyEndOff) {
                            bool in_edited = false;
                            for (auto &[rs_s, rs_e] : m_edited_ranges) {
                                if (pos >= rs_s && pos < rs_e) { in_edited = true; break; }
                            }
                            if (!in_edited) {
                                // Determine replacement comparison
                                std::string cmp_repl;
                                std::string threshold = rs.inclusive_end ? "1" : "0";
                                bool lhs_is_base = (lhs == base_name);
                                std::string op_str(op);
                                // Trim spaces
                                op_str = op_str.substr(1, op_str.size() - 2);
                                if (lhs_is_base) {
                                    // base > end → len < threshold
                                    if (op_str == ">") cmp_repl = rs.slice_param_name + ".len < " + threshold;
                                    else if (op_str == ">=") cmp_repl = rs.slice_param_name + ".len <= " + threshold;
                                    else if (op_str == "<") cmp_repl = rs.slice_param_name + ".len > " + threshold;
                                    else if (op_str == "<=") cmp_repl = rs.slice_param_name + ".len >= " + threshold;
                                    else if (op_str == "==") cmp_repl = rs.slice_param_name + ".len == 0";
                                    else cmp_repl = rs.slice_param_name + ".len != 0";
                                } else {
                                    // end > base → len > threshold
                                    if (op_str == ">") cmp_repl = rs.slice_param_name + ".len > " + threshold;
                                    else if (op_str == ">=") cmp_repl = rs.slice_param_name + ".len >= " + threshold;
                                    else if (op_str == "<") cmp_repl = rs.slice_param_name + ".len < " + threshold;
                                    else if (op_str == "<=") cmp_repl = rs.slice_param_name + ".len <= " + threshold;
                                    else if (op_str == "==") cmp_repl = rs.slice_param_name + ".len == 0";
                                    else cmp_repl = rs.slice_param_name + ".len != 0";
                                }
                                SourceLocation loc = SM.getLocForStartOfFile(
                                    SM.getFileID(BodyStart)).getLocWithOffset(pos);
                                TheRewriter.ReplaceText(loc, pat.size(), cmp_repl);
                                m_edited_ranges.push_back({(unsigned)pos, (unsigned)(pos + pat.size())});
                            }
                            pos += pat.size();
                        }
                    }
                };
                replaceComparisonInRewriter(base_name, end_name, "");
                replaceComparisonInRewriter(end_name, base_name, "");

                // Also handle "end - base" subtraction pattern
                {
                    std::string sub_pat = end_name + " - " + base_name;
                    StringRef origBuf = SM.getBufferData(SM.getFileID(BodyStart));
                    size_t pos = bodyStartOff;
                    while ((pos = origBuf.find(sub_pat, pos)) != StringRef::npos && pos < bodyEndOff) {
                        bool in_edited = false;
                        for (auto &[rs_s, rs_e] : m_edited_ranges) {
                            if (pos >= rs_s && pos < rs_e) { in_edited = true; break; }
                        }
                        if (!in_edited) {
                            SourceLocation loc = SM.getLocForStartOfFile(
                                SM.getFileID(BodyStart)).getLocWithOffset(pos);
                            TheRewriter.ReplaceText(loc, sub_pat.size(),
                                rs.slice_param_name + ".len");
                            m_edited_ranges.push_back({(unsigned)pos, (unsigned)(pos + sub_pat.size())});
                        }
                        pos += sub_pat.size();
                    }
                }

                replaceInRewriter(base_name, rs.slice_param_name + ".ptr");
                replaceInRewriter(end_name, rs.slice_param_name + ".len");
            }
        }
    }

    return true;
}
