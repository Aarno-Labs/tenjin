#pragma once

#include "Common.h"

// PointerAccessCollector — per-function AST visitor that:
//   1. Finds every local/parameter pointer variable (VisitVarDecl) and
//      records it in `tracked_pointers`.
//   2. Visits every reference to those pointers (VisitDeclRefExpr) and
//      classifies the use into a PointerAccessKind, appending to the
//      pointer's entry in `accesses`.
class PointerAccessCollector : public RecursiveASTVisitor<PointerAccessCollector>
{
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
  // record the result on `pa`. `Owner` is the pointer being assigned, and
  // `RHS` is either the initializer in `Owner`'s declaration or the
  // right-hand side of an assignment to `Owner`.
  //
  // The split is refused, and the right-hand side kept whole, when the
  // root steps over another type than `Owner`:
  //     int *v; void *raw = v + 1;        // v counts ints, raw counts bytes
  // and, for an initializer, when the offset cannot be evaluated at the
  // index declaration: ahead of a name it reads (escapesForInitScope),
  //     for (int i = k, *p = a + i; ...)
  // would become, with `i` not yet declared:
  //     int p_index_xj = i; for (int i = k, *p = a; ...)
  // or across a sibling declarator that could tell (crossesSibling):
  //     char *a = p++, c = *p;            // the step would follow c = *p
  void splitAssignedValue(const Expr *RHS, PointerAccess &pa,
                          const VarDecl *Owner);

  // Walk up the AST parent chain from `DRE` to determine what kind of
  // use this is (Deref, Increment, Subscript, ...) and append a
  // PointerAccess record to `access_list`.
  void classifyAccess(DeclRefExpr *DRE, const VarDecl *PtrVar,
                      std::vector<PointerAccess> &access_list);

  // True if `E` is a null pointer constant: 0, NULL, or ((void*)0).
  bool isNullExpr(const Expr *E);

  // Decompose a pointer-valued expression into the base it starts from and
  // the offset, in elements, that it lands at. Returning false means the
  // expression is its own base at offset 0.
  bool decomposePointer(const Expr *E, PointerSplit &out);

  // True if `D` is one of the pointers this collector tracks.
  bool isTracked(const Decl *D) const;

  // True if `S` names something `Owner`'s own for-init binds. Such an
  // index has to be declared before the whole loop, where those names do
  // not exist yet.
  bool escapesForInitScope(const Stmt *S, const VarDecl *Owner);

  // `split` decomposes a pointer-valued expression into a root, a step on
  // the root, and offset terms:
  //     p++ + n                    // root p, step ++, term n
  // `Owner` is the pointer that expression initializes, declared with
  // sibling declarators:
  //     char *a = p++ + n, c = *p;
  //
  // Returns true if `split` has a side effect that is visible to any of the
  // sibling declarations of Owner (or vice versa).
  bool crossesSibling(const PointerSplit &split, const VarDecl *Owner);
};
