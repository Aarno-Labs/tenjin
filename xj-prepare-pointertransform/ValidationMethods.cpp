// ValidationMethods.cpp — what is left of the gatekeeper.
//
// The rewrite is total: the pointer variable is its own base and is never
// deleted, so there is no base to be unstable, punned, const, or reseated
// out from under an access. Nothing here judges a base. Two questions
// remain, and neither is about what a pointer points at:
//
//   1. Can every access be classified and edited?
//   2. Is this pointer worth rewriting at all?
//
// The second is candidacy, not correctness. A pointer that never moves is
// already an ordinary indexable name; giving it an index that stays 0
// would be noise.

#include "FunctionAccessAnalyzer.h"

#include "EditPlan.h"

bool FunctionAccessAnalyzer::validatePointerCandidate(
    const VarDecl *PtrVar,
    PointerCandidate &candidate,
    std::vector<PointerAccess> &accesses,
    ASTContext &Ctx,
    std::string &error) {

    (void)PtrVar;
    (void)candidate;

    if (accesses.empty()) {
        error = "No accesses found";
        return false;
    }

    // Whether a null test has to move is a property of the whole access
    // list, not of the test — see nullLivesInIndex. Asked here, of the same
    // function the planner asks, so the two cannot disagree about which
    // tests need an editable range.
    const bool null_in_index = nullLivesInIndex(accesses);

    for (const auto &access : accesses) {
        // &p means the pointer's storage is observable. A retained pointer
        // holds its base while the index holds the position, so anything
        // reading through &p would see the wrong one of the two.
        if (access.kind == PointerAccessKind::AddressOf) {
            error = "Pointer address taken (&p)";
            return false;
        }
        // No parent to anchor an edit to. Nothing syntactic should reach
        // here; the location is reported because a hit means the
        // classifier has a hole, and the pointer's own declaration line is
        // not where to look for it.
        if (access.kind == PointerAccessKind::Unknown) {
            error = "Unknown access pattern at " +
                    access.loc.printToString(Ctx.getSourceManager());
            return false;
        }
        // A pairwise root emits nothing *while its owner is rewritten*, and
        // whether the owner is rewritten is what this pass is deciding. If
        // it turns out not to be, the root is rebuilt as whatever it would
        // have been on its own — so hold it to that standard now. The
        // demotion happens once validation is over, with no second chance to
        // refuse.
        //
        // Which standard that is depends on the step. A bare root becomes a
        // value read, whose edit is the reference itself. A stepped root
        // becomes its own increment, whose edit covers the whole `p++` — a
        // wider span, and one that can fail to be addressable where the bare
        // reference would not.
        const Stmt *node = nullptr;
        if (access.kind == PointerAccessKind::PairwiseRoot) {
            if (access.root_adjust == RootAdjust::None) {
                node = access.expr;
            } else {
                PointerAccess demoted = access;
                demoted.kind = (access.root_adjust == RootAdjust::PostInc ||
                                access.root_adjust == RootAdjust::PreInc)
                                   ? PointerAccessKind::Increment
                                   : PointerAccessKind::Decrement;
                node = editedNode(demoted, Ctx);
            }
        } else if (access.kind == PointerAccessKind::NullTest) {
            node = null_in_index ? nullTestNode(access) : nullptr;
        } else {
            node = editedNode(access, Ctx);
        }
        if (!node)
            continue;  // this access legitimately rewrites nothing

        // The Rewriter cannot edit text inside a macro expansion, so an
        // access there would be left naming the pointer while its
        // siblings moved to the index form.
        if (access.loc.isMacroID()) {
            error = "Pointer used inside macro expansion";
            return false;
        }

        // An access that needs a rewrite needs a span to put it in, and
        // that span has to be addressable. Asking here — through the same
        // function the planner asks later — is what lets the planner treat
        // a missing extent as a bug rather than as a case to handle: by the
        // time it runs, every access it sees is plannable.
        FileID file;
        unsigned begin = 0, end = 0;
        if (!editRangeOf(node, Ctx, file, begin, end)) {
            error = std::string("No editable range for a ") +
                    pointerAccessKindToString(access.kind) + " at " +
                    access.loc.printToString(Ctx.getSourceManager());
            return false;
        }
    }

    // ---- Candidacy -------------------------------------------------------
    // Require at least one mutation (++/--/+=/-=) or one assignment that
    // lands at a non-zero offset. A pointer that is only ever dereferenced
    // is not iterating and gains nothing from an index.
    bool has_mutation = false;
    bool has_offset_assignment = false;
    bool has_meaningful_use = false;

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
        case PointerAccessKind::AssignFromAllowedFunc:
            has_mutation = true;
            break;
        case PointerAccessKind::Init:
        case PointerAccessKind::InitNull:
        case PointerAccessKind::Assign:
        case PointerAccessKind::AssignNull:
            // Asked of the split itself, not of its spelling: an assignment
            // lands at an offset when the decomposition moved something into
            // the index, either addend terms or a step on the base's own
            // index.
            if (!access.index_terms.empty() ||
                access.root_adjust != RootAdjust::None)
                has_offset_assignment = true;
            break;
        default:
            break;
        }

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
        case PointerAccessKind::ValueUse:
            has_meaningful_use = true;
            break;
        default:
            break;
        }
    }

    if (!has_mutation && !has_offset_assignment) {
        error = "No array-like usage (no mutations or indexed assignments)";
        return false;
    }

    // Beyond mutation, require at least one use of the value the pointer
    // reaches. Two pointers that only reference each other produce wrong
    // output when only one of them gets rewritten.
    if (!has_meaningful_use && !has_mutation) {
        error = "Pointer never dereferenced or used (only init + comparison)";
        return false;
    }

    gLog.foundPointer = true;
    return true;
}
