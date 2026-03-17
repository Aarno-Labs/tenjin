#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/Tooling.h"
#include <set>

#ifndef _FIND_EXTERNAL_H
#define _FIND_EXTERNAL_H

typedef llvm::SmallString<32> USRString;
typedef std::map<clang::FileID, clang::SourceLocation> InsertPointsMap;

class FindExternallyDefinedFunctionASTVisitor
    : public clang::RecursiveASTVisitor<FindExternallyDefinedFunctionASTVisitor>
{
public:
  explicit FindExternallyDefinedFunctionASTVisitor(
      clang::ASTContext *Context,
      std::set<USRString> *Decl,
      std::set<USRString> *Body);
  bool VisitFunctionDecl(clang::FunctionDecl *Decl);

private:
  clang::ASTContext *Context;
  std::set<USRString> *DeclSet;
  std::set<USRString> *BodySet;
};

class FindExternallyDefinedFunctionConsumer : public clang::ASTConsumer
{
public:
  explicit FindExternallyDefinedFunctionConsumer(clang::ASTContext *Context, std::set<USRString> *Decl, std::set<USRString> *Body, InsertPointsMap *InsertPoints);

  virtual void HandleTranslationUnit(clang::ASTContext &Context);

private:
  std::set<USRString> *Decl;
  std::set<USRString> *Body;
  InsertPointsMap *InsertPoints;
  FindExternallyDefinedFunctionASTVisitor Visitor;
};

class FindExternallyDefinedFunctionAction : public clang::ASTFrontendAction
{
public:
  FindExternallyDefinedFunctionAction(std::set<USRString> *Decl,
     std::set<USRString> *Body,
     InsertPointsMap *InsertPoints);
  virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance &Compiler, llvm::StringRef InFile);

private:
  std::set<USRString> *Decl;
  std::set<USRString> *Body;
  InsertPointsMap *InsertPoints;
};

class FindExternalFunctionActionFactory : public clang::tooling::FrontendActionFactory
{
  public:
  FindExternalFunctionActionFactory() {}

  std::set<USRString> GetExternalDecls();
  InsertPointsMap GetInsertPoints() const { return InsertPoints; }

  std::unique_ptr<clang::FrontendAction> create() override
  {
    return std::make_unique<FindExternallyDefinedFunctionAction>(&Decls, &Bodies, &InsertPoints);
  }
  private:
    std::set<USRString> Decls;
    std::set<USRString> Bodies;
    InsertPointsMap InsertPoints;
};

#endif