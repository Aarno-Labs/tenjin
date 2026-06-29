// CLI entry point for the alloc-motion preparation pass.
//
// Parses the standard LibTooling command line (compilation database +
// source paths) and runs AllocMotionAction over each source file. The
// transform rewrites "heap-allocate then field-by-field initialize"
// patterns into "stack-initialize then box" so the C maps cleanly onto a
// Rust `Box::new(...)`. See AllocMotion.cpp and ~/tractor/Allocations.md.

#include "AllocMotionAction.h"

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Signals.h"

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

static cl::OptionCategory MyToolCategory("xj-prepare-allocmotion options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// --inplace: overwrite source files instead of streaming the rewritten
// translation unit to stdout.
static cl::opt<bool> InplaceOpt(
    "inplace",
    cl::desc("Overwrite source files in-place instead of writing to stdout"),
    cl::init(false),
    cl::cat(MyToolCategory));

// --verbose: log per-candidate accept/reject reasons to stderr.
static cl::opt<bool> VerboseOpt(
    "verbose",
    cl::desc("Log why each malloc candidate was accepted or rejected"),
    cl::init(false),
    cl::cat(MyToolCategory));

// --report: dry run. Print which allocations would be rewritten (one per
// line, to stdout) and make no changes. Ignores --inplace.
static cl::opt<bool> ReportOpt(
    "report",
    cl::desc("Report rewritable allocations to stdout without transforming"),
    cl::init(false),
    cl::cat(MyToolCategory));

int main(int argc, const char **argv) {
    llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();

    g_allocmotion_inplace = InplaceOpt;
    g_allocmotion_verbose = VerboseOpt;
    g_allocmotion_report = ReportOpt;

    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<AllocMotionAction>().get());
}
