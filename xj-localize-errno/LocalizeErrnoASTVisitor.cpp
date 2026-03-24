#include "LocalizeErrnoASTVisitor.h"

#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"

using namespace clang;
using namespace clang::tooling;

const std::string LOCAL_ERRNO_NAME = "local_errno";

bool DeclUSR(FunctionDecl *Decl, USRString &Buf)
{
  bool shouldDiscard = index::generateUSRForDecl(Decl, Buf);
  if (shouldDiscard)
  {
    llvm::errs() << "Should discard USR result\n";
  }
  return !shouldDiscard;
}

std::string WrapperString(FunctionDecl *Decl)
{
  auto retType = Decl->getReturnType();
  std::string retTypeStr = retType.getAsString();
  std::string funcName = Decl->getNameAsString();
  bool isVoid = retType->isVoidType();

  std::string wrapperStr = retTypeStr + " _xj_wrap_" + funcName + "(int *_xj_error";
  std::string callArgs;
  for (auto *param : Decl->parameters()) {
    std::string paramType = param->getType().getAsString();
    std::string paramName = param->getNameAsString();
    wrapperStr += ", " + paramType + " " + paramName;
    if (!callArgs.empty()) callArgs += ", ";
    callArgs += paramName;
  }
  if (Decl->isVariadic()) {
    wrapperStr += ", ...";
    callArgs += ", args";
  }
  wrapperStr += ") { ";
  if (Decl->isVariadic()) {
    wrapperStr += "va_list args; ";
    wrapperStr += "__builtin_va_start(args, " + Decl->parameters().back()->getNameAsString() + "); ";
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
  return wrapperStr;
}

bool IsErrnoCall(Expr *E)
{
  if (auto Paren = llvm::dyn_cast<ParenExpr, Expr>(E))
  {
    return IsErrnoCall(Paren->getSubExpr());
  }
  else if (auto Op = llvm::dyn_cast<UnaryOperator, Expr>(E)) 
  {
    if (Op->getOpcode() == UnaryOperatorKind::UO_Deref)
    {
      if (CallExpr *Call = llvm::dyn_cast<CallExpr, Expr>(Op->getSubExpr()))
      {
        if (FunctionDecl *Callee = llvm::dyn_cast<FunctionDecl, Decl>(Call->getCalleeDecl()))
        {
          return Callee->getNameAsString() == "__errno_location";
        }
      }
    }
  }
  return false;
}

bool LocalizeErrnoASTVisitor::TraverseFunctionDecl(FunctionDecl *Decl)
{
  if (Decl->hasBody())
  {
    CurrentFunction = Decl;
  }
  bool res = RecursiveASTVisitor::TraverseFunctionDecl(Decl); 
  if (CurrentFunctionNeedsDecl)
  {
    llvm::outs() << "Needs Decl\n";
    if (auto Body = llvm::dyn_cast<CompoundStmt, Stmt>(Decl->getBody())) 
    {
      AtomicChange InsertLocalDecl(Context->getSourceManager(), Decl->getBody()->getBeginLoc(), /* insert_after = */false);
      auto InsertLoc = Body->body_front()->getBeginLoc();
      auto err = InsertLocalDecl.insert(Context->getSourceManager(), InsertLoc, "int " + LOCAL_ERRNO_NAME + ";\n");
      if (err)
      {
        llvm::errs() << "Error inserting local errno declaration for " << Decl->getNameAsString() << "\n";
        llvm::errs() << err << "\n";
        return false;
      }
      Changes.push_back(InsertLocalDecl);
    }
  }
  CurrentFunctionNeedsDecl = false;
  CurrentFunction = nullptr;
  return res;
}

bool LocalizeErrnoASTVisitor::ReplaceErrnoUsage(Expr *E)
{
  AtomicChange ReplaceUsage(Context->getSourceManager(), E->getBeginLoc());
  CharSourceRange Range(E->getSourceRange(), true);
  llvm::StringRef Replacement = LOCAL_ERRNO_NAME;
  auto err = ReplaceUsage.replace(Context->getSourceManager(), Range, Replacement);
  if (err) {
    llvm::errs() << "Error inserting wrappers: " << err << "\n";
  } else {
    Changes.push_back(ReplaceUsage);
  }
  return !err;
}

bool LocalizeErrnoASTVisitor::VisitParenExpr(clang::ParenExpr *Paren)
{
  if (IsErrnoCall(Paren))
  {
    if (!ReplaceErrnoUsage(Paren))
    {
      // TODO signal error
    }
    CurrentFunctionNeedsDecl = true;
    llvm::outs() << "Found needs Decl\n";
    llvm::outs() << "CurrentFunction " << CurrentFunction->getNameAsString() << "\n";
    return true;
  }
  return true;
}

// bool LocalizeErrnoASTVisitor::VisitUnaryOperator(UnaryOperator *Op)
// {
//   if (IsErrnoCall(Op))
//   {
//     if (!ReplaceErrnoUsage(Op))
//     {
//       // TODO signal error
//     }
//     return true;
//   }
//   return true;
// }

bool LocalizeErrnoASTVisitor::VisitCallExpr(CallExpr *Call)
{
  if (!CurrentFunction)
  {
    return false;
  }

  FunctionDecl *Callee = llvm::dyn_cast<FunctionDecl, Decl>(Call->getCalleeDecl());
  if (!Callee)
  {
    Call->getCalleeDecl()->dump();
    return false;
  }

  USRString CalleeUSR;
  if (!DeclUSR(Callee, CalleeUSR))
  {
    // Signal error?
    return false;
  }

  if (External.find(CalleeUSR) == External.end())
  {
    // Not external
    return true;
  }

  if (Callee->getNameAsString() == "__errno_location")
  {
    return true;
  }

  CurrentFunctionNeedsDecl = true;

  AtomicChange CallWrapper(Context->getSourceManager(), Call->getBeginLoc());
  CharSourceRange CalleeRange(Call->getCallee()->getSourceRange(), true);
  auto err = CallWrapper.replace(Context->getSourceManager(), CalleeRange, "_xj_wrap_" + Callee->getNameAsString());
  if (err)
  {
    llvm::errs() << "Error calling wrapper for: " << Callee->getNameAsString() << "\n";
    return false;
  }
  if (Callee->getNumParams() == 0) 
  {
    err = CallWrapper.insert(Context->getSourceManager(), Call->getRParenLoc(), "&" + LOCAL_ERRNO_NAME, false);
  }
  else
  {
    err = CallWrapper.insert(Context->getSourceManager(), Call->getArg(0)->getBeginLoc(), "&" + LOCAL_ERRNO_NAME + ", ", false);
  }
  if (err)
  {
    llvm::errs() << "Error calling wrapper for: " << Callee->getNameAsString() << "\n";
    return false;
  }
  Changes.push_back(CallWrapper);

  if (!GenerateWrapper(CurrentFunction, Callee))
  {
    llvm::errs() << "Error generating wrapper for: " << Callee->getNameAsString() << "\n";
    return false;
  }

  return true;
}

bool LocalizeErrnoASTVisitor::GenerateWrapper(FunctionDecl *CallContext, FunctionDecl *Callee)
{
  USRString CalleeUSR;
  if (!DeclUSR(Callee, CalleeUSR)) {
    // TODO: signal error
    return false;
  }

  auto ThisInsertLoc = CallContext->getBeginLoc();
  auto PrevLoc = InsertLocations.find(CalleeUSR);

  if (PrevLoc == InsertLocations.end() || Context->getSourceManager().isBeforeInTranslationUnit(ThisInsertLoc, PrevLoc->second))
  {
    InsertLocations.insert(std::pair(CalleeUSR, ThisInsertLoc));
  }

  if (Wrappers.find(CalleeUSR) != Wrappers.end())
  {
    return true;
  }
  Wrappers.insert(CalleeUSR);

  auto WrapperText = WrapperString(Callee);
  PendingWrappers.push_back(std::pair(CalleeUSR, WrapperText));

  return true;
}

void
LocalizeErrnoConsumer::HandleTranslationUnit(clang::ASTContext &Context) {
    Visitor.CurrentFunction = nullptr;
    Visitor.CurrentFunctionNeedsDecl = false;
    Visitor.PendingWrappers.clear();
    Visitor.InsertLocations.clear();
    Visitor.Wrappers.clear();
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());

    std::map<SourceLocation, std::string> ChangesByLoc;
    if (!Visitor.PendingWrappers.empty()) {
    for (auto &Pending : Visitor.PendingWrappers) {
      auto USR = Pending.first;
      auto Wrapper = Pending.second;
      // This should definitely exist
      auto Loc = Visitor.InsertLocations[USR];
      auto LocChanges = ChangesByLoc.find(Loc);
      if (LocChanges == ChangesByLoc.end())
      {
        ChangesByLoc.insert(std::pair(Loc, Wrapper));
      }
      else
      {
        LocChanges->second.append("\n" + Wrapper);
      }
    }
    FileID f;
    SourceManager &SM = Context.getSourceManager();
    for (auto &LocChange : ChangesByLoc)
    {
      AtomicChange wrapperChange(SM, LocChange.first);
      // This assumes we're all inlined so all changes are in the same file.
      f = SM.getFileID(LocChange.first);
      auto err = wrapperChange.insert(SM, LocChange.first, LocChange.second, /*InsertAfter=*/false);
      if (err) {
        llvm::errs() << "Error inserting wrappers: " << err << "\n";
      } else {
        Changes.push_back(wrapperChange);
      }
    }

    // Do we need the __errno_location() decl?
    auto *TUDeclContext = Context.getTranslationUnitDecl();
    DeclarationName ErrnoLocName = &Context.Idents.get("__errno_location");
    auto ErrnoLocResults = TUDeclContext->lookup(ErrnoLocName);
    bool needsErrnoDecl = ErrnoLocResults.empty();
    if (needsErrnoDecl) {
      AtomicChange declareErrno(SM, SM.getLocForStartOfFile(f));
      auto err = declareErrno.insert(SM, SM.getLocForStartOfFile(f), "int *__errno_location();\n", false);
      if (err) {
        llvm::errs() << "Error inserting wrappers: " << err << "\n";
      } else {
        Changes.push_back(declareErrno);
      }
    }
  }
}