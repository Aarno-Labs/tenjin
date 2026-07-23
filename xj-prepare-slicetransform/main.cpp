// CLI entry point for the slice signature reshaping tool.
//
// Runs after xj-prepare-pointertransform, in two sweeps over the sources:
//
//   1. Detection (read-only): SliceDetector finds every RustSlice
//      candidate — (ptr,len)/(lo,hi) parameter pairs, singleton callees,
//      index-returnable functions — from the index-transformed C plus
//      the per-pointer metadata records (--metadata-in). Detection for
//      *all* TUs completes before any rewriting so that call sites in
//      one TU see candidates defined in another.
//   2. Rewriting: SliceRewriter reshapes signatures, bodies, and call
//      sites to use RustSlice_<T> structs, driven by the detected
//      records.
//
// --metadata-out (optional) dumps the enriched metadata — the pointer
// records plus this tool's detection results — after sweep 1; used by
// the fixture tests to assert detection decisions.

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
// tool detects with no pointer-identity hints (and finds nothing, since
// detection is keyed on the recorded index variables).
static cl::opt<std::string> MetadataInOpt(
    "metadata-in",
    cl::desc("Path to pointer/index metadata JSON from xj-prepare-pointertransform"),
    cl::init(""),
    cl::cat(MyToolCategory));

// --metadata-out: dump the enriched metadata (pointer records + this
// tool's detection results) after the detection sweep.
static cl::opt<std::string> MetadataOutOpt(
    "metadata-out",
    cl::desc("Path to write metadata JSON enriched with detection results"),
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
    g_slice_metadata_out = MetadataOutOpt;

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
    // (The detection sweep is read-only, but keeps the same structure.)
    int rc = 0;

    // ---- Sweep 1: detection (all TUs, no edits) -----------------------
    for (const std::string &Source : OptionsParser.getSourcePathList()) {
        ClangTool Tool(OptionsParser.getCompilations(), {Source});
        int r = Tool.run(newFrontendActionFactory<SliceDetectAction>().get());
        if (r != 0)
            rc = r;
    }

    if (!g_slice_metadata_out.empty() &&
        !g_slice_metadata.writeToFile(g_slice_metadata_out)) {
        llvm::errs() << "xj-prepare-slicetransform: failed to write metadata to "
                     << g_slice_metadata_out << "\n";
        return 1;
    }

    // ---- Sweep 2: rewriting -------------------------------------------
    for (const std::string &Source : OptionsParser.getSourcePathList()) {
        ClangTool Tool(OptionsParser.getCompilations(), {Source});
        int r = Tool.run(newFrontendActionFactory<SliceTransformAction>().get());
        if (r != 0)
            rc = r;
    }
    return rc;
}
