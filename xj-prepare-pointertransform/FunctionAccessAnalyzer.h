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
//      The results are only *recorded* here (see exportMetadata); the
//      signature-level reshaping is applied by xj-prepare-slicetransform.
//   2. transformAllFunctions: rewrite each pointer access inside the
//      bodies of locally-eligible pointers, in plain form (base params
//      kept, comparisons against the original len/end params).
//   3. Globals: file-scope pointers are transformed at the very end,
//      then exportMetadata records this TU's facts for the slice pass.

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

    // Apply a vector<Edit> to the Rewriter, sorted to avoid offset drift
    // and skipping any that overlap an already-edited range.
    void applyEdits(std::vector<Edit> &edits, SourceManager &SM);

    // ---- Cross-function transformation phases -------------------------
    void detectAllTransformations(ASTContext &Ctx);
    void transformAllFunctions(ASTContext &Ctx);

    // ---- Metadata export for xj-prepare-slicetransform ----------------
    // Look up (or create) the metadata record for FD; nullptr when a
    // same-named function from another file already owns the record.
    xj::PtrIndexFunctionRecord *metadataRecordFor(const FunctionDecl *FD,
                                                  ASTContext &Ctx);
    // Record this TU's surviving slice / global-return detection results.
    void exportMetadata(ASTContext &Ctx);
};
