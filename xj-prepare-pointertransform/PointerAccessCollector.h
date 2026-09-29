#pragma once

#include "Common.h"

// PointerAccessCollector — per-function AST visitor that:
//   1. Finds every local/parameter pointer variable (VisitVarDecl) and
//      records it in `tracked_pointers`.
//   2. Visits every reference to those pointers (VisitDeclRefExpr) and
//      classifies the use into a PointerAccessKind, appending to the
//      pointer's entry in `accesses`.
//
// The classification is *syntactic and local*. It never asks what a
// pointer's base is — the pointer variable is its own base — so it has no
// notion of a base being stable, consistent, or reachable. Every question
// of that kind belongs to base resolution, which runs on this tool's
// output.
class PointerAccessCollector : public RecursiveASTVisitor<PointerAccessCollector> {
  public:
    explicit PointerAccessCollector(ASTContext &Ctx);

    bool VisitVarDecl(VarDecl *VD);
    bool VisitDeclRefExpr(DeclRefExpr *DRE);

    // Output: every tracked pointer in the visited function and the
    // ordered list of accesses recorded for it.
    std::set<const VarDecl *> tracked_pointers;
    std::map<const VarDecl *, std::vector<PointerAccess>> accesses;

  private:
    ASTContext &Ctx;
    const SourceManager &SM;

    // Split a pointer-valued right-hand side into a root and an offset and
    // record the result on `pa`. `Owner` is the pointer being assigned.
    //
    // The split is refused, and the right-hand side kept whole, when the
    // root steps over another type than `Owner` does. It is also refused
    // when `owner_is_declared_here` and the offset cannot move to where the
    // index is declared: ahead of the names it reads
    // (escapesForInitScope), or across a sibling declarator that could
    // tell (crossesSibling).
    void splitAssignedValue(const Expr *RHS, PointerAccess &pa,
                            const VarDecl *Owner = nullptr,
                            bool owner_is_declared_here = false);

    // Walk up the AST parent chain from `DRE` to determine what kind of
    // use this is (Deref, Increment, Subscript, ...) and append a
    // PointerAccess record to `access_list`.
    void classifyAccess(DeclRefExpr *DRE, const VarDecl *PtrVar,
                        std::vector<PointerAccess> &access_list);

    // True if `E` is a null pointer constant: 0, NULL, or ((void*)0).
    bool isNullExpr(const Expr *E);

    // Decompose a pointer-valued expression into the base it starts from and
    // the offset, in elements, that it lands at. Returning false means the
    // expression is its own base at offset 0, which is always sound.
    bool decomposePointer(const Expr *E, PointerSplit &out);

    // True if `VD` is one of the pointers this collector tracks.
    bool isTracked(const Decl *D) const;

    // True if `S` names something `Owner`'s own for-init binds. Such an
    // index has to be declared before the whole loop, where those names do
    // not exist yet.
    bool escapesForInitScope(const Stmt *S, const VarDecl *Owner);

    // True if moving `split`'s offset into `Owner`'s index declaration would
    // change what a sibling declarator sees, or what the offset does.
    //
    // An index is an int, so it cannot be declared among its pointer's
    // siblings: it goes after the whole statement, or ahead of the loop for
    // a for-init, and its initializer is evaluated there. That carries it
    // across the initializers on that side — `char *a = p++, c = *p;` would
    // read `c` before the step. Only a side effect can tell, on either
    // part: a step shows to a sibling that reads the stepped pointer, and
    // anything else is taken to show to all of them.
    bool crossesSibling(const PointerSplit &split, const VarDecl *Owner);
};
