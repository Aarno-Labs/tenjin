#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"

#include "LocalizeErrnoASTVisitor.h"
#include "StdlibSpec.h"

using namespace clang;
using namespace clang::tooling;

const std::string LOCAL_ERRNO_NAME = "_xj_local_errno";

USRString DeclUSR(FunctionDecl *Decl)
{
  USRString Buf;
  bool shouldDiscard = index::generateUSRForDecl(Decl, Buf);
  assert(!shouldDiscard);
  return Buf;
}

std::string WrapperString(FunctionDecl *Decl)
{
  auto retType = Decl->getReturnType();
  std::string retTypeStr = retType.getAsString();
  std::string funcName = Decl->getNameAsString();
  bool isVoid = retType->isVoidType();

  std::string wrapperStr = "static " + retTypeStr + " _xj_wrap_" + funcName + "(int *_xj_error";
  std::string callArgs;
  uint anonCtr = 0;
  for (auto *param : Decl->parameters())
  {
    std::string paramType = param->getType().getAsString();
    std::string paramName = param->getName().str();
    if (paramName.length() == 0)
    {
      paramName = "_xj_arg_" + std::to_string(anonCtr++);
    }
    wrapperStr += ", " + paramType + " " + paramName;
    if (!callArgs.empty())
      callArgs += ", ";
    callArgs += paramName;
  }
  if (Decl->isVariadic())
  {
    wrapperStr += ", ...";
    callArgs += ", args";
  }
  wrapperStr += ") { ";
  if (Decl->isVariadic())
  {
    wrapperStr += "__builtin_va_list args; ";
    wrapperStr += "__builtin_va_start(args, " + Decl->parameters().back()->getNameAsString() + "); ";
  }
  if (!isVoid)
  {
    wrapperStr += retTypeStr + " ret = ";
  }
  wrapperStr += funcName + "(" + callArgs + "); *_xj_error = errno; ";
  if (!isVoid)
  {
    wrapperStr += "return ret; ";
  }
  wrapperStr += "}\n";
  return wrapperStr;
}

bool LocalizeErrnoASTVisitor::TraverseFunctionDecl(FunctionDecl *Decl)
{
  if (Decl->hasBody())
  {
    assert(StagedChanges.empty());
    assert(!CurrentFunction);
    assert(!CurrentFunctionHasErrnoReference);
    assert(!CurrentFunctionNonDeclCall);
    assert(!CurrentFunctionNeedsDecl);
    CurrentFunction = Decl;
  }
  bool res = RecursiveASTVisitor::TraverseFunctionDecl(Decl);
  if (CurrentFunctionHasErrnoReference && CurrentFunctionNonDeclCall)
  {
    // Can not apply changes here
    llvm::errs() << "Unable to transform function " << CurrentFunction->getNameAsString() << "\n";
  }
  else
  {
    Changes.insert(Changes.begin(), StagedChanges.begin(), StagedChanges.end());
    if (CurrentFunctionNeedsDecl)
    {
      if (auto Body = llvm::dyn_cast<CompoundStmt, Stmt>(Decl->getBody()))
      {
        AtomicChange InsertLocalDecl(Context->getSourceManager(), Decl->getBody()->getBeginLoc(), /* insert_after = */ false);
        auto InsertLoc = Body->body_front()->getBeginLoc();
        auto err = InsertLocalDecl.insert(Context->getSourceManager(), InsertLoc, "int " + LOCAL_ERRNO_NAME + ";\n");
        assert(!err);
        Changes.push_back(InsertLocalDecl);
      }
    }
  }
  StagedChanges.clear();
  CurrentFunctionHasErrnoReference = false;
  CurrentFunctionNonDeclCall = false;
  CurrentFunctionNeedsDecl = false;
  CurrentFunction = nullptr;
  return res;
}

void LocalizeErrnoASTVisitor::ReplaceErrnoUsage(Expr *E)
{
  AtomicChange ReplaceUsage(Context->getSourceManager(), E->getBeginLoc());
  CharSourceRange Range(E->getSourceRange(), true);
  llvm::StringRef Replacement = LOCAL_ERRNO_NAME;
  auto err = ReplaceUsage.replace(Context->getSourceManager(), Range, Replacement);
  assert(!err);
  StagedChanges.push_back(ReplaceUsage);
}

bool LocalizeErrnoASTVisitor::VisitDeclRefExpr(clang::DeclRefExpr *Ref)
{
  if (Ref->getNameInfo().getAsString() == "errno")
  {
    CurrentFunctionHasErrnoReference = true;
    ReplaceErrnoUsage(Ref);
  }
  return true;
}

bool LocalizeErrnoASTVisitor::VisitCallExpr(CallExpr *Call)
{
  assert(CurrentFunction != nullptr);
  FunctionDecl *Callee = llvm::dyn_cast<FunctionDecl, Decl>(Call->getCalleeDecl());
  if (!Callee)
  {
    // Then this could be something like a call through a var or struct field,
    // i.e., a function pointer.
    // We just mark the calling context. If we also find a reference to
    // errno here, then we can not perform the transformation on this function.
    CurrentFunctionNonDeclCall = true;
    return true;
  }

  USRString CalleeUSR = DeclUSR(Callee);

  if (!NeedsWrapper(Callee))
  {
    return true;
  }

  if (External.find(CalleeUSR) == External.end())
  {
    // Not external
    return true;
  }

  CurrentFunctionNeedsDecl = true;

  AtomicChange CallWrapper(Context->getSourceManager(), Call->getBeginLoc());
  CharSourceRange CalleeRange(Call->getCallee()->getSourceRange(), true);
  auto err = CallWrapper.replace(Context->getSourceManager(), CalleeRange, "_xj_wrap_" + Callee->getNameAsString());
  assert(!err);
  if (Callee->getNumParams() == 0)
  {
    err = CallWrapper.insert(Context->getSourceManager(), Call->getRParenLoc(), "&" + LOCAL_ERRNO_NAME, false);
  }
  else
  {
    err = CallWrapper.insert(Context->getSourceManager(), Call->getArg(0)->getBeginLoc(), "&" + LOCAL_ERRNO_NAME + ", ", false);
  }
  assert(!err);
  StagedChanges.push_back(CallWrapper);

  GenerateWrapper(CurrentFunction, Callee);

  return true;
}

void LocalizeErrnoASTVisitor::GenerateWrapper(FunctionDecl *CallContext, FunctionDecl *Callee)
{
  USRString CalleeUSR = DeclUSR(Callee);

  auto ThisInsertLoc = CallContext->getBeginLoc();
  auto PrevLoc = InsertLocations.find(CalleeUSR);
  // Even if we've already generated the wrapper text,
  // This CallContext might be an earlier usage, so we should
  // bump the wrapper insertion point up
  if (PrevLoc == InsertLocations.end() || Context->getSourceManager().isBeforeInTranslationUnit(ThisInsertLoc, PrevLoc->second))
  {
    InsertLocations.insert(std::pair(CalleeUSR, ThisInsertLoc));
  }

  // If we've already generated the wrapper, then we're done
  if (Wrappers.find(CalleeUSR) != Wrappers.end())
  {
    return;
  }
  Wrappers.insert(CalleeUSR);
  auto WrapperText = WrapperString(Callee);
  PendingWrappers.push_back(std::pair(CalleeUSR, WrapperText));
}

void LocalizeErrnoConsumer::HandleTranslationUnit(clang::ASTContext &Context)
{
  Visitor.CurrentFunction = nullptr;
  Visitor.CurrentFunctionNeedsDecl = false;
  Visitor.CurrentFunctionHasErrnoReference = false;
  Visitor.CurrentFunctionNonDeclCall = false;
  Visitor.PendingWrappers.clear();
  Visitor.InsertLocations.clear();
  Visitor.Wrappers.clear();
  Visitor.TraverseDecl(Context.getTranslationUnitDecl());

  std::map<SourceLocation, std::string> ChangesByLoc;
  if (!Visitor.PendingWrappers.empty())
  {
    for (auto &Pending : Visitor.PendingWrappers)
    {
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
      assert(!err);
      Changes.push_back(wrapperChange);
    }
  }
}