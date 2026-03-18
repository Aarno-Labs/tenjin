#ifndef _STDLIB_CALL_COLLECTOR_H
#define _STDLIB_CALL_COLLECTOR_H

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"
#include "clang/Tooling/Transformer/RewriteRule.h"

#include "FindExternal.h"

#include <set>
#include <map>
#include <vector>

class StdlibCallCollector : public clang::ast_matchers::MatchFinder::MatchCallback
{
private:
  std::set<USRString> *ExternalUSRs;
  std::set<const clang::CompoundStmt*> InsertedDeclaration;
  std::map<std::string, std::vector<clang::FunctionDecl>> RequiredWrappers;

  const clang::transformer::ASTEdit changeToWrapperCall;
  const clang::transformer::ASTEdit insertErrnoEmpty;
  const clang::transformer::ASTEdit insertErrnoNonEmpty;
  const clang::transformer::EditGenerator readLocalErrno;
  const clang::transformer::EditGenerator localDecl;

  void CollectAtomicChanges(clang::SourceManager *SM, clang::SourceLocation loc, llvm::SmallVector<clang::transformer::Edit, 1> &edits);
  void ApplyEdit(clang::transformer::EditGenerator editGen, const clang::ast_matchers::MatchFinder::MatchResult &Result, clang::SourceLocation Loc, std::string ErrMsg);
  void CollectPendingWrapper(clang::SourceManager *SM, const clang::FunctionDecl *funCtxt, const clang::FunctionDecl *funDecl, clang::FileID FileID, const USRString &Buf);
  void ApplyErrnoUseRewrite(const clang::ast_matchers::MatchFinder::MatchResult &Result, const clang::UnaryOperator *errnoUse);

  std::set<std::pair<clang::SourceLocation, clang::transformer::ASTEdit>> Wrappers;
  std::set<USRString> InsertedWrappers;
  std::vector<std::pair<USRString, std::string>> PendingWrappers;
  clang::SourceManager *PendingSM = nullptr;
  std::map<USRString, clang::SourceLocation> insertLocations;
  bool HasErrnoDecl = false;

public:
  StdlibCallCollector(std::set<USRString> *ExternalUSRs);
  clang::tooling::AtomicChanges changes;

  void onStartOfTranslationUnit() override;
  void onEndOfTranslationUnit() override;
  void run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;


  static const std::string USE_DECL;
  static const std::string USE_CALL_EXPR;
  static const std::string USE_CALL_EXPR_CALLEE;
  static const std::string CALL_BODY;
  static const std::string USE_CONTEXT;
};

#endif
