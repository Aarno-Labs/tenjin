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

    // Split a pointer-valued right-hand side into a root and an offset and
    // record the result on `pa`. `Owner` is the pointer being assigned;
    // `owner_is_declared_here` says its index declaration may have to be
    // hoisted out of a for-init, which is the one thing left that can
    // refuse the split — an offset cannot move to a position ahead of the
    // names it reads.
    void splitAssignedValue(const Expr *RHS, PointerAccess &pa,
                            const VarDecl *Owner = nullptr,
                            bool owner_is_declared_here = false);

    // Output: every tracked pointer in the visited function and the
    // ordered list of accesses recorded for it.
    std::map<const VarDecl *, PointerCandidate> tracked_pointers;
    std::map<const VarDecl *, std::vector<PointerAccess>> accesses;

  private:
    ASTContext &Ctx;
    const SourceManager &SM;
    const LangOptions &LO;

    // Walk up the AST parent chain from `DRE` to determine what kind of
    // use this is (Deref, Increment, Subscript, ...) and append a
    // PointerAccess record to `access_list`.
    void classifyAccess(DeclRefExpr *DRE, const VarDecl *PtrVar,
                        std::vector<PointerAccess> &access_list);

    // True if `E` is a null pointer constant: 0, NULL, or ((void*)0).
    bool isNullExpr(const Expr *E);

    // If this reference is the root of another tracked pointer's
    // initializer or assignment RHS, return that pointer. The reference
    // then needs no edit of its own: the owner's (base, index) rewrite
    // carries the pair.
    const VarDecl *pairwiseOwner(const DeclRefExpr *DRE);

    // True if `VD` is one of the pointers this collector tracks.
    bool isTracked(const Decl *D) const;

    // True if `S` names something `Owner`'s own for-init binds. Such an
    // index has to be declared before the whole loop, where those names do
    // not exist yet.
    bool escapesForInitScope(const Stmt *S, const VarDecl *Owner);
};
