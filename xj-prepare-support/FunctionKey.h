// Program-wide identity for a function, shared by
// xj-prepare-pointertransform and xj-prepare-slicetransform.
//
// Both tools accumulate per-function state across translation units in
// maps that outlive any one TU. A bare function name is not a valid key
// for those maps: uniquify_statics runs *after* both passes, so two
// distinct `static` functions in different files can still share a name
// here, and a file basename does not separate them either (src1/util.c
// and src2/util.c). Keying by the value this header computes keeps such
// functions apart.

#ifndef XJ_PREPARE_SUPPORT_FUNCTION_KEY_H
#define XJ_PREPARE_SUPPORT_FUNCTION_KEY_H

#include "clang/AST/Decl.h"
#include "clang/Basic/SourceManager.h"

#include <string>

namespace xj
{

    // Identity of `FD` across the whole run. External-linkage functions
    // are unique program-wide by name, so the name alone is the key.
    // Internal-linkage ones are unique only within their file, so their
    // key carries the path of the file the definition is spelled in
    // (the declaration's file when no definition is visible; for a
    // `static` these are necessarily the same file).
    std::string functionKey(const clang::FunctionDecl *FD,
                            clang::SourceManager &SM);

    // Path of the file `FD` is spelled in, as reported by the file
    // entry. Stored alongside a record for diagnostics and as a
    // human-readable cross-check on the key.
    std::string functionFilePath(const clang::FunctionDecl *FD,
                                 clang::SourceManager &SM);

} // namespace xj

#endif // XJ_PREPARE_SUPPORT_FUNCTION_KEY_H
