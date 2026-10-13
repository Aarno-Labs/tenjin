#include "GuidanceAction.h"

#include "FlowInstrumenter.h"
#include "Normalizer.h"
#include "Retyper.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;

namespace xj {

namespace {

class GuidanceConsumer : public ASTConsumer {
public:
  explicit GuidanceConsumer(const SweepConfig &C) : C(C) {}

  void HandleTranslationUnit(ASTContext &Ctx) override {
    if (Ctx.getDiagnostics().hasErrorOccurred()) {
      llvm::errs() << "xj-prepare-guidance: skipping "
                   << mainFileName(Ctx.getSourceManager())
                   << ": it does not compile\n";
      return;
    }
    TUState S(Ctx, *C.G, *C.Reg, C.Results);
    // The order matters: a normalizer rewrite replaces its expression, and
    // a replacement must come before the flow markers that wrap it.
    Retyper(S).run();
    Normalizer(S).run();
    FlowInstrumenter(S).run();
    if (C.Results)
      write(S);
  }

private:
  SweepConfig C;

  static std::string mainFileName(const SourceManager &SM) {
    auto File = SM.getFileEntryRefForID(SM.getMainFileID());
    return File ? File->getName().str() : "<main file>";
  }

  void write(TUState &S) {
    S.Local.emit(S.Edits, S.Reg);
    Rewriter RW(S.SM, S.Ctx.getLangOpts());
    std::vector<GuidanceEdits::Lost> Lost = S.Edits.apply(RW);
    // A bug in this tool, not in the input: writing the file anyway would
    // drop an edit.
    for (const GuidanceEdits::Lost &L : Lost)
      S.diagnose("edit-lost", L.Loc, L.Text.empty() ? "<expression>" : L.Text,
                 std::string("the rewrite ") + L.What +
                     "; the file is left unchanged");
    const auto *Buffer = RW.getRewriteBufferFor(S.SM.getMainFileID());
    if (!Lost.empty() || !Buffer)
      return;
    if (!C.Inplace) {
      Buffer->write(llvm::outs());
      return;
    }
    if (RW.overwriteChangedFiles())
      llvm::errs() << "xj-prepare-guidance: cannot write " << mainFileName(S.SM)
                   << "\n";
  }
};

class GuidanceActionFactory : public tooling::FrontendActionFactory {
public:
  explicit GuidanceActionFactory(const SweepConfig &C) : C(C) {}
  std::unique_ptr<FrontendAction> create() override {
    return std::make_unique<GuidanceAction>(C);
  }

private:
  SweepConfig C;
};

} // namespace

std::unique_ptr<ASTConsumer>
GuidanceAction::CreateASTConsumer(CompilerInstance &, llvm::StringRef) {
  return std::make_unique<GuidanceConsumer>(C);
}

std::unique_ptr<tooling::FrontendActionFactory>
newGuidanceActionFactory(const SweepConfig &C) {
  return std::make_unique<GuidanceActionFactory>(C);
}

} // namespace xj
