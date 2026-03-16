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

#include <iostream>
#include <fstream>
#include <stdio.h>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace llvm;

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static llvm::cl::OptionCategory LocalizeCategory("xj-localize-errno options");

const std::string USE_DECL = "decl";
const std::string USE_CALL_EXPR = "call";
const std::string USE_CALL_EXPR_CALLEE = "callee";

class StdlibCallCollector : public MatchFinder::MatchCallback
{
private:
  std::set<SmallString<32>> *ExternalUSRs;
  std::set<const clang::CompoundStmt*> InsertedDeclaration;
  std::map<std::string, std::vector<clang::FunctionDecl>> RequiredWrappers;

  const transformer::ASTEdit changeToWrapperCall =
        transformer::changeTo(
          transformer::node(USE_CALL_EXPR_CALLEE),
          transformer::cat("_xj_wrap_", transformer::name(USE_DECL))
        );

  const transformer::ASTEdit insertErrnoEmpty = 
        transformer::changeTo(transformer::callArgs(USE_CALL_EXPR), transformer::cat("&local_errno, "));

  const transformer::ASTEdit insertErrnoNonEmpty = 
        transformer::insertBefore(transformer::callArgs(USE_CALL_EXPR), transformer::cat("&local_errno, "));

  const transformer::EditGenerator readLocalErrno = transformer::edit(
      transformer::changeTo(transformer::node("errno_use"),
                            transformer::cat("local_errno")));

  const transformer::EditGenerator localDecl = transformer::edit(
    transformer::insertBefore(transformer::statements("BODY"), transformer::cat("int local_errno = 0;"))
  );

  void CollectAtomicChanges(SourceManager *SM, SourceLocation loc, llvm::SmallVector<transformer::Edit, 1> &edits)
  {
    AtomicChange change(*SM, loc);

    for (auto &e : edits)
    {
      auto err = change.replace(*SM, e.Range, e.Replacement);
      if (err)
      {
        llvm::errs() << "Error: " << err << "\n";
      }
    }
    changes.push_back(change);
  }
  // auto applyResult = insertErrno(Result);

public:
  StdlibCallCollector(std::set<USRString> *ExternalUSRs) : ExternalUSRs(ExternalUSRs) {}
  AtomicChanges changes;

  virtual void onStartOfTranslationUnit()
  {

  }

  // TODO signal error so that transform is aborted if we're in an unexpected state
  virtual void run(const MatchFinder::MatchResult &Result)
  {
    auto errnoUse = Result.Nodes.getNodeAs<UnaryOperator>("errno_use");

    if (errnoUse)
    {
      auto applyResult = readLocalErrno(Result);
      if (!applyResult)
      {
        llvm::errs() << "Error transforming errno to local errno\n";
        llvm::errs() << applyResult.takeError() << "\n";
      }
      else
      {
        CollectAtomicChanges(Result.SourceManager, errnoUse->getBeginLoc(), applyResult.get());
      }
      return;
    }

    const clang::CompoundStmt *body = Result.Nodes.getNodeAs<CompoundStmt>("BODY");
    auto funDecl = Result.Nodes.getNodeAs<FunctionDecl>(USE_DECL);
    auto callExpr = Result.Nodes.getNodeAs<CallExpr>(USE_CALL_EXPR);

    if (!funDecl || !callExpr || !body)
    {
      llvm::errs() << "Unbound f or call\n";
      return;
    }

    auto FileName = Result.SourceManager->getFilename(funDecl->getLocation());

    // if (DeclsToWrap->find(funDecl) == DeclsToWrap->end())
    // {
    //   auto loc = funDecl->getLocation();
    //   auto f = Result.SourceManager->getFileID(loc);
    //   DeclsToWrap->insert(funDecl);
    // }

    if (InsertedDeclaration.find(body) == InsertedDeclaration.end())
    {
      InsertedDeclaration.insert(body);
      auto result = localDecl(Result);

      if (!result) {
        llvm::errs() << "Error inserting local decl\n";
        llvm::errs() << result.takeError() << "\n";
        return;
      } 

      CollectAtomicChanges(Result.SourceManager, body->getBeginLoc(), result.get());
    }

//    DeclsToWrap->insert(funDecl);

    auto range = transformer::callArgs("call")(Result);
    USRString Buf;
    bool shouldDiscard = index::generateUSRForDecl(funDecl, Buf);
    if (shouldDiscard)
    {
      llvm::errs() << "Should discard USR result\n";
      return;
    }

    if (ExternalUSRs->find(Buf) == ExternalUSRs->end())
    {
      // Not external
      return;
    }

    llvm::SmallVector<transformer::ASTEdit, 1> callEdits;
    callEdits.push_back(changeToWrapperCall);
    callEdits.push_back(funDecl->getNumParams() == 0 ? insertErrnoEmpty : insertErrnoNonEmpty);
    auto insertErrno = transformer::editList(callEdits);

    auto applyResult = insertErrno(Result);
    if (!applyResult)
    {
      llvm::errs() << "Error inserting errno parameter to stdlib call: ";
      llvm::errs() << applyResult.takeError() << "\n";
    }

    CollectAtomicChanges(Result.SourceManager, callExpr->getBeginLoc(), applyResult.get());
  }
};

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
  // std::set<const FunctionDecl*> DeclsToWrap;

  // TODO merge with below..
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
                      hasDeclaration(functionDecl(isExternC(), unless(hasName("__errno_location"))).bind(USE_DECL))
                    ).bind(USE_CALL_EXPR_CALLEE)
                  )
                ).bind(USE_CALL_EXPR)
              )
            )
          ).bind("BODY")
        )
      );

  StdlibCallCollector collector(&ExternalUSRs);
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

  for (const auto &File : Tool.getSourcePaths())
  {
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
    NewFile.open(File + ".errno");
    NewFile << result;
    NewFile.close();
  }

  return 0;
}
