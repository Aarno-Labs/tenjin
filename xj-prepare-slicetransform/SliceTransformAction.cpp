#include "SliceTransformAction.h"
#include "SliceRewriter.h"

bool g_slice_inplace = false;
bool g_slice_verbose = false;
std::string g_slice_metadata_in;
xj::PtrIndexMetadata g_slice_metadata;

SliceTransformAction::SliceTransformAction() {}

bool SliceTransformAction::BeginSourceFileAction(CompilerInstance &CI) {
    return true;
}

// Flush edits to disk (--inplace) or stream the rewritten main buffer to
// stdout, mirroring xj-prepare-pointertransform's behavior.
void SliceTransformAction::EndSourceFileAction() {
    SourceManager &SM = TheRewriter.getSourceMgr();
    if (g_slice_inplace) {
        TheRewriter.overwriteChangedFiles();
    } else {
        TheRewriter.getEditBuffer(SM.getMainFileID()).write(llvm::outs());
    }
}

namespace {

// Runs the SliceRewriter over the fully-parsed TU.
class SliceTransformConsumer : public ASTConsumer {
  public:
    explicit SliceTransformConsumer(Rewriter &R) : TheRewriter(R) {}

    void HandleTranslationUnit(ASTContext &Ctx) override {
        xj::SliceRewriter rewriter(TheRewriter, g_slice_metadata);
        rewriter.run(Ctx);
    }

  private:
    Rewriter &TheRewriter;
};

} // namespace

std::unique_ptr<ASTConsumer> SliceTransformAction::CreateASTConsumer(CompilerInstance &CI,
                                                                     StringRef file) {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<SliceTransformConsumer>(TheRewriter);
}
