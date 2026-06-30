// ValidationMethods.cpp — gatekeeper for the pointer-to-index rewrite.
//
// Each check below either confirms a property the rewriter relies on
// (single base array, constant offsets, ...) or rules out a pattern
// the rewriter can't safely reproduce (address-of, writes through
// const, type punning, ...). On the first failed check we set `error`
// to a human-readable reason and return false — that string ends up
// in the [FAILED] log line.

#include "FunctionAccessAnalyzer.h"

bool FunctionAccessAnalyzer::validatePointerCandidate(
    const VarDecl *PtrVar,
    PointerCandidate &candidate,
    std::vector<PointerAccess> &accesses,
    ASTContext &Ctx,
    std::string &error) {

    if (accesses.empty()) {
        error = "No accesses found";
        return false;
    }

    // Any single Unknown access disqualifies the pointer — it means the
    // collector hit a syntactic shape we don't have a rewrite for.
    for (const auto &access : accesses) {
        if (access.kind == PointerAccessKind::Unknown) {
            error = "Unknown access pattern";
            return false;
        }
    }

    // &p means the pointer's storage is observable; replacing it with
    // an int would change observed addresses.
    for (const auto &access : accesses) {
        if (access.kind == PointerAccessKind::AddressOf) {
            error = "Pointer address taken (&p)";
            return false;
        }
    }

    // Comparisons we couldn't resolve into an index form — fail.
    // Resolved comparisons (ComparisonNull, ComparisonExpr) are fine.
    for (const auto &access : accesses) {
        if (access.kind == PointerAccessKind::Comparison) {
            error = "Pointer used in unresolvable comparison";
            return false;
        }
    }

    // *(p + var) is rejected because we can't compute slice bounds for
    // a non-constant offset at compile time. Constant offsets like
    // *(p - 1) are fine — they were folded into min/max_relative_offset.
    if (!candidate.constant_offsets) {
        error = "Pointer has non-constant dereference offset";
        return false;
    }

    // Require at least one mutation (++/--/+=/-=) or one indexed
    // assignment. A pointer that's only ever dereferenced once isn't
    // really iterating and doesn't benefit from the rewrite.
    bool has_mutation = false;
    for (const auto &access : accesses) {
        switch (access.kind) {
        case PointerAccessKind::Increment:
        case PointerAccessKind::Decrement:
        case PointerAccessKind::PlusAssign:
        case PointerAccessKind::MinusAssign:
        case PointerAccessKind::DerefPostInc:
        case PointerAccessKind::DerefPreInc:
        case PointerAccessKind::DerefPostDec:
        case PointerAccessKind::DerefPreDec:
            has_mutation = true;
            break;
        default:
            break;
        }
    }

    // Indexed-assignment shapes also count as "iterating": if the
    // pointer is reseated to a different element of the same array
    // (or returned from a library function we wrap), the rewrite is
    // still meaningful even without an explicit ++/--.
    bool has_array_assignment = false;
    for (const auto &access : accesses) {
        switch (access.kind) {
        case PointerAccessKind::AssignAddrOf:
        case PointerAccessKind::AssignArrayOffset:
        case PointerAccessKind::InitArrayOffset:
        case PointerAccessKind::AssignFromAllowedFunc:
            has_array_assignment = true;
            break;
        default:
            break;
        }
    }

    if (!has_mutation && !has_array_assignment) {
        error = "No array-like usage (no mutations or indexed assignments)";
        return false;
    }

    // Beyond mutation, also require at least one *use* of the value
    // the pointer reaches: a deref, subscript, function call, or
    // return. Two pointers that only reference each other (e.g.
    // `lo`/`hi` that are compared but never read) produce wrong output
    // when only one of them gets rewritten.
    bool has_meaningful_use = false;
    for (const auto &access : accesses) {
        switch (access.kind) {
        case PointerAccessKind::Deref:
        case PointerAccessKind::DerefWrite:
        case PointerAccessKind::DerefPostInc:
        case PointerAccessKind::DerefPreInc:
        case PointerAccessKind::DerefPostDec:
        case PointerAccessKind::DerefPreDec:
        case PointerAccessKind::DerefOffset:
        case PointerAccessKind::DerefOffsetWrite:
        case PointerAccessKind::ArrowAccess:
        case PointerAccessKind::ArrowWrite:
        case PointerAccessKind::Subscript:
        case PointerAccessKind::SubscriptWrite:
        case PointerAccessKind::PassedToAllowedFunc:
        case PointerAccessKind::PassedToFunc:
        case PointerAccessKind::AssignFromAllowedFunc:
        case PointerAccessKind::ReturnPtr:
            has_meaningful_use = true;
            break;
        default:
            break;
        }
    }
    if (!has_meaningful_use && !has_mutation) {
        error = "Pointer never dereferenced or used (only init + comparison)";
        return false;
    }

    // Reject type-punning casts. e.g. `float *p = (float *)int_buf;`
    // writes IEEE-754 bits via *p; the rewritten `int_buf[p_index] = v`
    // would silently float-to-int convert. We require the pointer's
    // pointee type to match the base array's element type exactly.
    if (!candidate.base_array_text.empty() && candidate.base_array != nullptr) {
        QualType ptrPointee = PtrVar->getType()->getPointeeType().getUnqualifiedType();
        QualType baseElem;
        const Expr *baseExpr = candidate.base_array;
        QualType baseType = baseExpr->getType();
        if (baseType->isPointerType())
            baseElem = baseType->getPointeeType().getUnqualifiedType();
        else if (baseType->isArrayType())
            baseElem = Ctx.getAsArrayType(baseType)->getElementType().getUnqualifiedType();

        if (!baseElem.isNull() && ptrPointee != baseElem) {
            error = "Pointer pointee type (" + ptrPointee.getAsString() +
                ") differs from base array element type (" +
                baseElem.getAsString() + ")";
            return false;
        }
    }

    // Reject writes through a const base. The original code casts
    // const away on the pointer side; after rewriting, we'd be writing
    // through the const base directly (`const_arr[idx] = v`), which
    // doesn't even compile.
    if (!candidate.base_array_text.empty() && candidate.base_array != nullptr) {
        QualType baseType = candidate.base_array->getType();
        bool base_is_const = false;
        if (baseType->isPointerType())
            base_is_const = baseType->getPointeeType().isConstQualified();
        else if (baseType->isArrayType())
            base_is_const = Ctx.getAsArrayType(baseType)->getElementType().isConstQualified();
        if (base_is_const) {
            bool has_write = false;
            for (const auto &access : accesses) {
                switch (access.kind) {
                case PointerAccessKind::DerefWrite:
                case PointerAccessKind::SubscriptWrite:
                case PointerAccessKind::ArrowWrite:
                case PointerAccessKind::DerefOffsetWrite:
                    has_write = true;
                    break;
                default:
                    break;
                }
            }
            if (has_write) {
                error = "Pointer writes through const-qualified base";
                return false;
            }
        }
    }

    // The base is supposed to be an indexable array or pointer
    // expression. A `(` not at the start of the base text means we
    // recovered a function-call result (e.g. `w_utf8_drop(string)`),
    // which isn't indexable. A leading `(` is just a cast — fine.
    if (!candidate.base_array_text.empty()) {
        size_t paren_pos = candidate.base_array_text.find('(');
        if (paren_pos != std::string::npos && paren_pos > 0) {
            error = "Base array is a function call return value";
            return false;
        }
    }

    // Reject pointers whose base expression is mutated within the
    // function (e.g. `char *dst = s->out; ... s->out += length;`).
    //
    // The original C code captures the base's *value* at the
    // pointer's declaration. The rewriter, by contrast, substitutes
    // the base's *text* at every access site, so it re-evaluates the
    // base each time. If the base is then mutated between the
    // pointer's declaration and its uses, the rewritten accesses go
    // to the mutated location instead of the captured one.
    //
    // Conservative fix: walk the enclosing function body for any
    // assignment or inc/dec, take the LHS source text, and check if
    // that text appears as a sub-expression of the base text. If it
    // does, the base depends on a mutated lvalue — reject. The
    // sub-expression check uses word boundaries so `arr` doesn't
    // accidentally match `arr_local`.
    //
    // Globals (no enclosing function) are handled separately and
    // typically have stable bases like `static_arr`; skip this check
    // for them.
    if (!candidate.base_array_text.empty()) {
        const FunctionDecl *EnclosingFD = nullptr;
        for (const DeclContext *DC = PtrVar->getDeclContext();
             DC; DC = DC->getParent()) {
            if (const auto *FD = dyn_cast<FunctionDecl>(DC)) {
                EnclosingFD = FD;
                break;
            }
        }

        if (EnclosingFD && EnclosingFD->hasBody()) {
            const SourceManager &SM = Ctx.getSourceManager();
            const LangOptions &LO = Ctx.getLangOpts();
            const std::string &base_text = candidate.base_array_text;

            auto baseContains = [&](const std::string &lhs) -> bool {
                if (lhs.empty()) return false;
                size_t pos = 0;
                while ((pos = base_text.find(lhs, pos)) != std::string::npos) {
                    bool before_ok = pos == 0 ||
                        (!isalnum((unsigned char)base_text[pos - 1]) &&
                         base_text[pos - 1] != '_');
                    bool after_ok = pos + lhs.size() >= base_text.size() ||
                        (!isalnum((unsigned char)base_text[pos + lhs.size()]) &&
                         base_text[pos + lhs.size()] != '_');
                    if (before_ok && after_ok) return true;
                    pos += lhs.size();
                }
                return false;
            };

            class BaseMutationFinder : public RecursiveASTVisitor<BaseMutationFinder> {
              public:
                const SourceManager &SM;
                const LangOptions &LO;
                const VarDecl *TrackedPtr;
                std::function<bool(const std::string &)> baseContains;
                std::string mutated_lhs;
                bool mutated = false;
                std::string escaped_text;
                bool addr_escaped = false;

                BaseMutationFinder(const SourceManager &SM,
                                   const LangOptions &LO,
                                   const VarDecl *TrackedPtr,
                                   std::function<bool(const std::string &)> bc)
                    : SM(SM), LO(LO), TrackedPtr(TrackedPtr),
                      baseContains(std::move(bc)) {}

                // Skip assignments/mutations of the pointer being
                // tracked — those are part of the normal rewrite, not
                // base-expression mutations. e.g. for a parameter
                // `int *in` whose base text is "in", `in = arr + 5`
                // becomes `in_index = 5` after rewriting, which is
                // fine.
                bool refsTrackedPtr(const Expr *E) const {
                    if (!E) return false;
                    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
                        return DRE->getDecl() == TrackedPtr;
                    return false;
                }

                void check(const Expr *E) {
                    if (!E || mutated) return;
                    if (refsTrackedPtr(E)) return;
                    std::string txt = getSourceText(E, SM, LO);
                    if (baseContains(txt)) {
                        mutated_lhs = txt;
                        mutated = true;
                    }
                }

                // `&base` (the base expression, or an lvalue it depends
                // on) means the base may be reseated indirectly — e.g.
                // `esc(&buffer)` can change `buffer` with no visible
                // assignment. Since the rewriter re-evaluates the base's
                // text at each access, such a hidden mutation makes the
                // rewritten accesses read the wrong location. Treat it
                // like a direct base mutation and reject.
                void checkAddrOf(const Expr *E) {
                    if (!E || addr_escaped) return;
                    if (refsTrackedPtr(E)) return;  // &p: the per-pointer
                                                    // AddressOf check handles it
                    std::string txt = getSourceText(E, SM, LO);
                    if (baseContains(txt)) {
                        escaped_text = txt;
                        addr_escaped = true;
                    }
                }

                bool VisitBinaryOperator(BinaryOperator *BO) {
                    if (!BO->isAssignmentOp())
                        return true;
                    check(BO->getLHS()->IgnoreParenImpCasts());
                    return !mutated;
                }

                bool VisitUnaryOperator(UnaryOperator *UO) {
                    if (UO->getOpcode() == UO_AddrOf) {
                        checkAddrOf(UO->getSubExpr()->IgnoreParenImpCasts());
                        return !addr_escaped && !mutated;
                    }
                    if (!UO->isIncrementDecrementOp())
                        return true;
                    check(UO->getSubExpr()->IgnoreParenImpCasts());
                    return !mutated;
                }
            };

            BaseMutationFinder finder(SM, LO, PtrVar, baseContains);
            finder.TraverseStmt(EnclosingFD->getBody());
            if (finder.mutated) {
                error = "Base expression '" + base_text +
                        "' depends on '" + finder.mutated_lhs +
                        "', which is mutated within the function";
                return false;
            }
            if (finder.addr_escaped) {
                error = "Base expression '" + base_text +
                        "' has its address taken via '&" + finder.escaped_text +
                        "', so it may be mutated indirectly within the function";
                return false;
            }
        }
    }

    // The Rewriter can't edit text inside macro expansions, so any
    // "real" access (deref, subscript, comparison expr) inside a macro
    // would be left untouched while sibling accesses get rewritten —
    // producing a TU that mixes pointer and index usage of the same
    // name. Init shapes and ComparisonNull are exempt: they're either
    // replaced wholesale at the DeclStmt or render without naming the
    // pointer.
    for (const auto &access : accesses) {
        switch (access.kind) {
        case PointerAccessKind::InitNull:
        case PointerAccessKind::InitArray:
        case PointerAccessKind::InitArrayOffset:
        case PointerAccessKind::ComparisonNull:
            continue;  // wholesale-replaced; macro location is harmless
        default:
            break;
        }
        if (access.loc.isMacroID()) {
            error = "Pointer used inside macro expansion";
            return false;
        }
    }

    // Base-array consistency is largely enforced inline by the
    // collector (any conflicting base produces an Unknown access,
    // which we already rejected above). The loop here is kept as a
    // documentation point for the cases that participate.
    for (const auto &access : accesses) {
        switch (access.kind) {
        case PointerAccessKind::AssignArray:
        case PointerAccessKind::AssignAddrOf:
        case PointerAccessKind::AssignArrayOffset:
            break;  // base was captured/checked at collection time
        case PointerAccessKind::AssignNull:
        case PointerAccessKind::InitNull:
            break;  // NULL is compatible with any base
        default:
            break;
        }
    }

    // If we still don't have a base, the only salvageable case is a
    // parameter that gets indexed directly (`p[i]`) — there the
    // parameter name itself plays the role of the base array.
    if (candidate.base_array_text.empty()) {
        bool has_subscript = false;
        for (const auto &access : accesses) {
            if (access.kind == PointerAccessKind::Subscript ||
                access.kind == PointerAccessKind::SubscriptWrite) {
                has_subscript = true;
                break;
            }
        }

        if (candidate.is_parameter && (has_mutation || has_subscript)) {
            candidate.base_array_text = PtrVar->getNameAsString();
        } else {
            error = "Could not determine base array";
            return false;
        }
    }

    gLog.foundPointer = true;
    return true;
}
