// CLI entry point for the base reconstruction tool.
//
// Runs between xj-prepare-pointertransform and xj-prepare-slicetransform.
// The pointer pass hands it a side-file naming every pointer it rewrote;
// for each of them this tool asks xj::analysis::MustEqualAnalysis whether
// the pointer equals one nameable cell at every use, and where it does,
// substitutes that cell's spelling and deletes the pointer.
//
// The side-file is both input and output: `--metadata-in` and
// `--metadata-out` are normally the same path. What comes back out carries
// `base_text` for every pointer that was reconstructed, which is the fact
// the slice pass groups and anchors on.

#include "BaseRewriteAction.h"
#include "PtrIndexMetadata.h"

static llvm::cl::OptionCategory MyToolCategory("base-rewrite options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// --inplace: overwrite source files instead of streaming the rewritten
// translation unit to stdout.
static cl::opt<bool> InplaceOpt(
    "inplace",
    cl::desc("Overwrite source files in-place instead of writing to stdout"),
    cl::init(false),
    cl::cat(MyToolCategory));

// --verbose: one line per pointer, saying what was proved or why nothing
// was. `explain()` is the analysis's own account, so this is the first
// place to look when a base comes out wrong.
static cl::opt<bool> VerboseOpt(
    "verbose",
    cl::desc("Log every pointer considered, and why it did or did not resolve"),
    cl::init(false),
    cl::cat(MyToolCategory));

static cl::opt<std::string> MetadataInOpt(
    "metadata-in",
    cl::desc("Path to pointer/index metadata JSON from xj-prepare-pointertransform"),
    cl::init(""),
    cl::cat(MyToolCategory));

static cl::opt<std::string> MetadataOutOpt(
    "metadata-out",
    cl::desc("Path to re-write that metadata to, with proved bases filled in"),
    cl::init(""),
    cl::cat(MyToolCategory));

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();

    g_base_inplace = InplaceOpt;
    g_base_verbose = VerboseOpt;
    g_base_metadata_in = MetadataInOpt;
    g_base_metadata_out = MetadataOutOpt;

    if (!g_base_metadata_in.empty() &&
        !g_base_metadata.readFromFile(g_base_metadata_in)) {
        llvm::errs() << "xj-prepare-baserewrite: failed to read metadata from "
                     << g_base_metadata_in << "\n";
        return 1;
    }

    // Process each source file with its OWN ClangTool (fresh FileManager).
    // A shared FileManager caches each header's size on first read, and
    // this tool rewrites headers in --inplace mode; re-reading one against
    // a stale size aborts the process. See the matching comment in
    // xj-prepare-pointertransform.
    int rc = 0;
    for (const std::string &Source : OptionsParser.getSourcePathList()) {
        ClangTool Tool(OptionsParser.getCompilations(), {Source});
        int r = Tool.run(newFrontendActionFactory<BaseRewriteAction>().get());
        if (r != 0)
            rc = r;
    }

    const xj::ReconstructionStats &S = g_base_stats;
    llvm::errs() << "[SUMMARY] xj-prepare-baserewrite: considered "
                 << S.Considered << " pointers, reconstructed "
                 << (S.Params + S.Locals + S.LValues) << " (param " << S.Params
                 << ", local " << S.Locals << ", lvalue " << S.LValues
                 << "); " << S.Unresolved << " unresolved, " << S.Declined
                 << " declined, " << S.FunctionsDeclined
                 << " functions not analyzable\n";

    // The recorded declaration positions were valid in this tool's *input*.
    // It has just rewritten that input, so they are stale now; drop them
    // rather than leave a later consumer something to trust.
    if (!g_base_metadata_out.empty()) {
        for (auto &[Key, FnRec] : g_base_metadata.functions) {
            (void)Key;
            for (auto &P : FnRec.pointers) {
                P.decl_line = 0;
                P.decl_col = 0;
            }
        }
        if (!g_base_metadata.writeToFile(g_base_metadata_out)) {
            llvm::errs() << "xj-prepare-baserewrite: failed to write metadata to "
                         << g_base_metadata_out << "\n";
            rc = 1;
        }
    }
    return rc;
}
