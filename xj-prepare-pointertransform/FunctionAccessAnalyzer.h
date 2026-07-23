#pragma once

#include "PointerAccessCollector.h"

// FunctionAccessAnalyzer — the main orchestrator of the tool.
//
// MatchFinder calls run() once per function definition. run() collects
// per-function data (using PointerAccessCollector) and snapshots it into
// g_function_analyses, but it does NOT yet emit edits, so that every
// function in the TU has been analyzed before any rewriting starts.
//
// onEndOfTranslationUnit() runs after every function has been seen and
// drives the actual rewriting:
//
//   1. transformAllFunctions: rewrite each pointer access inside the
//      bodies of eligible pointers, in plain form (base params kept,
//      comparisons against the original len/end params). Each rewritten
//      pointer is recorded in the metadata side-file.
//   2. Globals: file-scope pointers are transformed at the very end.
//
// This tool performs NO RustSlice-related work: candidate detection and
// all signature-level reshaping live in xj-prepare-slicetransform, which
// runs on this tool's output (valid, index-rewritten C) plus the
// per-pointer metadata records.

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

    // ---- Cross-function transformation phase --------------------------
    void transformAllFunctions(ASTContext &Ctx);

    // ---- Metadata export for xj-prepare-slicetransform ----------------
    // Look up (or create) the metadata record for FD; nullptr when a
    // same-named function from another file already owns the record.
    xj::PtrIndexFunctionRecord *metadataRecordFor(const FunctionDecl *FD,
                                                  ASTContext &Ctx);
};
