#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/Transformer/SourceCode.h"
#include "clang/Tooling/Transformer/SourceCodeBuilders.h"
#include "clang/Tooling/Transformer/Stencil.h"
#include "clang/Tooling/Transformer/Transformer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"

#include "FindExternal.h"
#include "StdlibCallCollector.h"

#include <iostream>
#include <fstream>
#include <stdio.h>
#include <filesystem>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace llvm;

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static cl::OptionCategory LocalizeCategory("xj-localize-errno options");
cl::opt<bool> InPlace("i", cl::desc("Run localization in-place"), cl::cat(LocalizeCategory), cl::init(false));

int main(int argc, const char **argv)
{
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  auto ExpectedParser = CommonOptionsParser::create(argc, argv, LocalizeCategory);
  if (!ExpectedParser)
  {
    // Fail gracefully for unsupported options.
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }

  CommonOptionsParser &OptionsParser = ExpectedParser.get();
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());
  MatchFinder Finder;

  // Collect all decls we will consider to be external;
  FindExternalFunctionActionFactory Action;
  Tool.run(&Action);

  auto ExternalUSRs = Action.GetExternalDecls();

  clang::ast_matchers::DeclarationMatcher errnoDecl =
    functionDecl(hasName("__errno_location")).bind("errno_decl");

  clang::ast_matchers::StatementMatcher errnoUse = 
    unaryOperator(hasOperatorName("*"), hasUnaryOperand(callExpr(callee(functionDecl(hasName("__errno_location")))))).bind("errno_use");

  clang::ast_matchers::DeclarationMatcher callExtern =
      functionDecl(
        hasBody(
          compoundStmt(
            hasDescendant(
              findAll(
                callExpr(
                  callee(
                    declRefExpr(
                      hasDeclaration(functionDecl(isExternC(), unless(hasName("__errno_location"))).bind(StdlibCallCollector::USE_DECL))
                    ).bind(StdlibCallCollector::USE_CALL_EXPR_CALLEE)
                  )
                ).bind(StdlibCallCollector::USE_CALL_EXPR)
              )
            )
          ).bind(StdlibCallCollector::CALL_BODY)
        )
      ).bind(StdlibCallCollector::USE_CONTEXT);

  StdlibCallCollector collector(&ExternalUSRs);
  Finder.addMatcher(traverse(TK_IgnoreUnlessSpelledInSource, errnoDecl), &collector);
  Finder.addMatcher(traverse(TK_IgnoreUnlessSpelledInSource, errnoUse), &collector);
  Finder.addMatcher(traverse(TK_IgnoreUnlessSpelledInSource, callExtern), &collector);

  Tool.run(newFrontendActionFactory(&Finder).get());
  AtomicChanges Changes = collector.changes;

  tooling::ApplyChangesSpec Spec;

  std::map<std::string, AtomicChanges> file_changes;
  for (const auto &Change : Changes)
  {
    auto changes = file_changes.find(Change.getFilePath());
    if (changes == file_changes.end())
    {
      file_changes.insert(std::pair<std::string, AtomicChanges>(Change.getFilePath(), {Change}));
    }
    else
    {
      changes->second.push_back(Change);
    }
  }

  for (const auto &InFile : Tool.getSourcePaths())
  {
    auto File = std::filesystem::absolute(InFile).string();
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> BufferErr =
        llvm::MemoryBuffer::getFile(File);
    if (!BufferErr)
    {
      llvm::errs() << BufferErr.getError().message();
      continue;
    }

    std::string result;
    auto changes = file_changes.find(File);
    if (changes == file_changes.end())
    {
      result = (*BufferErr)->getBuffer();
    }
    else
    {
      auto applyResult = tooling::applyAtomicChanges(File, BufferErr.get()->getBuffer(), changes->second, Spec);
      if (!applyResult)
      {
        llvm::errs() << toString(applyResult.takeError());
        continue;
      }
      result = applyResult.get();
    }
    std::ofstream NewFile;
    if (InPlace) {
      NewFile.open(File);
    } else {
      NewFile.open(File + ".errno");
    }
    NewFile << result;
    NewFile.close();
  }

  return 0;
}
