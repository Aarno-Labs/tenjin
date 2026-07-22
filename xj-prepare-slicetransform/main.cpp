// CLI entry point for the slice signature reshaping tool.
//
// Runs after xj-prepare-pointertransform. Reads the pointer/index metadata
// side-file (--metadata-in), detects (ptr,len)/(lo,hi) parameter pairs and
// index-returnable functions, and reshapes signatures, bodies, and call
// sites to use RustSlice_<T> structs.

#include "SliceTransformAction.h"
#include "PtrIndexMetadata.h"

static llvm::cl::OptionCategory MyToolCategory("slice-transform options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// --inplace: overwrite source files instead of streaming the rewritten
// translation unit to stdout.
static cl::opt<bool> InplaceOpt(
    "inplace",
    cl::desc("Overwrite source files in-place instead of writing to stdout"),
    cl::init(false),
    cl::cat(MyToolCategory));

static cl::opt<bool> VerboseOpt(
    "verbose",
    cl::desc("Verbose per-candidate logging"),
    cl::init(false),
    cl::cat(MyToolCategory));

// --metadata-in: path to the JSON side-file written by
// xj-prepare-pointertransform (--metadata-out). Optional: without it the
// tool runs detection with no hints (and currently does nothing).
static cl::opt<std::string> MetadataInOpt(
    "metadata-in",
    cl::desc("Path to pointer/index metadata JSON from xj-prepare-pointertransform"),
    cl::init(""),
    cl::cat(MyToolCategory));

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();

    g_slice_inplace = InplaceOpt;
    g_slice_verbose = VerboseOpt;
    g_slice_metadata_in = MetadataInOpt;

    if (!g_slice_metadata_in.empty() &&
        !g_slice_metadata.readFromFile(g_slice_metadata_in)) {
        llvm::errs() << "xj-prepare-slicetransform: failed to read metadata from "
                     << g_slice_metadata_in << "\n";
        return 1;
    }

    // Process each source file with its OWN ClangTool (fresh FileManager).
    // This tool rewrites headers in --inplace mode, and a shared FileManager
    // caches header sizes on first read; rewriting a shared header mid-run
    // then trips MemoryBuffer's null-terminator invariant on the next TU
    // (SIGABRT). See the matching comment in xj-prepare-pointertransform.
    int rc = 0;
    for (const std::string &Source : OptionsParser.getSourcePathList()) {
        ClangTool Tool(OptionsParser.getCompilations(), {Source});
        int r = Tool.run(newFrontendActionFactory<SliceTransformAction>().get());
        if (r != 0)
            rc = r;
    }
    return rc;
}
