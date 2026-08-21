// Metadata handed along the pointer_transform pipeline:
//
//     xj-prepare-pointertransform  --metadata-out
//     xj-prepare-baserewrite       --metadata-in and --metadata-out
//     xj-prepare-slicetransform    --metadata-in
//
// The pointer pass records, for every pointer it rewrote as an index, the
// facts that identify the rewrite in the transformed source. It has no
// opinion about what the pointer's base equals: after its (total,
// syntactic) rewrite every pointer is its own base.
//
// The base rewrite tool is the one that answers that, by running the
// must-equality analysis over the pointer pass's output. When it proves a
// base and substitutes it, it fills in `base_text` and re-emits the
// side-file, and the slice pass consumes a base that was *proved* rather
// than one that was guessed from spellings.

#ifndef XJ_PREPARE_SUPPORT_PTR_INDEX_METADATA_H
#define XJ_PREPARE_SUPPORT_PTR_INDEX_METADATA_H

#include <map>
#include <string>
#include <vector>

namespace xj
{

    struct PtrIndexPointerRecord
    {
        std::string name;      // pointer variable name
        std::string index_var; // companion index variable name, "" if none
        int param_index = -1;  // position among the function's params, -1 if local

        // The base xj-prepare-baserewrite proved this pointer equals and
        // substituted for it throughout the function, e.g. "buf" or
        // "t->storage". Empty in two cases that the slice pass treats
        // alike: the base tool has not run yet, and the base tool declined
        // to reconstruct, leaving the pointer as its own base.
        std::string base_text;

        // Spelling position of the pointer's *declaring identifier*.
        //
        // A bare name is ambiguous under shadowing, so the base tool needs
        // a position to match a VarDecl on — but the position has to be
        // valid in the file the base tool parses, which is the pointer
        // pass's *output*. The pointer pass therefore maps the location
        // through its Rewriter at end of TU, once every edit is applied.
        //
        // Valid only in that intermediate. The base tool rewrites the file
        // in turn, so it clears these when it re-emits the side-file rather
        // than leave a stale position for a later consumer to trust. 0
        // means "no position recorded".
        int decl_line = 0;
        int decl_col = 0;
    };

    struct PtrIndexFunctionRecord
    {
        // Path of the file containing the function's definition.
        std::string file;
        std::vector<PtrIndexPointerRecord> pointers;
    };

    struct PtrIndexMetadata
    {
        // Keyed by xj::functionKey (see FunctionKey.h) — *not* by bare
        // function name, which does not separate same-named statics in
        // different files.
        std::map<std::string, PtrIndexFunctionRecord> functions;

        // Serialize to `path`, overwriting. Returns false on I/O error.
        bool writeToFile(const std::string &path) const;
        // Parse from `path`. Returns false on I/O or schema error.
        bool readFromFile(const std::string &path);
    };

} // namespace xj

#endif // XJ_PREPARE_SUPPORT_PTR_INDEX_METADATA_H
