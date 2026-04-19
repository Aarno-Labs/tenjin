#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"

#include "LocalizeErrnoASTVisitor.h"
#include "StdlibSpec.h"

using namespace clang;
using namespace clang::tooling;
using namespace localize;

const std::string LOCAL_ERRNO_NAME = "_xj_local_errno";

const std::string READ_ERRNO_TEXT_LINUX = "(*__errno_location())";
const std::string READ_ERRNO_TEXT_MACOS = "(*__error())";

USRString DeclUSR(FunctionDecl *Decl)
{
  USRString Buf;
  bool shouldDiscard = index::generateUSRForDecl(Decl, Buf);
  assert(!shouldDiscard);
  return Buf;
}


bool DerefOfCallExtern(Expr *E, std::string Name)
{
  if (auto *Op = llvm::dyn_cast<UnaryOperator>(E))
  {
    if (Op->getOpcode() == UnaryOperatorKind::UO_Deref)
    {
      if (auto *Call = llvm::dyn_cast<CallExpr>(Op->getSubExpr()))
      {
        if (auto *CalleeDecl = Call->getDirectCallee())
        {
          return CalleeDecl->getFormalLinkage() == Linkage::External && CalleeDecl->getNameAsString() == Name;
        }
      }
    }
  }

  return false;
}

bool IsErrno_Linux(Expr *E)
{
  return DerefOfCallExtern(E, "__errno_location");
}

bool IsErrno_MacOS(Expr *E)
{
  return DerefOfCallExtern(E, "__error");
}

#if defined(__gnu_linux__)
#define IsErrno IsErrno_Linux
#define READ_ERRNO_TEXT READ_ERRNO_TEXT_LINUX
#elif defined(__APPLE__) && defined(__MACH__)
#define IsErrno IsErrno_MacOS
#define READ_ERRNO_TEXT READ_ERRNO_TEXT_MACOS
#else
#error "Unknown OS errno spec"
#endif

bool FunctionTransaction::Empty()
{
  return !Decl && !HasNonDeclCall && !HasErrnoAccess && Changes.empty() && NeededWrappers.empty();
}

void FunctionTransaction::Clear()
{
  Decl = nullptr;
  HasNonDeclCall = false;
  HasErrnoAccess = false;
  Changes.clear();
  NeededWrappers.clear();
}

void FunctionTransaction::Begin(FunctionDecl *F)
{
  assert(Empty());
  Decl = F;
}

void FunctionTransaction::End(FunctionDecl *F)
{
  assert(Decl == F);
  Clear();
}

bool FunctionTransaction::ShouldApply(TransformPolicy Policy)
{
  return Policy == TransformPolicy::Unconditional || (Policy == TransformPolicy::OnDemand && HasErrnoAccess);
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
    else
    {
      Decl->getBody()->dump();
      assert(0);
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
  wrapperStr += funcName + "(" + callArgs + "); *_xj_errno = " + READ_ERRNO_TEXT + ";";
  if (!isVoid)
  {
    wrapperStr += "return ret; ";
  }
  wrapperStr += "}\n";
  return wrapperStr;
}

bool LocalizeErrnoASTVisitor::TraverseFunctionDecl(FunctionDecl *Decl)
{
  if (!Decl->hasBody()) {
    return true;
  }

  CurrentFunctionTxn.Begin(Decl);
  bool res = RecursiveASTVisitor::TraverseFunctionDecl(Decl);
  if (CurrentFunctionTxn.ShouldApply(Policy))
  {
    CurrentFunctionTxn.CommitChanges(Context, Changes, Wrappers, WrapperDefinitions, InsertLocations);
  }
  CurrentFunctionTxn.End(Decl);
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

bool LocalizeErrnoASTVisitor::VisitExpr(clang::Expr *Expr)
{
  if (IsErrno(Expr))
  {
    CurrentFunctionTxn.HasErrnoAccess = true;
    ReplaceErrnoUsage(Expr);
  }
  return true;
}

bool LocalizeErrnoASTVisitor::VisitCallExpr(CallExpr *Call)
{
  assert(CurrentFunctionTxn.Decl != nullptr);
  FunctionDecl *Callee = llvm::dyn_cast<FunctionDecl, Decl>(Call->getCalleeDecl());
  if (!Callee)
  {
    // This is OK, then we don't need to wrap this function
    // Why? Because the codehawk-based analysis does not handle anything but direct calls
    // to library functions
    return true;
  }

  USRString CalleeUSR = DeclUSR(Callee);

  if (External.find(CalleeUSR) == External.end() || !NeedsWrapper(Callee))
  {
    // Nothing to do: either not external or our spec says it does not need to be wrapped
    return true;
  }

  // Build and stage the change to the call expression:
  // replace foo(...) with xj_wrap_foo(&local_errno, ...)
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
  if (err)
  {
    llvm::errs() << "Error inserting call to wrapper: " << err << "\n";
    assert(false);
  }
  
  // Stages the change in the current transaction
  CurrentFunctionTxn.Changes.push_back(CallWrapper);
  CurrentFunctionTxn.NeededWrappers.insert(Callee);

  return true;
}

void FunctionTransaction::GenerateWrapper(ASTContext *Context,
                                          FunctionDecl *Callee, 
                                          std::set<USRString> &Wrappers,
                                          std::map<USRString, std::string> &WrapperDefinitions,
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
  assert(WrapperDefinitions.find(CalleeUSR) == WrapperDefinitions.end());
  WrapperDefinitions.insert(std::pair(CalleeUSR, WrapperText));
}

void LocalizeErrnoConsumer::HandleTranslationUnit(clang::ASTContext &Context)
{
  Visitor.WrapperDefinitions.clear();
  Visitor.InsertLocations.clear();
  Visitor.Wrappers.clear();
  Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  assert (Visitor.CurrentFunctionTxn.Empty());

  std::map<SourceLocation, std::string> ChangesByLoc;
  if (!Visitor.WrapperDefinitions.empty())
  {
    for (auto &Pending : Visitor.WrapperDefinitions)
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