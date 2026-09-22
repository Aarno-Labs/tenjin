#include "GuidanceAction.h"

#include "FlowInstrumenter.h"
#include "Normalizer.h"
#include "Retyper.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
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
    if (S.Edits.empty())
      return;
    std::string Text = S.Edits.renderFile();
    if (!C.Inplace) {
      llvm::outs() << Text;
      return;
    }
    // Written to a temporary and renamed: the source buffer may be a
    // mapping of the file being replaced.
    llvm::Error E = llvm::writeToOutput(
        mainFileName(S.SM), [&](llvm::raw_ostream &OS) {
          OS << Text;
          return llvm::Error::success();
        });
    if (E)
      llvm::errs() << "xj-prepare-guidance: cannot write "
                   << mainFileName(S.SM) << ": "
                   << llvm::toString(std::move(E)) << "\n";
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
