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

// This visitor traverses the AST to find and generate changes:
// 1. Reads of errno (*__errno_location()) are replaced with reads of
//    a local errno int
// 2. Calls to an external function `f` that may set errno. 
//    a. We generate `_xj_wrap_f` that has the same signature as `f` except a new
//    first parameter, which is a pointer to an integer
//    b. We replace calls `f(...)` with `_xj_wrap_f(&local_errno, ...)`
// 3. In functions containing 1 or 2, add the declaration int local_errno; 
class LocalizeErrnoASTVisitor : public clang::RecursiveASTVisitor<LocalizeErrnoASTVisitor>
{
friend class LocalizeErrnoConsumer;
public:
  LocalizeErrnoASTVisitor(clang::ASTContext *Context, std::set<USRString> &External, clang::tooling::AtomicChanges &Changes)
      : CurrentFunction(nullptr), Context(Context), External(External), Changes(Changes), ShouldApply(true)
      {}

  bool TraverseFunctionDecl(clang::FunctionDecl *Decl);

  bool VisitCallExpr(clang::CallExpr *Call);
  // bool VisitUnaryOperator(clang::UnaryOperator *Op);
  bool VisitParenExpr(clang::ParenExpr *Paren);

private:
  void ReplaceErrnoUsage(clang::Expr *Expr);
  void GenerateWrapper(clang::FunctionDecl *Context, clang::FunctionDecl *Callee);

  // USRs of functions that we should consider external to this
  // project (that is, all TUs being considered), and hence need
  // to be wrapped. This can be further refined in the future to
  // indicate which functions are known not to modify errno.
  std::set<USRString> &External;
  // f |-> L if f is the USR of a function for which we will generate a wrapper,
  // and L is a location that precedes all uses of f
  std::map<USRString, clang::SourceLocation> InsertLocations;
  // All f that we've generated wrappers for
  std::set<USRString> Wrappers;
  // Pairs of (f, <wrapper decl/definition of f>)
  std::vector<std::pair<USRString, std::string>> PendingWrappers;
  // During traversal, tracks which function body we're in
  clang::FunctionDecl *CurrentFunction;
  // If we need to declare the local error number var in the current
  // function being traversed
  bool CurrentFunctionNeedsDecl;
  clang::ASTContext *Context; 
  // The accumulated changes
  clang::tooling::AtomicChanges &Changes;
  // ShouldApply is false if an error was encountered
  bool ShouldApply;
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