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

// How a pointer will be rewritten, decided by validatePointerCandidate.
//
//   Reject   — nothing about this pointer is rewritten.
//   Collapse — the pointer variable is deleted and every access becomes
//              `<base source text>[p_index]`. The base is a *syntactic*
//              fact re-substituted at each access site, so it has to be
//              textually valid and stable across the whole function.
//
// Two values today, so this is exactly the boolean the validator used to
// return. It is an enum because the verdict is about to gain a third
// answer, and a bool cannot carry one.
enum class TransformMode { Reject, Collapse };

// Pointers that will be transformed, and how.
//
// This is the single authority on both questions: membership means "will
// be transformed", and the mapped value is how. Keeping the two together
// is deliberate — a set of pointers riding alongside a separate map of
// modes can fall out of step, and then a pointer dropped from one but
// left in the other is rewritten after having been rejected.
using TransformModeMap = std::map<const VarDecl *, TransformMode>;

// The order pointers are rewritten in, split by whether their bound
// comparison resolves against a parameter.
//
// Param-bounded pointers go first so that when two pointers' comparison
// rewrites overlap, the param-bounded form wins — applyEdits drops the
// later of two overlapping edits.
//
// The two groups stay separate because they are not treated alike at
// rewrite time: the verdict map is consulted for `rest` and not for
// `param_bounded`. That asymmetry is longstanding behavior, preserved
// here verbatim rather than quietly unified.
struct EditOrder {
    std::vector<const VarDecl *> param_bounded;
    std::vector<const VarDecl *> rest;
};

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
    TransformMode validatePointerCandidate(const VarDecl *PtrVar,
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
    // transformAllFunctions walks every analyzed function; transformFunction
    // runs the rewrite pipeline for one of them. The pipeline steps below
    // are members because they need validatePointerCandidate or
    // transformPointerVar; the remaining steps are file-static helpers in
    // FunctionAccessAnalyzer.cpp.
    void transformAllFunctions(ASTContext &Ctx);
    void transformFunction(const FunctionDecl *FD, FunctionAnalysis &analysis,
                           ASTContext &Ctx);

    // Judge every pointer, into the mode it will be rewritten in. Pointers
    // that cannot be rewritten at all are simply absent from the map, so
    // membership answers "will this be transformed" and the mapped value
    // answers "how" — one container rather than a set and a mode that can
    // fall out of step.
    TransformModeMap decideTransformModes(FunctionAnalysis &analysis,
                                          ASTContext &Ctx);

    // Drop pointers whose pasted offset text the rewrite would invalidate.
    void rejectStaleOffsets(FunctionAnalysis &analysis, TransformModeMap &modes,
                            ASTContext &Ctx);

    // Rewrite the pointers in `order` that survived into `modes`.
    void emitPointerRewrites(const FunctionDecl *FD, FunctionAnalysis &analysis,
                             const TransformModeMap &modes,
                             const EditOrder &order, ASTContext &Ctx);

    // ---- Metadata export for xj-prepare-slicetransform ----------------
    // Look up (or create) the metadata record for FD; nullptr when a
    // same-named function from another file already owns the record.
    xj::PtrIndexFunctionRecord *metadataRecordFor(const FunctionDecl *FD,
                                                  ASTContext &Ctx);
};
