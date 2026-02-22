#include "UnionTransformAction.h"

static llvm::cl::OptionCategory MyToolCategory("my-tool options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp("\nMore help text...\n");

static cl::opt<bool> InplaceOpt(
    "inplace",
    cl::desc("Overwrite source files in-place instead of writing to stdout"),
    cl::init(false),
    cl::cat(MyToolCategory));

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    g_inplace = InplaceOpt;
    ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
    return Tool.run(newFrontendActionFactory<UnionTransformAction>().get());
}
