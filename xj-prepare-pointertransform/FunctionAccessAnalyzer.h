#pragma once

#include "EditPlan.h"
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
//   1. collectCandidates: decide which pointers — locals, parameters and
//      file-scope alike — are rewritten, and where each companion index is
//      declared. Settled first because a pointer's index may name another's.
//   2. EditPlan: plan every access rewrite in the TU at once, so two that
//      nest fold together instead of one silently displacing the other.
//      Knowing the rewritten set, the plan also decides which indices pair.
//   3. Apply, then record each rewritten pointer in the metadata side-file.
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

    // One pointer that survived validation and has a home for its index.
    // `accesses` points into the analysis that owns it — g_function_analyses
    // for a local or parameter, g_global_pointer_map for a file-scope
    // pointer.
    struct PointerPlan {
        const FunctionDecl *FD = nullptr;   // null for a file-scope pointer
        const VarDecl *ptr = nullptr;
        const std::vector<PointerAccess> *accesses = nullptr;
        IndexDeclSite site;
    };

    // Scan the TU once for file-scope pointer variables.
    void collectGlobalPointers(ASTContext &Ctx);

    // Run PointerAccessCollector over the body of one function.
    void traverseFunctionBody(Stmt *Body, PointerAccessCollector &V);

    // Emit a [FAILED] log entry plus update gLog/per-file state.
    void logFailedPointer(const VarDecl *VD, ASTContext &Ctx, const std::string &error);

    // Decide which pointers in the TU are rewritten and where each index
    // is declared, in the order their declarations should be emitted.
    // `facts` is pointerFacts() of the ones that are.
    void collectCandidates(ASTContext &Ctx, std::vector<PointerPlan> &plans,
                           std::set<const VarDecl *> &transformed,
                           std::map<const VarDecl *, PointerFacts> &facts);

    // Log one rewritten pointer and add it to the metadata side-file.
    void recordTransformed(const PointerPlan &P, ASTContext &Ctx);

    // Debug dump of an access list (only fires when VERBOSE).
    void printAccesses(const VarDecl *VD, const std::vector<PointerAccess> &seq,
                       ASTContext &Ctx);

    // Both defined in ValidationMethods.cpp, and both leave the reason in
    // `error` when they return false.
    //
    // True if the pointer is one to rewrite: nothing takes its address, and
    // it moves. `roots` is every reference in the TU that some assignment
    // takes as its root.
    bool isCandidate(const std::vector<PointerAccess> &accesses,
                     const std::set<const Expr *> &roots,
                     ASTContext &Ctx,
                     std::string &error);

    // True if every access that `facts` says is rewritten has text the
    // Rewriter can replace.
    bool isEditable(const std::vector<PointerAccess> &accesses,
                    const PointerFacts &facts,
                    ASTContext &Ctx,
                    std::string &error);

    // Apply a vector<Edit> to the Rewriter, highest offset first so the
    // offsets still to come stay valid. Every edit handed here is applied;
    // EditPlan has already settled which ones there are.
    void applyEdits(std::vector<Edit> &edits, SourceManager &SM);

    // ---- Metadata export for xj-prepare-slicetransform ----------------
    // Look up (or create) the metadata record for FD; nullptr when a
    // same-named function from another file already owns the record.
    xj::PtrIndexFunctionRecord *metadataRecordFor(const FunctionDecl *FD,
                                                  ASTContext &Ctx);
};
