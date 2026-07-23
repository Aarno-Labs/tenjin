// Metadata written by xj-prepare-pointertransform (--metadata-out) as a
// JSON side-file, describing the pointer-to-index rewrites it performed.
//
// For every pointer it rewrote as an index, the tool records the facts
// that identify the rewrite in the transformed source: the synthesized
// index variable, the base it indexes into, and the constant offset
// bounds observed. These are cheap to record during analysis but fragile
// to reconstruct from the rewritten text alone.
//
// The schema also reserves per-function slice records and a
// global-return map: the RustSlice signature reshaping is being split
// out of this tool into a separate pass, which will detect candidates
// from the transformed source plus these per-pointer facts and record
// its decisions in those sections. Nothing fills them yet. Any consumer
// must treat every fact here as a hint and re-verify it against the AST
// before acting on it.

#ifndef XJ_PREPARE_SUPPORT_PTR_INDEX_METADATA_H
#define XJ_PREPARE_SUPPORT_PTR_INDEX_METADATA_H

#include <map>
#include <string>
#include <vector>

namespace xj {

constexpr const char *XJ_PTR_INDEX_METADATA_FILENAME =
    "tenjin_ptr_index_metadata.json";
constexpr int XJ_PTR_INDEX_METADATA_VERSION = 1;

struct PtrIndexPointerRecord {
    std::string name;      // pointer variable name
    std::string index_var; // companion index variable name, "" if none
    int param_index = -1;  // position among the function's params, -1 if local
    bool moved = false;    // pointer had movement and was index-transformed
    // Source text of the base array this pointer indexes into (e.g. "buf",
    // "bs->buf"). Empty when the pointer is its own base (a parameter).
    std::string base_text;
    // Range of constant offsets seen relative to the index position
    // (e.g. p[-1] => min_offset = -1). Used for slice lookback/lookahead.
    long min_offset = 0;
    long max_offset = 0;
    // A non-constant offset (e.g. *(p + n)) was applied to this pointer.
    // Legal for the index transform, but disqualifies slice reshaping.
    bool variable_offsets = false;
    // Name of the single variable this pointer's handle was derived from
    // (sole reseat source), "" if none/multiple.
    std::string handle_source;
    // Some `return` statement returns this pointer (at its index offset).
    bool returned_at_offset = false;
    // Non-pointer variables this pointer's position was compared against
    // (candidates for the `len` of a slice).
    std::vector<std::string> compared_against;
    // The pointer (at fixed offset) is dereferenced but never moved;
    // relevant for (lo,hi) pointer-pair inclusive-end detection.
    bool dereferenced = false;
};

// How a function is to be reshaped into a RustSlice signature. Reserved
// for the downstream slice-reshaping pass; not written by
// xj-prepare-pointertransform.
struct PtrIndexSliceRecord {
    bool present = false; // false => no slice reshaping for this function

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

// A function whose every return is NULL or &global_array[i]: its return
// type is rewritten from T* to int and callers index the array directly.
// Reserved for the downstream slice-reshaping pass.
struct PtrIndexGlobalReturnRecord {
    std::string file;              // basename of the definition's file
    std::string global_array_name; // e.g. "node_storage"
    std::string pointee_type;      // e.g. "Node"
};

struct PtrIndexFunctionRecord {
    // Basename of the file containing the function's definition. Guards
    // against name collisions between static functions in different TUs
    // (uniquify_statics runs after this pass).
    std::string file;
    std::vector<PtrIndexPointerRecord> pointers;
    PtrIndexSliceRecord slice;
};

struct PtrIndexMetadata {
    // Keyed by function name.
    std::map<std::string, PtrIndexFunctionRecord> functions;
    std::map<std::string, PtrIndexGlobalReturnRecord> global_return_functions;
    std::vector<PtrIndexPointerRecord> globals;

    // Serialize to `path`, overwriting. Returns false on I/O error.
    bool writeToFile(const std::string &path) const;
    // Parse from `path`. Returns false on I/O or schema error.
    bool readFromFile(const std::string &path);
};

} // namespace xj

#endif // XJ_PREPARE_SUPPORT_PTR_INDEX_METADATA_H
