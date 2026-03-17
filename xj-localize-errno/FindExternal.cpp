#include "clang/Index/USRGeneration.h"
#include "FindExternal.h"

using namespace clang;

FindExternallyDefinedFunctionASTVisitor::FindExternallyDefinedFunctionASTVisitor(
    ASTContext *Context,
    std::set<USRString> *Decl,
    std::set<USRString> *Body) : Context(Context), DeclSet(Decl), BodySet(Body) {}

bool FindExternallyDefinedFunctionASTVisitor::VisitFunctionDecl(FunctionDecl *Decl)
{
  if (Linkage::External == getFormalLinkage(Decl->getLinkageAndVisibility().getLinkage()))
  {
    USRString Buf;
    bool shouldDiscard = index::generateUSRForDecl(Decl, Buf);
    if (!shouldDiscard)
    {
      DeclSet->insert(Buf);
      if (Decl->hasBody())
      {
        BodySet->insert(Buf);
      }
    }
  }
  return true;
}

FindExternallyDefinedFunctionConsumer::FindExternallyDefinedFunctionConsumer(clang::ASTContext *Context, std::set<USRString> *Decl, std::set<USRString> *Body, InsertPointsMap *InsertPoints)
    : Visitor(Context, Decl, Body), Decl(Decl), Body(Body), InsertPoints(InsertPoints)
{
}

void FindExternallyDefinedFunctionConsumer::HandleTranslationUnit(clang::ASTContext &Context)
{
  SourceLocation InsertWrapperLoc;
  for (auto *D : Context.getTranslationUnitDecl()->decls())
  {
    if (auto FunDecl = dyn_cast<FunctionDecl>(D)) {
      if (FunDecl->hasBody()) {
        InsertWrapperLoc = FunDecl->getBeginLoc();
        break;
      } 
    }
  }
  auto File = Context.getSourceManager().getFileID(InsertWrapperLoc);
  InsertPoints->insert(std::pair { File, InsertWrapperLoc });
  Visitor.TraverseDecl(Context.getTranslationUnitDecl());
}

FindExternallyDefinedFunctionAction::FindExternallyDefinedFunctionAction(std::set<USRString> *Decl, std::set<USRString> *Body, InsertPointsMap *InsertPoints)
    : Decl(Decl), Body(Body), InsertPoints(InsertPoints) {}

std::unique_ptr<clang::ASTConsumer> FindExternallyDefinedFunctionAction::CreateASTConsumer(
    clang::CompilerInstance &Compiler, llvm::StringRef InFile)
{
  return std::make_unique<FindExternallyDefinedFunctionConsumer>(&Compiler.getASTContext(), Decl, Body, InsertPoints);
}

std::set<USRString> FindExternalFunctionActionFactory::GetExternalDecls()
{
  std::set<USRString> out;
  for (auto &d : Decls)
  {
    if (Bodies.find(d) == Bodies.end())
    {
      out.insert(d);
    }
  }
  return out;
}