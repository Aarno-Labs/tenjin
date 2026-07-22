// SliceRewriter — applies RustSlice signature reshaping to one TU.
//
// Runs on code already processed by xj-prepare-pointertransform: moving
// pointers have been replaced by integer index variables in plain form
// (base params kept, `base[idx]` accesses, comparisons against the
// original len/end params). Driven by the metadata side-file, this class
// performs everything signature-level:
//
//   1. Body touch-ups per reshaped function: references to the removed
//      base/end/len params become arr.ptr / arr.len forms, index
//      initializers gain lookback offsets, `return base + idx` becomes
//      `return idx` when the return type collapses to int, and locals
//      holding return-changed call results are retyped.
//   2. Singleton (swap-style) functions: `*a` -> `arr.ptr[a]` and the
//      parameter reshape.
//   3. Signature rewrites (+ typedef emission and forward declarations).
//   4. Call-site rewrites (slice pass-through, sub-slices, or compound
//      literals wrapping the original (ptr,len)/(lo,hi) args).
//   5. T* -> int return-type propagation, including the global-return
//      machinery (functions returning &global[i]) and their callers.
//
// Every metadata fact is verified against the AST before use; facts that
// no longer match (e.g. a header already rewritten by an earlier TU in
// the same run) are skipped.

#pragma once

#include "PtrIndexMetadata.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace xj {

class SliceRewriter {
  public:
    SliceRewriter(clang::Rewriter &R, const PtrIndexMetadata &Metadata)
        : TheRewriter(R), Meta(Metadata) {}

    void run(clang::ASTContext &Ctx);

  private:
    clang::Rewriter &TheRewriter;
    const PtrIndexMetadata &Meta;

    // One verified, applicable slice reshaping in this TU.
    struct SliceTarget {
        const PtrIndexFunctionRecord *FnRec = nullptr;
        const PtrIndexSliceRecord *S = nullptr;
        const clang::FunctionDecl *Canon = nullptr; // canonical decl in this TU
        const clang::FunctionDecl *Def = nullptr;   // definition, if in this TU
        bool body_applicable = false; // Def present, still in pre-slice shape
    };

    // Populated by run() for the current TU.
    std::map<const clang::FunctionDecl *, SliceTarget> slice_targets; // by canonical
    std::map<std::string, const clang::FunctionDecl *> global_return_fns; // name->canonical
    // Locals retyped from T* to int because they receive a
    // return-type-changed call result.
    std::set<const clang::VarDecl *> index_return_vars;
    // Functions (with bodies) in this TU, for enclosing-function lookup.
    std::vector<const clang::FunctionDecl *> tu_functions;
    // Names with an existing typedef in this TU (idempotency guard).
    std::set<std::string> existing_typedefs;
    // Slice types whose typedef we have emitted into this TU already.
    std::set<std::string> emitted_typedefs;

    void collectTU(clang::ASTContext &Ctx);
    void verifyTargets(clang::ASTContext &Ctx);

    const SliceTarget *targetFor(const clang::FunctionDecl *FD) const;
    const clang::FunctionDecl *enclosingFunction(clang::SourceLocation Loc,
                                                 clang::SourceManager &SM) const;

    // Phase implementations (see class comment).
    void applySingletonBody(const SliceTarget &T, clang::ASTContext &Ctx);
    void applySliceBody(const SliceTarget &T, clang::ASTContext &Ctx);
    void rewriteSignature(const SliceTarget &T, clang::ASTContext &Ctx);
    void emitTypedefs(clang::ASTContext &Ctx);
    void rewriteCallSites(clang::ASTContext &Ctx);
    void rewriteForwardDeclarations(clang::ASTContext &Ctx);
    void fixReturnTypeChanges(clang::ASTContext &Ctx);

    std::string translateArgExpr(const clang::Expr *ArgExpr,
                                 const SliceTarget &CallerT,
                                 clang::ASTContext &Ctx);
};

} // namespace xj
