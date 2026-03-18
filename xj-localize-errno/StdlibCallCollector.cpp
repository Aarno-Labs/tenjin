#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/Transformer/SourceCodeBuilders.h"
#include "clang/Tooling/Transformer/Stencil.h"
#include "clang/Tooling/Transformer/Transformer.h"

#include "StdlibCallCollector.h"

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace llvm;

const std::string StdlibCallCollector::USE_DECL = "decl";
const std::string StdlibCallCollector::USE_CALL_EXPR = "call";
const std::string StdlibCallCollector::USE_CALL_EXPR_CALLEE = "callee";
const std::string StdlibCallCollector::CALL_BODY = "body";
const std::string StdlibCallCollector::USE_CONTEXT = "context";

StdlibCallCollector::StdlibCallCollector(std::set<USRString> *ExternalUSRs)
    : ExternalUSRs(ExternalUSRs),
      changeToWrapperCall(transformer::changeTo(
          transformer::node(USE_CALL_EXPR_CALLEE),
          transformer::cat("_xj_wrap_", transformer::name(USE_DECL)))),
      insertErrnoEmpty(transformer::changeTo(transformer::callArgs(USE_CALL_EXPR), transformer::cat("&local_errno, "))),
      insertErrnoNonEmpty(transformer::insertBefore(transformer::callArgs(USE_CALL_EXPR), transformer::cat("&local_errno, "))),
      readLocalErrno(transformer::edit(
          transformer::changeTo(transformer::node("errno_use"),
                                transformer::cat("local_errno")))),
      localDecl(transformer::edit(
          transformer::insertBefore(transformer::statements(CALL_BODY), transformer::cat("int local_errno = 0;"))))
{}

void StdlibCallCollector::CollectAtomicChanges(SourceManager *SM, SourceLocation loc, SmallVector<transformer::Edit, 1> &edits)
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

void StdlibCallCollector::ApplyEdit(transformer::EditGenerator editGen, const MatchFinder::MatchResult &Result, SourceLocation Loc, std::string ErrMsg)
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

void StdlibCallCollector::CollectPendingWrapper(SourceManager *SM, const FunctionDecl *funCtxt, const FunctionDecl *funDecl, clang::FileID FileID, const USRString &Buf)
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

void StdlibCallCollector::ApplyErrnoUseRewrite(const MatchFinder::MatchResult &Result, const UnaryOperator *errnoUse)
{
  ApplyEdit(readLocalErrno, Result, errnoUse->getBeginLoc(), "Error transforming errno to local errno");
}

void StdlibCallCollector::onStartOfTranslationUnit()
{
  Wrappers.clear();
  PendingWrappers.clear();
  InsertedWrappers.clear();
  PendingSM = nullptr;
  HasErrnoDecl = false;
}

void StdlibCallCollector::onEndOfTranslationUnit()
{
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
void StdlibCallCollector::run(const MatchFinder::MatchResult &Result)
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
