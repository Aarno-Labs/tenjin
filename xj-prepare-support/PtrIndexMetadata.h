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

    // How a pointer was rewritten.
    //
    //   Collapse — the pointer variable was deleted and each access became
    //              `<base>[index]`, with base substituted as source text.
    //   Handle   — the pointer variable was retained, frozen, and indexes
    //              itself, so base_text is its own name.
    enum class PtrIndexMode
    {
        Collapse,
        Handle,
    };

    struct PtrIndexPointerRecord
    {
        std::string name;      // pointer variable name
        std::string index_var; // companion index variable name, "" if none
        int param_index = -1;  // position among the function's params, -1 if local
        // Source text of the base array this pointer indexes into (e.g. "buf",
        // "bs->buf"). For a frozen handle this is the pointer's own name,
        // which is how the rewritten source spells every access.
        std::string base_text;

        // How the pointer was rewritten.
        PtrIndexMode mode = PtrIndexMode::Collapse;
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
