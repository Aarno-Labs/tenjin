// Metadata handed from xj-prepare-pointertransform to
// xj-prepare-slicetransform via a JSON side-file in the codebase
// directory (see XJ_PTR_INDEX_METADATA_FILENAME).
//
// The pointer pass records, for every pointer it rewrote as an index,
// the facts that identify the rewrite in the transformed source: the
// synthesized index variable, the base it indexes into, and the offset
// bounds it observed. Nothing slice-related is recorded by the pointer
// pass — RustSlice candidate detection runs entirely in the slice pass,
// which fills in the per-function slice records and the global-return
// map below itself (and can dump the enriched result via its
// --metadata-out flag). The slice pass treats every incoming fact as a
// hint and re-verifies it against the AST before acting on it.

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

// How a function is to be reshaped into a RustSlice signature. Filled
// in by the slice pass's detector (SliceDetector), consumed by its
// rewriter (SliceRewriter).
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

// Turn a C type string into something legal inside an identifier.
// e.g. "char *" -> "char_ptr", "unsigned char" -> "unsigned_char".
inline std::string sanitizeTypeForIdentifier(const std::string &type) {
    std::string result = type;
    size_t pos;
    while ((pos = result.find(" *")) != std::string::npos)
        result.replace(pos, 2, "_ptr");
    while ((pos = result.find('*')) != std::string::npos)
        result.replace(pos, 1, "_ptr");
    for (char &c : result) {
        if (c == ' ')
            c = '_';
    }
    return result;
}

// Generate the slice typedef name for a pointee type.
// e.g. "int" -> "RustSlice_int", "char *" -> "RustSlice_char_ptr".
inline std::string makeSliceTypeName(const std::string &pointee_type) {
    return "RustSlice_" + sanitizeTypeForIdentifier(pointee_type);
}

} // namespace xj

#endif // XJ_PREPARE_SUPPORT_PTR_INDEX_METADATA_H
