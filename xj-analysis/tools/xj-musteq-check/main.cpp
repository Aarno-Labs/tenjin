// xj-musteq-check — annotation-driven test driver for xj-analysis.
//
// Reads a C file, runs MustEqualAnalysis over every function defined in it,
// and verifies the `xj-expect` comments against what the analysis produced:
//
//     use(p);   /* xj-expect: copy_of_param: p -> buf */
//     use(p);   /* xj-expect: opaque_rhs: p -> none */
//
// The annotation is per (function, pointer), not per site, because the fact
// the library exposes is the reduced one — an annotation naming a line would
// be testing `sitesFor`, which is a different question. `-> none` asserts
// that no candidate agreed at every use site.
//
// This is the primary test surface, and it is why the transfer's havoc
// rules are worth stating one at a time: every rule gets a two-line C case.

#include "MustEqual.h"
#include "ResolveParameter.h"
#include "VarMutation.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <string>
#include <utility>

using namespace clang;

static llvm::cl::OptionCategory
    Category("xj-musteq-check options");

static llvm::cl::opt<bool>
    Dump("dump",
         llvm::cl::desc("Print every resolution instead of checking "
                        "annotations"),
         llvm::cl::init(false), llvm::cl::cat(Category));

namespace
{

  unsigned Checked = 0;
  unsigned Failed = 0;

  using Key = std::pair<std::string, std::string>; // (function, pointer)

  // `xj-expect: <function>: <pointer> -> <cell text|none>`
  std::map<Key, std::string> parseExpectations(llvm::StringRef Buffer)
  {
    std::map<Key, std::string> Out;
    size_t Pos = 0;
    const llvm::StringRef Marker = "xj-expect:";
    while ((Pos = Buffer.find(Marker, Pos)) != llvm::StringRef::npos)
    {
      llvm::StringRef Rest = Buffer.substr(Pos + Marker.size());
      Rest = Rest.take_until([](char C)
                             { return C == '\n'; });
      // Stop at a trailing comment terminator so `*/` never lands in the
      // expected text.
      size_t End = Rest.find("*/");
      if (End != llvm::StringRef::npos)
        Rest = Rest.substr(0, End);

      auto [Fn, After] = Rest.split(':');
      auto [Ptr, Want] = After.split("->");
      if (!After.contains("->"))
      {
        Pos += Marker.size();
        continue;
      }
      Out[{Fn.trim().str(), Ptr.trim().str()}] = Want.trim().str();
      Pos += Marker.size();
    }
    return Out;
  }

  class Consumer : public ASTConsumer
  {
  public:
    void HandleTranslationUnit(ASTContext &Ctx) override
    {
      const SourceManager &SM = Ctx.getSourceManager();
      auto Expect =
          parseExpectations(SM.getBufferData(SM.getMainFileID()));

      xj::analysis::Options Opts;

      for (Decl *D : Ctx.getTranslationUnitDecl()->decls())
      {
        auto *FD = llvm::dyn_cast<FunctionDecl>(D);
        if (FD == nullptr || !FD->doesThisDeclarationHaveABody() ||
            !SM.isWrittenInMainFile(FD->getLocation()))
          continue;

        std::string Fn = FD->getNameAsString();
        auto Analysis = xj::analysis::MustEqualAnalysis::run(Ctx, *FD, Opts);
        if (!Analysis)
        {
          llvm::errs() << "declined " << Fn << ": "
                       << llvm::toString(Analysis.takeError()) << "\n";
          continue;
        }

        auto Resolver =
            xj::analysis::ParameterResolver::run(*FD, *Analysis);

        for (const VarDecl *V : Resolver.tracked())
        {
          std::string Ptr = V->getNameAsString();
          auto R = Resolver.resolve(V);
          std::string Got =
              R ? Analysis->cells().print(R->Cell) : std::string("none");

          if (Dump)
          {
            llvm::errs() << Fn << ": " << Ptr << " -> " << Got;
            if (R)
            {
              llvm::errs() << " [" << R->Sites << " sites";
              if (R->EntryAnchored)
                llvm::errs() << ", entry-anchored";
              llvm::errs() << "]";
            }
            if (xj::analysis::neverReassigned(V, *FD))
              llvm::errs() << " never-reassigned";
            llvm::errs() << "\n";
            continue;
          }

          auto It = Expect.find({Fn, Ptr});
          if (It == Expect.end())
            continue;
          ++Checked;
          bool OK = It->second == Got;
          if (!OK)
            ++Failed;
          llvm::errs() << (OK ? "ok   " : "FAIL ") << Fn << ":" << Ptr
                       << " want=" << It->second << " got=" << Got << "\n";
        }
      }
    }
  };

  class Action : public ASTFrontendAction
  {
  public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &,
                                                   llvm::StringRef) override
    {
      return std::make_unique<Consumer>();
    }
  };

} // namespace

int main(int argc, const char **argv)
{
  auto Parser = tooling::CommonOptionsParser::create(argc, argv, Category);
  if (!Parser)
  {
    llvm::errs() << Parser.takeError();
    return 2;
  }

  tooling::ClangTool Tool(Parser->getCompilations(),
                          Parser->getSourcePathList());
  if (Tool.run(tooling::newFrontendActionFactory<Action>().get()) != 0)
    return 2;

  if (Dump)
    return 0;
  llvm::errs() << "\n"
               << (Checked - Failed) << "/" << Checked
               << " expectations met\n";
  return Failed == 0 ? 0 : 1;
}
