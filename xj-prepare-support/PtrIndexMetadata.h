// Metadata handed from xj-prepare-pointertransform to
// xj-prepare-slicetransform.
//
// The pointer pass records, for
// every pointer it rewrote as an index, the facts that identify the
// rewrite in the transformed source.

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
        // Source text of the base array this pointer indexes into (e.g. "buf",
        // "bs->buf"). Empty when the pointer is its own base (a parameter).
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
