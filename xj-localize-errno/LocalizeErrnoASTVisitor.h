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

namespace localize {
enum TransformPolicy {
  Unconditional, // Wrap all external calls
  OnDemand,      // Wrap external calls only when errno is referenced by the calling function
};

// This stages the transformation for each function so that we conditionally apply 
// the localization pass (e.g. if a function does not actually use errno even
// though it may call stdlib functions, we can decide whether to generate wrappers)
struct FunctionTransaction
{
  FunctionTransaction() : Decl(nullptr), HasNonDeclCall(false), HasErrnoAccess(false) {}
  // During traversal, tracks which function body we're in
  clang::FunctionDecl *Decl;
  bool HasNonDeclCall; // If there is a function call through a pointer or otherwise that we can not handle
  bool HasErrnoAccess; // If the function accesses errno
  clang::tooling::AtomicChanges Changes;
  std::set<clang::FunctionDecl*> NeededWrappers;

  bool ShouldApply(TransformPolicy Policy);

  void CommitChanges(clang::ASTContext *Context,
                    clang::tooling::AtomicChanges &DestChanges, 
                     std::set<USRString> &DestWrappedFunctions,
                     std::map<USRString, std::string> &DestWrappers,
                     std::map<USRString, clang::SourceLocation> &DestInsertLocations);

  bool Empty();
  void Begin(clang::FunctionDecl *F);
  void End(clang::FunctionDecl *F);

  void GenerateWrapper(clang::ASTContext *Context,
                       clang::FunctionDecl *Callee,
                       std::set<USRString> &Wrappers,
                       std::map<USRString, std::string> &WrapperDefinitions,
                       std::map<USRString, clang::SourceLocation> &InsertLocations);
private:
  void Clear();
};

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
  LocalizeErrnoASTVisitor(clang::ASTContext *Context, TransformPolicy Policy, std::set<USRString> &External, clang::tooling::AtomicChanges &Changes)
      : Context(Context), External(External), Changes(Changes), Policy(Policy)
      {}

  bool TraverseFunctionDecl(clang::FunctionDecl *Decl);

  bool VisitCallExpr(clang::CallExpr *Call);
  bool VisitExpr(clang::Expr *Expr);

private:
  void ReplaceErrnoUsage(clang::Expr *Expr);

  clang::ASTContext *Context; 

  // Dictates when to apply rewrites
  TransformPolicy Policy;

  // USRs of functions that we should consider external to this
  // project (that is, all TUs being considered), and hence need
  // to be wrapped. This can be further refined in the future to
  // indicate which functions are known not to modify errno.
  std::set<USRString> &External;

  // All changes related to the current function. This must be comitted
  // to populate InsertLocations, Wrappers, WrapperDefinitions, and Changes
  FunctionTransaction CurrentFunctionTxn;

  // f |-> L if f is the USR of a function for which we will generate a wrapper,
  // and L is a location that precedes all uses of f
  std::map<USRString, clang::SourceLocation> InsertLocations;
  
  // All f that we've generated wrappers for
  std::set<USRString> Wrappers;

  // Pairs of (f, <wrapper decl/definition of f>)
  std::map<USRString, std::string> WrapperDefinitions;

  // The accumulated changes
  clang::tooling::AtomicChanges &Changes;
};

class LocalizeErrnoConsumer : public clang::ASTConsumer {
public:
  explicit LocalizeErrnoConsumer(clang::ASTContext *Context, TransformPolicy Policy, std::set<USRString> &External, clang::tooling::AtomicChanges &Changes) 
    : Visitor(Context, Policy, External, Changes), Changes(Changes) {}

  virtual void HandleTranslationUnit(clang::ASTContext &Context) override;

private:
  LocalizeErrnoASTVisitor Visitor;
  clang::tooling::AtomicChanges &Changes;
};

class LocalizeErrnoAction : public clang::ASTFrontendAction {
public:
  LocalizeErrnoAction(TransformPolicy Policy, std::set<USRString> &External, clang::tooling::AtomicChanges &Changes) : Policy(Policy), External(External), Changes(Changes) {}

  virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
      return std::make_unique<LocalizeErrnoConsumer>(&Compiler.getASTContext(), Policy, External, Changes);
    }
private:
  TransformPolicy Policy;
  std::set<USRString> &External;
  clang::tooling::AtomicChanges &Changes;
};

class LocalizeErrnoActionFactory : public clang::tooling::FrontendActionFactory {
public:
  LocalizeErrnoActionFactory(TransformPolicy Policy, clang::tooling::AtomicChanges &Changes, std::set<USRString> &External)
  : External(External), Policy(Policy), Changes(Changes) {}

  std::unique_ptr<clang::FrontendAction> create() override
  {
    return std::make_unique<LocalizeErrnoAction>(Policy, External, Changes);
  }

private:
  clang::tooling::AtomicChanges &Changes;
  TransformPolicy Policy;
  std::set<USRString> &External;
};
}
#endif