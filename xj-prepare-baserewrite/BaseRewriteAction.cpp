#include "BaseRewriteAction.h"

bool g_base_inplace = false;
bool g_base_verbose = false;
std::string g_base_metadata_in;
std::string g_base_metadata_out;
xj::PtrIndexMetadata g_base_metadata;
std::set<std::string> g_base_handled;
xj::ReconstructionStats g_base_stats;

namespace {

class BaseRewriteConsumer : public ASTConsumer {
  public:
    explicit BaseRewriteConsumer(Rewriter &R) : TheRewriter(R) {}

    void HandleTranslationUnit(ASTContext &Ctx) override {
        xj::Reconstructor Rec(TheRewriter, g_base_metadata, g_base_handled,
                              g_base_stats, g_base_verbose);
        Rec.run(Ctx);
    }

  private:
    Rewriter &TheRewriter;
};

} // namespace

// Flush edits to disk (--inplace) or stream the rewritten main buffer to
// stdout, mirroring the two sibling tools.
void BaseRewriteAction::EndSourceFileAction() {
    SourceManager &SM = TheRewriter.getSourceMgr();
    if (g_base_inplace) {
        TheRewriter.overwriteChangedFiles();
    } else {
        TheRewriter.getEditBuffer(SM.getMainFileID()).write(llvm::outs());
    }
}

std::unique_ptr<ASTConsumer>
BaseRewriteAction::CreateASTConsumer(CompilerInstance &CI, StringRef file) {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<BaseRewriteConsumer>(TheRewriter);
}
