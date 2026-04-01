#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"

#include "LocalizeErrnoASTVisitor.h"
#include "StdlibSpec.h"

using namespace clang;
using namespace clang::tooling;
using namespace localize;

const std::string LOCAL_ERRNO_NAME = "_xj_local_errno";

USRString DeclUSR(FunctionDecl *Decl)
{
  USRString Buf;
  bool shouldDiscard = index::generateUSRForDecl(Decl, Buf);
  assert(!shouldDiscard);
  return Buf;
}

bool FunctionTransaction::empty()
{
  return !Decl && !NeedsDecl && !HasNonDeclCall && !HasErrnoReference && Changes.empty() && NeededWrappers.empty();
}

void FunctionTransaction::clear()
{
  Decl = nullptr;
  NeedsDecl = false;
  HasNonDeclCall = false;
  HasErrnoReference = false;
  Changes.clear();
  NeededWrappers.clear();
}

void FunctionTransaction::begin(FunctionDecl *F)
{
  Decl = F;
}

bool FunctionTransaction::ShouldApply(TransformPolicy Policy)
{
  return CanApply() && (Policy == TransformPolicy::Unconditional || (Policy == TransformPolicy::OnDemand && HasErrnoReference));
}

bool FunctionTransaction::CanApply()
{
  return !(HasNonDeclCall && HasErrnoReference);
}

void FunctionTransaction::CommitChanges(ASTContext *Context,
                                        clang::tooling::AtomicChanges &DestChanges,
                                        std::set<USRString> &DestWrappedFunctions,
                                        std::map<USRString, std::string> &DestWrappers,
                                        std::map<USRString, clang::SourceLocation> &DestInsertLocations)
{
    DestChanges.insert(DestChanges.end(), Changes.begin(), Changes.end());

    for (auto Callee : NeededWrappers)
    {
      GenerateWrapper(Context, Callee, DestWrappedFunctions, DestWrappers, DestInsertLocations);
    }

    if (auto Body = llvm::dyn_cast<CompoundStmt, Stmt>(Decl->getBody()))
    {
      AtomicChange InsertLocalDecl(Context->getSourceManager(), Decl->getBody()->getBeginLoc(), /* insert_after = */ false);
      auto InsertLoc = Body->body_front()->getBeginLoc();
      auto err = InsertLocalDecl.insert(Context->getSourceManager(), InsertLoc, "int " + LOCAL_ERRNO_NAME + ";\n");
      assert(!err);
      DestChanges.push_back(InsertLocalDecl);
    }
}

std::string WrapperString(FunctionDecl *Decl)
{
  auto retType = Decl->getReturnType();
  std::string retTypeStr = retType.getAsString();
  std::string funcName = Decl->getNameAsString();
  bool isVoid = retType->isVoidType();

  std::string wrapperStr = "static " + retTypeStr + " _xj_wrap_" + funcName + "(int *_xj_errno";
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
  wrapperStr += funcName + "(" + callArgs + "); *_xj_errno = errno; ";
  if (!isVoid)
  {
    wrapperStr += "return ret; ";
  }
  wrapperStr += "}\n";
  return wrapperStr;
}

void LocalizeErrnoASTVisitor::EnterFunction(FunctionDecl *F)
{
  assert(CurrentFunctionTxn.empty());
  CurrentFunctionTxn.Decl = F;
}

void LocalizeErrnoASTVisitor::ExitFunction(FunctionDecl *F)
{
  assert(CurrentFunctionTxn.Decl == F);
  CurrentFunctionTxn.clear();
}


bool LocalizeErrnoASTVisitor::TraverseFunctionDecl(FunctionDecl *Decl)
{
  EnterFunction(Decl);
  bool res = RecursiveASTVisitor::TraverseFunctionDecl(Decl);
  if (!CurrentFunctionTxn.CanApply())
  {
    // Can not apply changes here
    llvm::errs() << "Unable to transform function " << Decl->getNameAsString() << "\n";
  }
  else if (CurrentFunctionTxn.ShouldApply(Policy))
  {
    llvm::outs() << "Needs Decl? " << CurrentFunctionTxn.NeedsDecl << "\n";
    CurrentFunctionTxn.CommitChanges(Context, Changes, Wrappers, PendingWrappers, InsertLocations);
  }
  else 
  {
    llvm::errs() << "Not applying any changes for " << Decl->getNameAsString() << "\n";
  }
  ExitFunction(Decl);
  return res;
}

void LocalizeErrnoASTVisitor::ReplaceErrnoUsage(Expr *E)
{
  AtomicChange ReplaceUsage(Context->getSourceManager(), E->getBeginLoc());
  CharSourceRange Range(E->getSourceRange(), true);
  llvm::StringRef Replacement = LOCAL_ERRNO_NAME;
  auto err = ReplaceUsage.replace(Context->getSourceManager(), Range, Replacement);
  assert(!err);
  CurrentFunctionTxn.Changes.push_back(ReplaceUsage);
}

bool LocalizeErrnoASTVisitor::VisitDeclRefExpr(clang::DeclRefExpr *Ref)
{
  if (Ref->getNameInfo().getAsString() == "errno")
  {
    CurrentFunctionTxn.HasErrnoReference = true;
    ReplaceErrnoUsage(Ref);
  }
  return true;
}

bool LocalizeErrnoASTVisitor::VisitCallExpr(CallExpr *Call)
{
  assert(CurrentFunctionTxn.Decl != nullptr);
  FunctionDecl *Callee = llvm::dyn_cast<FunctionDecl, Decl>(Call->getCalleeDecl());
  if (!Callee)
  {
    // Then this could be something like a call through a var or struct field,
    // i.e., a function pointer.
    // We just mark the calling context. If we also find a reference to
    // errno here, then we can not perform the transformation on this function.
    CurrentFunctionTxn.HasNonDeclCall = true;
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
  CurrentFunctionTxn.Changes.push_back(CallWrapper);
  CurrentFunctionTxn.NeededWrappers.insert(Callee);

  return true;
}

void FunctionTransaction::GenerateWrapper(ASTContext *Context,
                                          FunctionDecl *Callee, 
                                          std::set<USRString> &Wrappers,
                                          std::map<USRString, std::string> &PendingWrappers,
                                          std::map<USRString, SourceLocation> &InsertLocations)
{
  FunctionDecl *CallContext = Decl;
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
  assert(PendingWrappers.find(CalleeUSR) == PendingWrappers.end());
  PendingWrappers.insert(std::pair(CalleeUSR, WrapperText));
}

void LocalizeErrnoConsumer::HandleTranslationUnit(clang::ASTContext &Context)
{
  Visitor.CurrentFunctionTxn.clear();
  Visitor.PendingWrappers.clear();
  Visitor.InsertLocations.clear();
  Visitor.Wrappers.clear();
  Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  assert (Visitor.CurrentFunctionTxn.empty());

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