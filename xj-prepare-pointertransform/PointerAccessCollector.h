#pragma once

#include "Common.h"

// PointerAccessCollector — per-function AST visitor that:
//   1. Finds every local/parameter pointer variable (VisitVarDecl) and
//      records its initial base array (if any) into `tracked_pointers`.
//   2. Visits every reference to those pointers (VisitDeclRefExpr) and
//      classifies the use into a PointerAccessKind, appending to the
//      pointer's entry in `accesses`.
//
// The output is the raw material every later phase consumes: validation
// inspects the access list to decide if a pointer is safe to rewrite,
// and the rewriter walks the same list to emit edits.
class PointerAccessCollector : public RecursiveASTVisitor<PointerAccessCollector> {
  public:
    explicit PointerAccessCollector(ASTContext &Ctx);

    bool VisitVarDecl(VarDecl *VD);
    bool VisitDeclRefExpr(DeclRefExpr *DRE);

    // Inspect an initializer (or assignment RHS) to extract the base
    // array and emit the matching Init*/Assign* access record.
    void analyzePointerInit(const Expr *Init, const VarDecl *PtrVar,
                            PointerCandidate &candidate,
                            std::vector<PointerAccess> &access_list);

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
                        std::vector<PointerAccess> &access_list,
                        PointerCandidate &candidate);

    // True if `E` is a null pointer constant: 0, NULL, or ((void*)0).
    bool isNullExpr(const Expr *E);

    // Match the &arr[i] pattern. On success, fills `base_text` with the
    // array and `index_text` with the index expression.
    bool isAddrOfSubscript(const Expr *E, std::string &base_text,
                           std::string &index_text);
};
