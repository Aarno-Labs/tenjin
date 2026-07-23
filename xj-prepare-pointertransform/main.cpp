// CLI entry point for the pointer-to-index transformation tool.
//
// Parses the standard LibTooling command line (compilation database +
// source paths), sets a couple of global flags, and runs the
// PointerTransformAction over each source file.

#include "PointerTransformAction.h"

static llvm::cl::OptionCategory MyToolCategory("pointer-transform options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// --inplace: overwrite source files instead of streaming the rewritten
// translation unit to stdout. Useful when running over a whole project.
static cl::opt<bool> InplaceOpt(
    "inplace",
    cl::desc("Overwrite source files in-place instead of writing to stdout"),
    cl::init(false),
    cl::cat(MyToolCategory));

// --verbose: include rejected pointers that never iterated (no array-like
// usage) in the per-file [FAILED] log. By default these are filtered out
// to keep output focused on real candidates.
static cl::opt<bool> VerboseOpt(
    "verbose",
    cl::desc("Show all pointer candidates including non-array-like ones"),
    cl::init(false),
    cl::cat(MyToolCategory));

// --metadata-out: path where the pointer/index metadata side-file is
// written (accumulated over every processed TU). Records the facts about
// each rewritten pointer for downstream consumers; see
// xj-prepare-support/PtrIndexMetadata.h.
static cl::opt<std::string> MetadataOutOpt(
    "metadata-out",
    cl::desc("Path to write pointer/index metadata JSON"),
    cl::init(""),
    cl::cat(MyToolCategory));

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();

    // Publish CLI flags to the rest of the tool through Common.cpp globals.
    g_inplace = InplaceOpt;
    g_verbose = VerboseOpt;
    g_metadata_out = MetadataOutOpt;

    // Process each source file with its OWN ClangTool, and therefore a fresh
    // FileManager, rather than one ClangTool over the whole source list.
    //
    // A single ClangTool shares one FileManager across every file in the run,
    // and that FileManager caches each header's stat (size) on first read. In
    // --inplace mode, EndSourceFileAction calls overwriteChangedFiles(), which
    // rewrites shared headers on disk mid-run (e.g. util.h, which holds the
    // GIT_INLINE functions that get index-transformed while processing the
    // first .c file). A later translation unit that re-reads such a header then
    // reads it against the stale cached size, violating MemoryBuffer's
    // null-terminator invariant ("Buffer is not null terminated!") and aborting
    // the whole process with SIGABRT. A per-file FileManager re-stats every
    // header from its current on-disk contents, so no stale size leaks across
    // files. (Per-file analyzer state is already reset in BeginSourceFileAction.)
    int rc = 0;
    for (const std::string &Source : OptionsParser.getSourcePathList()) {
        ClangTool Tool(OptionsParser.getCompilations(), {Source});
        int r = Tool.run(newFrontendActionFactory<PointerTransformAction>().get());
        if (r != 0)
            rc = r;
    }

    // g_metadata accumulated one function/pointer record set per TU as
    // files were processed; flush it once at the end of the run.
    if (!g_metadata_out.empty() && !g_metadata.writeToFile(g_metadata_out)) {
        llvm::errs() << "xj-prepare-pointertransform: failed to write metadata to "
                     << g_metadata_out << "\n";
        rc = 1;
    }
    return rc;
}
