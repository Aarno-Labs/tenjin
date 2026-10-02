// ValidationMethods.cpp — which pointers are rewritten.
//
// The rewrite is total: the pointer variable is its own base and is never
// deleted, so nothing here judges a base. Two questions are asked, and
// neither is about what a pointer points at:
//
//   1. Is this pointer worth rewriting at all?
//   2. Can every access be edited?
//
// The first is candidacy, not correctness. A pointer that never moves is
// already an ordinary indexable name; giving it an index that stays 0
// would be noise.
//
// They are asked separately because the second has to wait for the first.
// Which accesses are edited depends on the pointer's facts, and those
// follow its pairings, which are with candidates.

#include "FunctionAccessAnalyzer.h"

#include "EditPlan.h"

bool FunctionAccessAnalyzer::isCandidate(
    const std::vector<PointerAccess> &accesses,
    const std::set<const Expr *> &roots,
    ASTContext &Ctx,
    std::string &error) {

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
    }

    // Require at least one mutation (++/--/+=/-=) or one assignment that
    // lands at a non-zero offset. A pointer that is only ever dereferenced
    // is not iterating and gains nothing from an index.
    bool has_mutation = false;
    bool has_offset_assignment = false;
    bool has_meaningful_use = false;

    for (const auto &access : accesses) {
        switch (access.kind) {
        case PointerAccessKind::Move:
        case PointerAccessKind::AssignFromAllowedFunc:
            has_mutation = true;
            break;
        case PointerAccessKind::Element:
            // `*p++` is both.
            if (access.step != IndexStep::None)
                has_mutation = true;
            has_meaningful_use = true;
            break;
        case PointerAccessKind::Init:
        case PointerAccessKind::InitNull:
        case PointerAccessKind::Assign:
        case PointerAccessKind::AssignNull:
            // An assignment lands at an offset when the split moved
            // something into the index, either addend terms or a step on
            // the base's own index.
            if (access.isSplit())
                has_offset_assignment = true;
            break;
        case PointerAccessKind::ValueUse:
            // Being copied into another pointer is not a use of what this
            // one reaches.
            if (!roots.count(access.expr))
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

    return true;
}

bool FunctionAccessAnalyzer::isEditable(
    const std::vector<PointerAccess> &accesses,
    const PointerFacts &facts,
    ASTContext &Ctx,
    std::string &error) {

    for (const auto &access : accesses) {
        // A reference that ends up the root of a pairing gets no rewrite, but
        // whether it does is the plan's to decide, later. It is held to the
        // standard of the rewrite it would have by itself.
        const Stmt *node = editedNode(access, facts);
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
        if (!xj::fileRangeOf(node->getSourceRange(), Ctx.getSourceManager(),
                             Ctx.getLangOpts())) {
            error = std::string("No editable range for a ") +
                    pointerAccessKindToString(access.kind) + " at " +
                    access.loc.printToString(Ctx.getSourceManager());
            return false;
        }
    }
    return true;
}
