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

    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<PointerTransformAction>().get());
}
