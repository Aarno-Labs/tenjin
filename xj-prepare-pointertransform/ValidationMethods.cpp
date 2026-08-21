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

// True if this access kind produces a source edit. The kinds that do not
// are exempt from the macro check below — there is nothing for the
// Rewriter to fail to write.
static bool emitsEdit(PointerAccessKind kind) {
    switch (kind) {
    case PointerAccessKind::NullTest:
    case PointerAccessKind::NoEdit:
    case PointerAccessKind::PairwiseRoot:
    case PointerAccessKind::AddressOf:
    case PointerAccessKind::Unknown:
        return false;
    default:
        return true;
    }
}

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
        // The Rewriter cannot edit text inside a macro expansion, so an
        // access there would be left naming the pointer while its
        // siblings moved to the index form.
        if (emitsEdit(access.kind) && access.loc.isMacroID()) {
            error = "Pointer used inside macro expansion";
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
            has_mutation = true;
            break;
        case PointerAccessKind::Init:
        case PointerAccessKind::Assign:
            if (access.offset_text != "0")
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
