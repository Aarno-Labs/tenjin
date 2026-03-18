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
#include <filesystem>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace llvm;

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static cl::OptionCategory LocalizeCategory("xj-localize-errno options");
cl::opt<bool> InPlace("i", cl::desc("Run localization in-place"), cl::cat(LocalizeCategory), cl::init(false));

const std::string USE_DECL = "decl";
const std::string USE_CALL_EXPR = "call";
const std::string USE_CALL_EXPR_CALLEE = "callee";
const std::string CALL_BODY = "body";
const std::string USE_CONTEXT = "context";

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
    transformer::insertBefore(transformer::statements(CALL_BODY), transformer::cat("int local_errno = 0;"))
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

  void ApplyEdit(transformer::EditGenerator editGen, const MatchFinder::MatchResult &Result, SourceLocation Loc, std::string ErrMsg)
  {
    auto Edits = editGen(Result);
    if (!Edits)
    {
      llvm::errs() << ErrMsg << "\n";
      llvm::errs() << Edits.takeError() << "\n";
    }
    else
    {
      CollectAtomicChanges(Result.SourceManager, Loc, Edits.get());
    }
  }

  // auto applyResult = insertErrno(Result);

  std::set<std::pair<SourceLocation, transformer::ASTEdit>> Wrappers;
  std::set<USRString> InsertedWrappers;
  std::vector<std::pair<USRString, std::string>> PendingWrappers;
  SourceManager *PendingSM = nullptr;
  std::map<USRString, SourceLocation> insertLocations;
  bool HasErrnoDecl = false;

  void CollectPendingWrapper(SourceManager *SM, const FunctionDecl *funCtxt, const FunctionDecl *funDecl, clang::FileID FileID, const USRString &Buf)
  {
    // Even if we've generated a wrapper, still need to 
    // push the insertion point back if we found an earlier use.
    auto thisContextInsertLoc = funCtxt->getBeginLoc();

    auto prevLoc = insertLocations.find(Buf);
    if (prevLoc == insertLocations.end() || SM->isBeforeInTranslationUnit(thisContextInsertLoc, prevLoc->second))
    {
      llvm::outs() << "USR: " << Buf << " " << (prevLoc == insertLocations.end() ? "(first)" : prevLoc->second.printToString(*SM)) << " --> " << thisContextInsertLoc.printToString(*SM) << "\n";
      insertLocations.insert(std::pair<USRString, SourceLocation>(Buf, thisContextInsertLoc));
    }

    if (InsertedWrappers.find(Buf) != InsertedWrappers.end())
    {
      return;
    }
    InsertedWrappers.insert(Buf);

    auto retType = funDecl->getReturnType();
    std::string retTypeStr = retType.getAsString();
    std::string funcName = funDecl->getNameAsString();
    bool isVoid = retType->isVoidType();

    std::string wrapperStr = retTypeStr + " _xj_wrap_" + funcName + "(int *_xj_error";
    std::string callArgs;
    for (auto *param : funDecl->parameters()) {
      std::string paramType = param->getType().getAsString();
      std::string paramName = param->getNameAsString();
      wrapperStr += ", " + paramType + " " + paramName;
      if (!callArgs.empty()) callArgs += ", ";
      callArgs += paramName;
    }
    if (funDecl->isVariadic()) {
      wrapperStr += ", ...";
      callArgs += ", args";
    }
    wrapperStr += ") { ";
    if (funDecl->isVariadic()) {
      wrapperStr += "va_list args; ";
      wrapperStr += "__builtin_va_start(args, " + funDecl->parameters().back()->getNameAsString() + "); ";
    }
    if (!isVoid) {
      wrapperStr += retTypeStr + " ret = ";
    }
    // TODO: assuming #define errno (*errno_location())
    wrapperStr += funcName + "(" + callArgs + "); *_xj_error = (*__errno_location()); ";
    if (!isVoid) {
      wrapperStr += "return ret; ";
    }
    wrapperStr += "}\n";

    PendingWrappers.push_back(std::pair(Buf, wrapperStr));
    PendingSM = SM;
  }

  void ApplyErrnoUseRewrite(const MatchFinder::MatchResult &Result, const UnaryOperator *errnoUse)
  {
      ApplyEdit(readLocalErrno, Result, errnoUse->getBeginLoc(), "Error transforming errno to local errno");
  }


public:
  StdlibCallCollector(std::set<USRString> *ExternalUSRs) : ExternalUSRs(ExternalUSRs) {}
  AtomicChanges changes;

  void onStartOfTranslationUnit() override {
    Wrappers.clear();
    PendingWrappers.clear();
    InsertedWrappers.clear();
    PendingSM = nullptr;
    HasErrnoDecl = false;
  }

  void onEndOfTranslationUnit() override {
    std::map<SourceLocation, std::string> ChangesByLoc;
    if (PendingSM && !PendingWrappers.empty()) {
      for (auto &Pending : PendingWrappers) {
        auto USR = Pending.first;
        auto Wrapper = Pending.second;
        // This should definitely exist
        auto Loc = insertLocations[USR];
        auto Changes = ChangesByLoc.find(Loc);
        if (Changes == ChangesByLoc.end())
        {
          ChangesByLoc.insert(std::pair(Loc, Wrapper));
        }
        else
        {
          Changes->second.append("\n" + Wrapper);
        }
      }
      FileID f;
      for (auto &LocChange : ChangesByLoc)
      {
        AtomicChange wrapperChange(*PendingSM, LocChange.first);
        // This assumes we're all inlined so all changes are in the same file.
        f = PendingSM->getFileID(LocChange.first);
        auto err = wrapperChange.insert(*PendingSM, LocChange.first, LocChange.second, /*InsertAfter=*/false);
        if (err) {
          llvm::errs() << "Error inserting wrappers: " << err << "\n";
        } else {
          changes.push_back(wrapperChange);
        }
      }
      if (!HasErrnoDecl)
      {
        AtomicChange declareErrno(*PendingSM, PendingSM->getLocForStartOfFile(f));
        auto err = declareErrno.insert(*PendingSM, PendingSM->getLocForStartOfFile(f), "int *__errno_location();\n", false);
        if (err) {
          llvm::errs() << "Error inserting wrappers: " << err << "\n";
        } else {
          changes.push_back(declareErrno);
        }
      }
    }
  }

  // TODO signal error so that transform is aborted if we're in an unexpected state
  virtual void run(const MatchFinder::MatchResult &Result)
  {

    if (auto errnoUse = Result.Nodes.getNodeAs<UnaryOperator>("errno_use"))
    {
      ApplyErrnoUseRewrite(Result, errnoUse);
      return;
    }
    else if (auto errnoDecl = Result.Nodes.getNodeAs<FunctionDecl>("errno_decl"))
    {
      HasErrnoDecl = true;
    }

    const clang::CompoundStmt *body = Result.Nodes.getNodeAs<CompoundStmt>(CALL_BODY);
    auto funDecl = Result.Nodes.getNodeAs<FunctionDecl>(USE_DECL);
    auto callExpr = Result.Nodes.getNodeAs<CallExpr>(USE_CALL_EXPR);
    auto context = Result.Nodes.getNodeAs<FunctionDecl>(USE_CONTEXT);

    if (!funDecl || !callExpr || !body || !context)
    {
      llvm::errs() << "Unbound f or call\n";
      return;
    }

    auto FileID = Result.SourceManager->getFileID(funDecl->getLocation());

    if (InsertedDeclaration.find(body) == InsertedDeclaration.end())
    {
      InsertedDeclaration.insert(body);
      ApplyEdit(localDecl, Result, body->getBeginLoc(), "Error inserting local decl");
    }


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

    // Generate and insert wrapper function at the file's insert point
    CollectPendingWrapper(Result.SourceManager, context, funDecl, FileID, Buf);

    llvm::SmallVector<transformer::ASTEdit, 1> callEdits;
    callEdits.push_back(changeToWrapperCall);
    callEdits.push_back(funDecl->getNumParams() == 0 ? insertErrnoEmpty : insertErrnoNonEmpty);
    auto insertErrno = transformer::editList({ changeToWrapperCall, funDecl->getNumParams() == 0 ? insertErrnoEmpty : insertErrnoNonEmpty });
    ApplyEdit(insertErrno, Result, callExpr->getBeginLoc(), "Error inserting errno parameter to stdlib call\n");
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
                      hasDeclaration(functionDecl(isExternC(), unless(hasName("__errno_location"))).bind(USE_DECL))
                    ).bind(USE_CALL_EXPR_CALLEE)
                  )
                ).bind(USE_CALL_EXPR)
              )
            )
          ).bind(CALL_BODY)
        )
      ).bind(USE_CONTEXT);

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
