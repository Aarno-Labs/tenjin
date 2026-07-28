// Metadata handed from xj-prepare-pointertransform to
// xj-prepare-slicetransform via a JSON side-file. The driver
// (cli/translation_preparation.py) chooses the path and passes it to
// the pointer tool's --metadata-out and the slice tool's --metadata-in.
//
// This is strictly one-way communication: the pointer pass records, for
// every pointer it rewrote as an index, the facts that identify the
// rewrite in the transformed source — the synthesized index variable
// and the base it indexes into — and the slice pass reads them as
// hints, re-verifying every fact against the AST before acting on it.
// The slice pass's own results (RustSlice candidates, global-return
// functions, offset bounds) are in-memory state private to that tool
// (see xj-prepare-slicetransform/SliceDetector.h) and never serialized.

#ifndef XJ_PREPARE_SUPPORT_PTR_INDEX_METADATA_H
#define XJ_PREPARE_SUPPORT_PTR_INDEX_METADATA_H

#include <map>
#include <string>
#include <vector>

namespace xj {

struct PtrIndexPointerRecord {
    std::string name;      // pointer variable name
    std::string index_var; // companion index variable name, "" if none
    int param_index = -1;  // position among the function's params, -1 if local
    // Source text of the base array this pointer indexes into (e.g. "buf",
    // "bs->buf"). Empty when the pointer is its own base (a parameter).
    std::string base_text;
};

struct PtrIndexFunctionRecord {
    // Basename of the file containing the function's definition. Guards
    // against name collisions between static functions in different TUs
    // (uniquify_statics runs after this pass).
    std::string file;
    std::vector<PtrIndexPointerRecord> pointers;
};

struct PtrIndexMetadata {
    // Keyed by function name.
    std::map<std::string, PtrIndexFunctionRecord> functions;

    // Serialize to `path`, overwriting. Returns false on I/O error.
    bool writeToFile(const std::string &path) const;
    // Parse from `path`. Returns false on I/O or schema error.
    bool readFromFile(const std::string &path);
};

} // namespace xj

#endif // XJ_PREPARE_SUPPORT_PTR_INDEX_METADATA_H
