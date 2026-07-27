// SliceDetector — RustSlice candidate detection over index-transformed C.
//
// Runs as a read-only sweep over every TU before SliceRewriter touches
// anything. Input is the output of xj-prepare-pointertransform: moving
// pointers have been replaced by integer index variables in plain form
// (`base[idx]` accesses, `idx < len` / `idx < (end - base)` comparisons,
// `return base + idx`), and the metadata side-file identifies each
// synthesized index variable (its name, the base it indexes, and the
// constant offset bounds observed). The pointer pass records nothing
// slice-related: this class is the sole author of the per-function
// PtrIndexSliceRecord and global-return entries in the metadata, which
// SliceRewriter then consumes.
//
// Detection uses the metadata records for pointer *identity* (which int
// locals are indices, over which base, with what lookaround) and the AST
// for the *anchors*, in four sub-phases mirroring the pre-split tool:
//
//   A. Root candidates: a function containing an index variable whose
//      base is a pointer parameter and whose bound comparison resolves
//      against another parameter (length or end pointer).
//   B. Singleton callees: a function called from a detected function
//      whose pointer params are only ever dereferenced (swap-style).
//   C. Pointer-pair propagation (fixpoint): a function forwarding a
//      (base, end) parameter pair to a detected callee, or recursing
//      over such a pair.
//   D. Global-return functions: every return is NULL or &global[i];
//      the return type collapses to int.
//
// Results accumulate across TUs into the shared metadata with
// first-TU-wins semantics and a file-basename guard against same-named
// statics (uniquify_statics runs after this pass).

#pragma once

#include "PtrIndexMetadata.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace xj {

class SliceDetector {
  public:
    explicit SliceDetector(PtrIndexMetadata &Metadata) : Meta(Metadata) {}

    // Detect candidates in one TU and fold the results into Meta.
    void run(clang::ASTContext &Ctx);

  private:
    PtrIndexMetadata &Meta;

    // Per-TU state (one SliceDetector instance per TU).
    std::vector<const clang::FunctionDecl *> tu_defs; // definitions, source order
    std::map<const clang::FunctionDecl *, const clang::FunctionDecl *>
        def_by_canon;
    std::map<const clang::FunctionDecl *, PtrIndexSliceRecord>
        detected; // by canonical decl
    std::vector<const clang::FunctionDecl *> detect_order;
    std::map<const clang::FunctionDecl *, PtrIndexGlobalReturnRecord>
        global_returns;

    // The pointer-pass record for FD's function, or nullptr when there is
    // none or a same-named function from a different file owns it.
    const PtrIndexFunctionRecord *recordFor(const clang::FunctionDecl *FD,
                                            clang::SourceManager &SM) const;

    // Slice info for a callee: detected in this TU, else recorded in the
    // metadata by an earlier TU, else nullptr.
    const PtrIndexSliceRecord *
    sliceInfoFor(const clang::FunctionDecl *Callee) const;

    void collectTU(clang::ASTContext &Ctx);
    void detectRoots(clang::ASTContext &Ctx);
    void detectSingletons(clang::ASTContext &Ctx);
    void detectPointerPairs(clang::ASTContext &Ctx);
    void detectGlobalReturns(clang::ASTContext &Ctx);
    void exportResults(clang::ASTContext &Ctx);

    void markDetected(const clang::FunctionDecl *Canon,
                      PtrIndexSliceRecord rec);
};

} // namespace xj
