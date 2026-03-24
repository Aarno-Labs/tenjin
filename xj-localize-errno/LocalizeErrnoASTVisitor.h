#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"
#include "clang/Tooling/Tooling.h"

#include "FindExternal.h"

#include <set>
#include <clang/AST/Decl.h>

#ifndef LOCALIZE_ERRNO_AST_VISITOR_
#define LOCALIZE_ERRNO_AST_VISITOR_
class LocalizeErrnoASTVisitor : public clang::RecursiveASTVisitor<LocalizeErrnoASTVisitor>
{
friend class LocalizeErrnoConsumer;
public:
  LocalizeErrnoASTVisitor(clang::ASTContext *Context, std::set<USRString> &External, clang::tooling::AtomicChanges &Changes)
      : CurrentFunction(nullptr), Context(Context), External(External), Changes(Changes)
      {}

  bool TraverseFunctionDecl(clang::FunctionDecl *Decl);

  bool VisitCallExpr(clang::CallExpr *Call);
  // bool VisitUnaryOperator(clang::UnaryOperator *Op);
  bool VisitParenExpr(clang::ParenExpr *Paren);

private:
  bool ReplaceErrnoUsage(clang::Expr *Expr);
  bool GenerateWrapper(clang::FunctionDecl *Context, clang::FunctionDecl *Callee);

  std::map<USRString, clang::SourceLocation> InsertLocations;
  std::set<USRString> Wrappers;
  std::vector<std::pair<USRString, std::string>> PendingWrappers;
  clang::FunctionDecl *CurrentFunction;
  bool CurrentFunctionNeedsDecl;
  clang::ASTContext *Context; 
  std::set<USRString> &External;
  clang::tooling::AtomicChanges &Changes;
};

class LocalizeErrnoConsumer : public clang::ASTConsumer {
public:
  explicit LocalizeErrnoConsumer(clang::ASTContext *Context, std::set<USRString> &External, clang::tooling::AtomicChanges &Changes) 
    : Visitor(Context, External, Changes), Changes(Changes) {}

  virtual void HandleTranslationUnit(clang::ASTContext &Context) override;

private:
  LocalizeErrnoASTVisitor Visitor;
  clang::tooling::AtomicChanges &Changes;
};

class LocalizeErrnoAction : public clang::ASTFrontendAction {
public:
  LocalizeErrnoAction(std::set<USRString> &External, clang::tooling::AtomicChanges &Changes) : External(External), Changes(Changes) {}

  virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
      return std::make_unique<LocalizeErrnoConsumer>(&Compiler.getASTContext(), External, Changes);
    }
private:
  std::set<USRString> &External;
  clang::tooling::AtomicChanges &Changes;
};

class LocalizeErrnoActionFactory : public clang::tooling::FrontendActionFactory {
public:
  LocalizeErrnoActionFactory(std::set<USRString> &External)
  : External(External) {}

  std::unique_ptr<clang::FrontendAction> create() override
  {
    return std::make_unique<LocalizeErrnoAction>(External, Changes);
  }

  clang::tooling::AtomicChanges Changes;
private:
  std::set<USRString> &External;
};

#endif