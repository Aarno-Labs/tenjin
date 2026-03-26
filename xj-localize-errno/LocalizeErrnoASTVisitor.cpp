#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"
#include "clang/Lex/HeaderSearchOptions.h"

#include "LocalizeErrnoASTVisitor.h"
#include "StdlibSpec.h"

using namespace clang;
using namespace clang::tooling;

// Small AST visitor used only by discoverErrnoSpec to find the function that
// errno dereferences, i.e. the callee in (*callee()) inside a test snippet.
namespace {
class ErrnoDiscoveryVisitor : public RecursiveASTVisitor<ErrnoDiscoveryVisitor> {
public:
  FunctionDecl *ErrnoFunc = nullptr;

  bool VisitUnaryOperator(UnaryOperator *Op) {
    if (Op->getOpcode() == UO_Deref) {
      if (auto *Call = dyn_cast<CallExpr>(Op->getSubExpr())) {
        if (auto *FD = dyn_cast_or_null<FunctionDecl>(Call->getCalleeDecl())) {
          ErrnoFunc = FD;
        }
      }
    }
    return true;
  }
};
} // namespace

ErrnoSpec LocalizeErrnoConsumer::discoverErrnoSpec(CompilerInstance &CI) {
  // Fallback for platforms where discovery fails.
  ErrnoSpec Fallback;
  Fallback.FuncName = "__errno_location";
  Fallback.FuncDeclStr = "int *__errno_location(void);";

  // Build compile args that mirror the current compiler's include paths so
  // that the same <errno.h> is found.
  std::vector<std::string> Args;
  Args.push_back("-x");
  Args.push_back("c");

  const HeaderSearchOptions &HSOpts = CI.getHeaderSearchOpts();
  if (!HSOpts.ResourceDir.empty()) {
    Args.push_back("-resource-dir");
    Args.push_back(HSOpts.ResourceDir);
  }
  for (const auto &Entry : HSOpts.UserEntries) {
    switch (Entry.Group) {
    case frontend::System:
    case frontend::ExternCSystem:
      Args.push_back("-isystem");
      Args.push_back(Entry.Path);
      break;
    case frontend::Angled:
      Args.push_back("-I");
      Args.push_back(Entry.Path);
      break;
    default:
      break;
    }
  }
  if (!HSOpts.Sysroot.empty()) {
    Args.push_back("--sysroot=" + HSOpts.Sysroot);
  }

  // Compile a tiny C file whose only expression is errno.  After preprocessing
  // this becomes (*__errno_location()) on Linux/glibc or (*__error()) on macOS.
  auto TestAST = buildASTFromCodeWithArgs(
      "#include <errno.h>\nvoid f(void) { (void)errno; }", Args, "errno_test.c");
  if (!TestAST)
    return Fallback;

  ASTContext &TestCtx = TestAST->getASTContext();

  // Locate the body of f().
  FunctionDecl *F = nullptr;
  for (auto *D : TestCtx.getTranslationUnitDecl()->decls()) {
    if (auto *FD = dyn_cast<FunctionDecl>(D)) {
      if (FD->getNameAsString() == "f" && FD->hasBody()) {
        F = FD;
        break;
      }
    }
  }
  if (!F)
    return Fallback;

  // Find the (*callee()) pattern inside f's body.
  ErrnoDiscoveryVisitor Visitor;
  Visitor.TraverseDecl(F);
  if (!Visitor.ErrnoFunc)
    return Fallback;

  FunctionDecl *ErrnoFD = Visitor.ErrnoFunc;
  ErrnoSpec Spec;
  Spec.FuncName = ErrnoFD->getNameAsString();
  // Build a minimal forward declaration from the return type.
  Spec.FuncDeclStr =
      ErrnoFD->getReturnType().getAsString() + " " + Spec.FuncName + "(void);";
  return Spec;
}

const std::string LOCAL_ERRNO_NAME = "local_errno";

USRString DeclUSR(FunctionDecl *Decl)
{
  USRString Buf;
  bool shouldDiscard = index::generateUSRForDecl(Decl, Buf);
  assert (!shouldDiscard);
  return Buf;
}

std::string WrapperString(FunctionDecl *Decl, const std::string &ErrnoFuncName)
{
  auto retType = Decl->getReturnType();
  std::string retTypeStr = retType.getAsString();
  std::string funcName = Decl->getNameAsString();
  bool isVoid = retType->isVoidType();

  std::string wrapperStr = "static " + retTypeStr + " _xj_wrap_" + funcName + "(int *_xj_error";
  std::string callArgs;
  uint anonCtr = 0;
  for (auto *param : Decl->parameters()) {
    std::string paramType = param->getType().getAsString();
    std::string paramName = param->getName().str();
    if (paramName.length() == 0)
    {
      paramName = "_xj_arg_" + std::to_string(anonCtr++);
    }
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
    wrapperStr += "__builtin_va_list args; ";
    wrapperStr += "__builtin_va_start(args, " + Decl->parameters().back()->getNameAsString() + "); ";
  }
  if (!isVoid) {
    wrapperStr += retTypeStr + " ret = ";
  }
  wrapperStr += funcName + "(" + callArgs + "); *_xj_error = (*" + ErrnoFuncName + "()); ";
  if (!isVoid) {
    wrapperStr += "return ret; ";
  }
  wrapperStr += "}\n";
  return wrapperStr;
}

bool IsErrnoCall(Expr *E, const std::string &ErrnoFuncName)
{
  if (auto Paren = llvm::dyn_cast<ParenExpr, Expr>(E))
  {
    return IsErrnoCall(Paren->getSubExpr(), ErrnoFuncName);
  }
  else if (auto Op = llvm::dyn_cast<UnaryOperator, Expr>(E))
  {
    if (Op->getOpcode() == UnaryOperatorKind::UO_Deref)
    {
      if (CallExpr *Call = llvm::dyn_cast<CallExpr, Expr>(Op->getSubExpr()))
      {
        if (FunctionDecl *Callee = llvm::dyn_cast<FunctionDecl, Decl>(Call->getCalleeDecl()))
        {
          return Callee->getNameAsString() == ErrnoFuncName;
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
      assert(!err);
      Changes.push_back(InsertLocalDecl);
    }
  }
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
  Changes.push_back(ReplaceUsage);
}

bool LocalizeErrnoASTVisitor::VisitParenExpr(clang::ParenExpr *Paren)
{
  if (IsErrnoCall(Paren, Spec.FuncName))
  {
    ReplaceErrnoUsage(Paren);
    CurrentFunctionNeedsDecl = true;
  }
  return true;
}

bool LocalizeErrnoASTVisitor::VisitCallExpr(CallExpr *Call)
{
  assert(CurrentFunction != nullptr);
  FunctionDecl *Callee = llvm::dyn_cast<FunctionDecl, Decl>(Call->getCalleeDecl());
  assert(Callee != nullptr);

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

  if (Callee->getNameAsString() == Spec.FuncName)
  {
    return true;
  }

  CurrentFunctionNeedsDecl = true;

  AtomicChange CallWrapper(Context->getSourceManager(), Call->getBeginLoc());
  CharSourceRange CalleeRange(Call->getCallee()->getSourceRange(), true);
  auto err = CallWrapper.replace(Context->getSourceManager(), CalleeRange, "_xj_wrap_" + Callee->getNameAsString());
  assert (!err);
  if (Callee->getNumParams() == 0) 
  {
    err = CallWrapper.insert(Context->getSourceManager(), Call->getRParenLoc(), "&" + LOCAL_ERRNO_NAME, false);
  }
  else
  {
    err = CallWrapper.insert(Context->getSourceManager(), Call->getArg(0)->getBeginLoc(), "&" + LOCAL_ERRNO_NAME + ", ", false);
  }
  assert (!err);
  Changes.push_back(CallWrapper);

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
  auto WrapperText = WrapperString(Callee, Spec.FuncName);
  PendingWrappers.push_back(std::pair(CalleeUSR, WrapperText));
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
      assert (!err);
      Changes.push_back(wrapperChange);
    }

    // Do we need a forward declaration for the errno function?
    auto *TUDeclContext = Context.getTranslationUnitDecl();
    DeclarationName ErrnoLocName = &Context.Idents.get(Spec.FuncName);
    auto ErrnoLocResults = TUDeclContext->lookup(ErrnoLocName);
    bool needsErrnoDecl = ErrnoLocResults.empty();
    if (needsErrnoDecl) {
      AtomicChange declareErrno(SM, SM.getLocForStartOfFile(f));
      auto err = declareErrno.insert(SM, SM.getLocForStartOfFile(f), Spec.FuncDeclStr + "\n", false);
      assert (!err);
      Changes.push_back(declareErrno);
    }
  }
}