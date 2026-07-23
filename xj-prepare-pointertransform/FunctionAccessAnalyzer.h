#pragma once

#include "PointerAccessCollector.h"

// FunctionAccessAnalyzer — the main orchestrator of the tool.
//
// MatchFinder calls run() once per function definition. run() collects
// per-function data (using PointerAccessCollector) and snapshots it into
// g_function_analyses, but it does NOT yet emit edits. That deferral is
// important: detection of RustSlice-transformable functions runs in a
// fixpoint loop, so the set of "transformed" callees can grow as more
// functions are analyzed.
//
// onEndOfTranslationUnit() runs after every function has been seen and
// drives the actual rewriting in a fixed phase order:
//
//   1. detectAllTransformations: figure out which functions become
//      RustSlice (singletons, pointer-pairs, propagation, global-return).
//   2. transformAllFunctions: rewrite each pointer access inside the
//      bodies of locally-eligible pointers.
//   3. applySingletonTransformations / applyPointerPairTransformations:
//      rewrite the signatures + bodies of RustSlice-transformed funcs.
//   4. emitTypedefs: insert RustSlice_* typedefs.
//   5. rewriteCallSites: patch up callers of RustSlice functions.
//   6. rewriteForwardDeclarations: keep header decls in sync.
//   7. fixReturnTypeChanges: propagate `T* -> int` return-type changes
//      through callers (including return NULL → return -1).
//   8. Globals: file-scope pointers are transformed at the very end.

class FunctionAccessAnalyzer : public MatchFinder::MatchCallback {
  public:
    explicit FunctionAccessAnalyzer(Rewriter &R);

    void run(const MatchFinder::MatchResult &Result) override;
    void onEndOfTranslationUnit() override;

  private:
    Rewriter &TheRewriter;
    ASTContext *StoredCtx = nullptr;       // captured once so end-of-TU phases can use it
    bool globals_collected = false;        // file-scope pointers only need scanning once

    // (begin, end) file offsets of edits already applied. Used to drop
    // later edits that overlap an earlier one — protects against double
    // rewrites when multiple phases would touch the same range.
    std::vector<std::pair<unsigned, unsigned>> m_edited_ranges;

    // Scan the TU once for file-scope pointer variables.
    void collectGlobalPointers(ASTContext &Ctx);

    // Run PointerAccessCollector over the body of one function.
    void traverseFunctionBody(Stmt *Body, PointerAccessCollector &V);

    // Emit a [FAILED] log entry plus update gLog/per-file state.
    void logFailedPointer(const VarDecl *VD, ASTContext &Ctx, const std::string &error);

    // Validate + rewrite one local pointer (the simple within-function path).
    void transformPointerVar(const FunctionDecl *FD, const VarDecl *PtrVar,
                             PointerCandidate &candidate,
                             std::vector<PointerAccess> &accesses,
                             ASTContext &Ctx);

    // Debug dump of an access list (only fires when VERBOSE).
    void printAccesses(const VarDecl *VD, const std::vector<PointerAccess> &seq,
                       ASTContext &Ctx);

    // Defined in ValidationMethods.cpp.
    bool validatePointerCandidate(const VarDecl *PtrVar,
                                  PointerCandidate &candidate,
                                  std::vector<PointerAccess> &accesses,
                                  ASTContext &Ctx,
                                  std::string &error);

    // Defined in TransformationMethods.cpp.
    // generateTransformation: rewrite a single local pointer in place.
    bool generateTransformation(const FunctionDecl *FD,
                                const VarDecl *PtrVar,
                                PointerCandidate &candidate,
                                std::vector<PointerAccess> &accesses,
                                ASTContext &Ctx);

    // generateGlobalTransformation: same idea but for a file-scope
    // pointer (visited from every function that uses it).
    bool generateGlobalTransformation(const VarDecl *PtrVar,
                                      PointerCandidate &candidate,
                                      std::vector<PointerAccess> &accesses,
                                      ASTContext &Ctx);

    // generateSingletonTransformation: rewrite a function whose pointer
    // params are non-iterating (e.g. swap).
    bool generateSingletonTransformation(const FunctionDecl *FD,
                                         const RustSliceInfo &slice_info,
                                         FunctionAnalysis &analysis,
                                         ASTContext &Ctx);

    // generatePointerPairTransformation: rewrite a function whose pointer
    // pair (lo, hi) becomes a single RustSlice.
    bool generatePointerPairTransformation(const FunctionDecl *FD,
                                           FunctionAnalysis &analysis,
                                           ASTContext &Ctx);

    // Apply a vector<Edit> to the Rewriter, sorted to avoid offset drift
    // and skipping any that overlap an already-edited range.
    void applyEdits(std::vector<Edit> &edits, SourceManager &SM);

    // ---- Metadata export (--metadata-out) -----------------------------
    // Look up (or create) the metadata record for FD; nullptr when a
    // same-named function from another file already owns the record.
    xj::PtrIndexFunctionRecord *metadataRecordFor(const FunctionDecl *FD,
                                                  ASTContext &Ctx);

    // ---- Cross-function transformation phases -------------------------
    void detectAllTransformations(ASTContext &Ctx);
    void transformAllFunctions(ASTContext &Ctx);
    void applySingletonTransformations(ASTContext &Ctx);
    void applyPointerPairTransformations(ASTContext &Ctx);

    // Rewrite every call site of a RustSlice-transformed function so
    // callers pass the new slice (or a compound literal) instead of the
    // original (ptr, len) / (lo, hi) pair.
    void rewriteCallSites(ASTContext &Ctx);

    // After RustSlice signature rewriting, some uses of the *original*
    // base/end/len params can survive in the body. This patches them up
    // to reference the new slice (arr.ptr / arr.len).
    void replaceRemovedParams(const FunctionDecl *FD, ASTContext &Ctx);

    // Insert one RustSlice_<T> typedef per pointee type at the earliest
    // function in the TU that needs it.
    void emitTypedefs(ASTContext &Ctx);

    // Translate an argument expression at a call site so that any
    // references to transformed pointers / removed params come out
    // correctly (e.g. `p` → `p_index`, `lo` → `arr.ptr`).
    std::string translateArgExpr(const Expr *ArgExpr, const FunctionDecl *CallerFD,
                                  ASTContext &Ctx);

    // Propagate `T* -> int` return-type changes: rewrite return NULL,
    // change pointer-typed locals that hold return values to int, and
    // patch up callers' assumptions about pointer return values.
    void fixReturnTypeChanges(ASTContext &Ctx);

    // Update non-defining declarations (typically in headers) to match
    // the rewritten signature so the program still links.
    void rewriteForwardDeclarations(ASTContext &Ctx);
};
