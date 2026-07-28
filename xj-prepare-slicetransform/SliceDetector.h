// SliceDetector — RustSlice candidate detection over index-transformed C.
//
// Runs as a read-only sweep over every TU before SliceRewriter touches
// anything. Input is the output of xj-prepare-pointertransform: moving
// pointers have been replaced by integer index variables in plain form
// (`base[idx]` accesses, `idx < len` / `idx < (end - base)` comparisons,
// `return base + idx`), and the metadata side-file identifies each
// synthesized index variable (its name and the base it indexes). The
// pointer pass records identity only: this class is the sole author of
// the per-pointer offset bounds, the per-function PtrIndexSliceRecords,
// and the global-return map — all tool-private in-memory state (defined
// below) that SliceRewriter then consumes; the metadata itself is
// read-only input.
//
// Detection uses the metadata records for pointer *identity* (which int
// locals are indices, over which base) and the AST for everything else
// (offset bounds and the anchors), in four sub-phases:
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
// Results accumulate across TUs into the shared in-memory maps with
// first-TU-wins semantics, keyed by xj::functionKey so that same-named
// statics in different files stay apart (uniquify_statics runs after
// this pass, so the names are not unique yet).

#pragma once

#include "FunctionKey.h"
#include "PtrIndexMetadata.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace xj
{

  // Offset bounds derived for one pointer record: the range of constant
  // offsets applied at the index position in the rewritten source (e.g.
  // base[idx - 1] => min_offset = -1), and whether any subscript applied
  // a non-constant offset to the index (base[idx + n]) — such a pointer
  // has no static bounds, so slice detection must not claim any.
  //
  // This is derived, tool-private state — computed by SliceDetector from
  // the AST, consumed by it and SliceRewriter in the same process — so
  // it lives in an in-memory map keyed by the pointer record's address
  // in the shared metadata (stable for the life of the run), not in the
  // metadata schema itself.
  struct PtrOffsetBounds
  {
    long min_offset = 0;
    long max_offset = 0;
    bool variable_offset = false;
  };
  using PtrOffsetBoundsMap = std::map<const PtrIndexPointerRecord *, PtrOffsetBounds>;

  // How a function is to be reshaped into a RustSlice signature. Filled
  // in by SliceDetector, consumed by SliceRewriter; in-memory only,
  // like PtrOffsetBounds.
  struct PtrIndexSliceRecord
  {
    // Path of the definition's file, for diagnostics and as a
    // cross-check when verifying a record against a redeclaration.
    std::string file;

    std::string slice_param_name; // name of the new slice param, e.g. "arr"
    std::string slice_type;       // generated typedef name, e.g. "RustSlice_int"
    std::string pointee_type;     // element type, e.g. "int"

    // Indices into the *original* parameter list.
    int base_param_index = -1; // pointer parameter that becomes arr.ptr
    int end_param_index = -1;  // end pointer ((lo,hi) form); -1 otherwise
    int len_param_index = -1;  // length parameter (ptr+len form); -1 otherwise

    long lookback = 0;  // slice widening below the base (from *(p - k))
    long lookahead = 0; // slice widening past the bound (from *(p + k))

    bool inclusive_end = false;       // [lo, hi] with hi dereferenced
    bool return_type_changed = false; // T* return rewritten to int

    // Pointer params that don't iterate but are dereferenced (e.g.
    // swap's a,b). They become int indices alongside the slice.
    std::vector<int> singleton_param_indices;
  };
  // Detected reshapings, keyed by xj::functionKey.
  using SliceRecordMap = std::map<std::string, PtrIndexSliceRecord>;

  // Functions whose every return is NULL or &global_array[i]: the
  // return type is rewritten from T* to int and callers index the array
  // directly. xj::functionKey -> global array name.
  using GlobalReturnMap = std::map<std::string, std::string>;

  class SliceDetector
  {
  public:
    SliceDetector(const PtrIndexMetadata &Metadata,
                  PtrOffsetBoundsMap &OffsetBounds, SliceRecordMap &SliceRecords,
                  GlobalReturnMap &GlobalReturns)
        : Meta(Metadata), Bounds(OffsetBounds), Slices(SliceRecords),
          GReturns(GlobalReturns) {}

    // Detect candidates in one TU and fold the results into Meta.
    void run(clang::ASTContext &Ctx);

  private:
    const PtrIndexMetadata &Meta;
    PtrOffsetBoundsMap &Bounds;
    SliceRecordMap &Slices;
    GlobalReturnMap &GReturns;

    // Per-TU state (one SliceDetector instance per TU).
    clang::SourceManager *SM = nullptr;               // set by run()
    std::vector<const clang::FunctionDecl *> tu_defs; // definitions, source order
    std::map<const clang::FunctionDecl *, const clang::FunctionDecl *>
        def_by_canon;
    std::map<const clang::FunctionDecl *, PtrIndexSliceRecord>
        detected; // by canonical decl
    std::vector<const clang::FunctionDecl *> detect_order;
    std::map<const clang::FunctionDecl *, std::string>
        global_returns; // by canonical decl -> global array name

    // The pointer-pass record for FD's function, or nullptr when there
    // is none.
    const PtrIndexFunctionRecord *recordFor(const clang::FunctionDecl *FD,
                                            clang::SourceManager &SM) const;

    // Slice info for a callee: detected in this TU, else recorded in
    // Slices by an earlier TU, else nullptr.
    const PtrIndexSliceRecord *
    sliceInfoFor(const clang::FunctionDecl *Callee) const;

    void collectTU(clang::ASTContext &Ctx);
    // Fill Bounds for each pointer record from the rewritten AST
    // (constant offsets applied at the index position, e.g.
    // base[idx - 1]). The pointer pass records identity only; the
    // lookaround bounds are derived here, where they are consumed.
    void computeOffsetBounds(clang::ASTContext &Ctx);
    void detectRoots(clang::ASTContext &Ctx);
    void detectSingletons(clang::ASTContext &Ctx);
    void detectPointerPairs(clang::ASTContext &Ctx);
    void detectGlobalReturns(clang::ASTContext &Ctx);
    void exportResults(clang::ASTContext &Ctx);

    void markDetected(const clang::FunctionDecl *Canon,
                      PtrIndexSliceRecord rec);
  };

} // namespace xj
