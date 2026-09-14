#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Signals.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace clang;
using namespace clang::tooling;

namespace {

llvm::cl::OptionCategory AtomicsCategory("xj-prepare-atomics options");
llvm::cl::opt<bool> Inplace("inplace",
                            llvm::cl::desc("Overwrite source files in place"),
                            llvm::cl::init(false),
                            llvm::cl::cat(AtomicsCategory));

enum class EditKind {
  Load,
  Store,
  FetchAdd,
  FetchSub,
  FetchAnd,
  FetchOr,
  FetchXor
};

struct AccessEdit {
  EditKind Kind;
  const Expr *WholeExpr;
  const Expr *Rhs;
  const VarDecl *Variable;
  unsigned Begin;
  unsigned End;
};

struct Candidate {
  const VarDecl *Canonical = nullptr;
  std::vector<const VarDecl *> Declarations;
  std::vector<AccessEdit> Edits;
  std::string AtomicTypeName;
  bool Eligible = true;
  std::string RejectionReason;
};

class AtomicCollector {
public:
  explicit AtomicCollector(ASTContext &Context)
      : Ctx(Context), SM(Context.getSourceManager()),
        LangOpts(Context.getLangOpts()) {}

  void collectAndRewrite(Rewriter &R) {
    DeclarationVisitor DeclVisitor(*this);
    DeclVisitor.TraverseDecl(Ctx.getTranslationUnitDecl());
    finishDeclarationChecks();

    AccessVisitor RefVisitor(*this);
    RefVisitor.TraverseDecl(Ctx.getTranslationUnitDecl());
    rewrite(R);
  }

  unsigned promotedCount() const { return PromotedCount; }

private:
  class DeclarationVisitor : public RecursiveASTVisitor<DeclarationVisitor> {
  public:
    explicit DeclarationVisitor(AtomicCollector &Owner) : Owner(Owner) {}
    bool VisitVarDecl(VarDecl *VD) {
      Owner.considerDeclaration(VD);
      return true;
    }

  private:
    AtomicCollector &Owner;
  };

  class AccessVisitor : public RecursiveASTVisitor<AccessVisitor> {
  public:
    explicit AccessVisitor(AtomicCollector &Owner) : Owner(Owner) {}
    bool VisitDeclRefExpr(DeclRefExpr *DRE) {
      Owner.considerAccess(DRE);
      return true;
    }

  private:
    AtomicCollector &Owner;
  };

  ASTContext &Ctx;
  SourceManager &SM;
  const LangOptions &LangOpts;
  llvm::DenseMap<const VarDecl *, Candidate> Candidates;
  llvm::DenseSet<const Stmt *> RecordedOperations;
  unsigned PromotedCount = 0;

  static const VarDecl *canonical(const VarDecl *VD) {
    return VD->getCanonicalDecl();
  }

  bool isMainFileLocation(SourceLocation Loc) const {
    return Loc.isValid() && !Loc.isMacroID() && SM.isWrittenInMainFile(Loc);
  }

  std::optional<std::pair<unsigned, unsigned>>
  offsetsFor(SourceRange Range) const {
    CharSourceRange FileRange = Lexer::makeFileCharRange(
        CharSourceRange::getTokenRange(Range), SM, LangOpts);
    if (FileRange.isInvalid() || !isMainFileLocation(FileRange.getBegin()) ||
        !isMainFileLocation(FileRange.getEnd()))
      return std::nullopt;
    return std::make_pair(SM.getFileOffset(FileRange.getBegin()),
                          SM.getFileOffset(FileRange.getEnd()));
  }

  bool hasEligibleType(const VarDecl *VD) const {
    QualType T = VD->getType();
    bool IsVolatileSigAtomic =
        T.isVolatileQualified() &&
        T.getUnqualifiedType().getAsString() == "sig_atomic_t";
    if (T.isNull() || T->isAtomicType() || T.isConstQualified() ||
        (T.isVolatileQualified() && !IsVolatileSigAtomic) ||
        T.isRestrictQualified() || T->isArrayType() || T->isPointerType() ||
        T->isEnumeralType() || T->isBitIntType())
      return false;

    QualType Canonical = T.getCanonicalType();
    if (!Canonical->isIntegerType())
      return false;
    uint64_t Bits = Ctx.getTypeSize(Canonical);
    return Bits == 8 || Bits == 16 || Bits == 32 || Bits == 64;
  }

  std::string atomicTypeName(const VarDecl *VD) const {
    QualType T = VD->getType();
    std::string Spelling = T.getUnqualifiedType().getAsString();
    if (Spelling == "size_t")
      return "__tenjin_atomic_usize_t";
    if (Spelling == "ssize_t")
      return "__tenjin_atomic_isize_t";
    if (T->isBooleanType())
      return "__tenjin_atomic_bool_t";

    llvm::StringRef Sign;
    if (T->isSignedIntegerType())
      Sign = "i";
    else if (T->isUnsignedIntegerType())
      Sign = "u";
    else
      return {};
    return "__tenjin_atomic_" + Sign.str() +
           std::to_string(Ctx.getTypeSize(T.getCanonicalType())) + "_t";
  }

  void reject(Candidate &C, llvm::StringRef Reason) {
    if (C.Eligible) {
      C.Eligible = false;
      C.RejectionReason = Reason.str();
    }
  }

  void considerDeclaration(VarDecl *VD) {
    if (!VD->isFileVarDecl() || VD->getFormalLinkage() != Linkage::Internal)
      return;

    const VarDecl *Canon = canonical(VD);
    Candidate &C = Candidates[Canon];
    if (!C.Canonical)
      C.Canonical = Canon;
    C.Declarations.push_back(VD);

    if (!hasEligibleType(VD))
      reject(C, "unsupported type");
    std::string AtomicType = atomicTypeName(VD);
    if (AtomicType.empty())
      reject(C, "integer type has no signed or unsigned atomic representation");
    else if (C.AtomicTypeName.empty())
      C.AtomicTypeName = std::move(AtomicType);
    else if (C.AtomicTypeName != AtomicType)
      reject(C, "redeclarations use incompatible atomic type spellings");
    if (VD->getTLSKind() != VarDecl::TLS_None)
      reject(C, "thread-local variable");
    if (!isMainFileLocation(VD->getBeginLoc()) ||
        !isMainFileLocation(VD->getLocation()))
      reject(C, "declaration is not directly spelled in the main file");
    if (!VD->getTypeSourceInfo())
      reject(C, "declaration has no rewritable type spelling");
  }

  void finishDeclarationChecks() {
    // A single type-specifier can declare several variables. The production
    // pipeline splits these first; rejecting them here keeps the standalone
    // tool from accidentally promoting an ineligible sibling declarator.
    std::map<unsigned, std::vector<Candidate *>> ByDeclarationStart;
    for (auto &Entry : Candidates) {
      Candidate &C = Entry.second;
      for (const VarDecl *VD : C.Declarations) {
        if (isMainFileLocation(VD->getBeginLoc()))
          ByDeclarationStart[SM.getFileOffset(VD->getBeginLoc())].push_back(&C);
      }
    }
    for (auto &Entry : ByDeclarationStart) {
      std::set<Candidate *> Distinct(Entry.second.begin(), Entry.second.end());
      if (Distinct.size() > 1) {
        for (Candidate *C : Distinct)
          reject(*C, "joined declaration");
      }
    }

    for (auto &Entry : Candidates) {
      Candidate &C = Entry.second;
      for (const VarDecl *VD : C.Declarations) {
        if (!C.Eligible)
          break;
        TypeSourceInfo *TSI = VD->getTypeSourceInfo();
        if (!TSI || !offsetsFor(TSI->getTypeLoc().getSourceRange()))
          reject(C, "type spelling cannot be rewritten");
      }
    }
  }

  const Stmt *transparentParent(const Stmt *S) const {
    const Stmt *Current = S;
    while (true) {
      auto Parents = Ctx.getParents(*Current);
      if (Parents.size() != 1)
        return nullptr;
      const Stmt *Parent = Parents[0].get<Stmt>();
      if (!Parent)
        return nullptr;
      if (isa<ParenExpr>(Parent)) {
        Current = Parent;
        continue;
      }
      return Parent;
    }
  }

  bool resultIsDiscarded(const Expr *E) const {
    const Stmt *Current = E;
    while (true) {
      auto Parents = Ctx.getParents(*Current);
      if (Parents.size() != 1)
        return false;
      if (const auto *PE = Parents[0].get<ParenExpr>()) {
        Current = PE;
        continue;
      }
      const Stmt *Parent = Parents[0].get<Stmt>();
      if (isa_and_nonnull<CompoundStmt>(Parent))
        return true;
      if (const auto *For = dyn_cast_or_null<ForStmt>(Parent))
        return For->getInit() == Current || For->getInc() == Current;
      return false;
    }
  }

  void addEdit(Candidate &C, EditKind Kind, const Expr *WholeExpr,
               const Expr *Rhs, const VarDecl *VD) {
    if (!RecordedOperations.insert(WholeExpr).second)
      return;
    auto Offsets = offsetsFor(WholeExpr->getSourceRange());
    if (!Offsets) {
      reject(C, "access cannot be rewritten");
      return;
    }
    C.Edits.push_back(
        {Kind, WholeExpr, Rhs, VD, Offsets->first, Offsets->second});
  }

  void considerAccess(DeclRefExpr *DRE) {
    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (!VD)
      return;
    auto It = Candidates.find(canonical(VD));
    if (It == Candidates.end())
      return;
    Candidate &C = It->second;
    if (!isMainFileLocation(DRE->getLocation())) {
      reject(C, "access is not directly spelled in the main file");
      return;
    }

    const Stmt *Parent = transparentParent(DRE);
    if (!Parent) {
      reject(C, "unclassifiable access");
      return;
    }

    if (const auto *Cast = dyn_cast<ImplicitCastExpr>(Parent)) {
      if (Cast->getCastKind() == CK_LValueToRValue) {
        addEdit(C, EditKind::Load, DRE, nullptr, VD);
        return;
      }
      reject(C, "unsupported implicit use");
      return;
    }

    if (const auto *BO = dyn_cast<BinaryOperator>(Parent)) {
      if (!BO->isAssignmentOp() || BO->getLHS()->IgnoreParenImpCasts() != DRE) {
        reject(C, "unsupported lvalue use");
        return;
      }
      if (!resultIsDiscarded(BO)) {
        reject(C, "assignment result is used");
        return;
      }

      EditKind Kind;
      switch (BO->getOpcode()) {
      case BO_Assign:
        Kind = EditKind::Store;
        break;
      case BO_AddAssign:
        Kind = EditKind::FetchAdd;
        break;
      case BO_SubAssign:
        Kind = EditKind::FetchSub;
        break;
      case BO_AndAssign:
        Kind = EditKind::FetchAnd;
        break;
      case BO_OrAssign:
        Kind = EditKind::FetchOr;
        break;
      case BO_XorAssign:
        Kind = EditKind::FetchXor;
        break;
      default:
        reject(C, "compound assignment has no C11 atomic primitive");
        return;
      }
      if (VD->getType()->isBooleanType() && Kind != EditKind::Store) {
        reject(C, "boolean read-modify-write");
        return;
      }
      addEdit(C, Kind, BO, BO->getRHS(), VD);
      return;
    }

    if (const auto *UO = dyn_cast<UnaryOperator>(Parent)) {
      if ((UO->isIncrementDecrementOp()) &&
          UO->getSubExpr()->IgnoreParenImpCasts() == DRE) {
        if (VD->getType()->isBooleanType()) {
          reject(C, "boolean increment or decrement");
          return;
        }
        bool IsPostfix = UO->isPostfix();
        if (!IsPostfix && !resultIsDiscarded(UO)) {
          reject(C, "prefix increment or decrement result is used");
          return;
        }
        addEdit(C,
                UO->isIncrementOp() ? EditKind::FetchAdd : EditKind::FetchSub,
                UO, nullptr, VD);
        return;
      }
      reject(C, UO->getOpcode() == UO_AddrOf ? "address is taken"
                                             : "unsupported unary use");
      return;
    }

    reject(C,
           "use is not an atomic load, store, or supported read-modify-write");
  }

  std::string sourceSlice(unsigned Begin, unsigned End) const {
    llvm::StringRef Buffer = SM.getBufferData(SM.getMainFileID());
    return Buffer.slice(Begin, End).str();
  }

  std::vector<const AccessEdit *> eligibleEditsWithin(unsigned Begin,
                                                      unsigned End) const {
    std::vector<const AccessEdit *> Result;
    for (const auto &Entry : Candidates) {
      const Candidate &C = Entry.second;
      if (!C.Eligible)
        continue;
      for (const AccessEdit &Edit : C.Edits) {
        if (Edit.Begin >= Begin && Edit.End <= End)
          Result.push_back(&Edit);
      }
    }
    std::sort(Result.begin(), Result.end(),
              [](const AccessEdit *A, const AccessEdit *B) {
                if (A->Begin != B->Begin)
                  return A->Begin < B->Begin;
                return A->End > B->End;
              });
    return Result;
  }

  std::string renderRange(unsigned Begin, unsigned End) const {
    std::vector<const AccessEdit *> Contained = eligibleEditsWithin(Begin, End);
    std::vector<const AccessEdit *> Outermost;
    unsigned CoveredUntil = Begin;
    for (const AccessEdit *Edit : Contained) {
      if (Edit->Begin >= CoveredUntil) {
        Outermost.push_back(Edit);
        CoveredUntil = Edit->End;
      }
    }

    std::string Result;
    unsigned Cursor = Begin;
    for (const AccessEdit *Edit : Outermost) {
      Result += sourceSlice(Cursor, Edit->Begin);
      Result += renderEdit(*Edit);
      Cursor = Edit->End;
    }
    Result += sourceSlice(Cursor, End);
    return Result;
  }

  std::string renderExpr(const Expr *E) const {
    auto Range = offsetsFor(E->getSourceRange());
    return Range ? renderRange(Range->first, Range->second) : std::string();
  }

  static llvm::StringRef primitiveName(EditKind Kind) {
    switch (Kind) {
    case EditKind::Load:
      return "__c11_atomic_load";
    case EditKind::Store:
      return "__c11_atomic_store";
    case EditKind::FetchAdd:
      return "__c11_atomic_fetch_add";
    case EditKind::FetchSub:
      return "__c11_atomic_fetch_sub";
    case EditKind::FetchAnd:
      return "__c11_atomic_fetch_and";
    case EditKind::FetchOr:
      return "__c11_atomic_fetch_or";
    case EditKind::FetchXor:
      return "__c11_atomic_fetch_xor";
    }
    llvm_unreachable("unknown atomic edit kind");
  }

  std::string renderEdit(const AccessEdit &Edit) const {
    // The pipeline operates on already-preprocessed .i files, so a newly
    // inserted <stdatomic.h> include cannot be resolved by later tools.
    // Clang's __c11 builtins are the header-free lowering of C11's generic
    // atomic operations, and c2rust handles these forms directly.
    std::string Result = primitiveName(Edit.Kind).str() + "(&" +
                         Edit.Variable->getNameAsString();
    if (Edit.Kind == EditKind::Load)
      return Result + ", __ATOMIC_SEQ_CST)";
    if (Edit.Kind == EditKind::Store)
      return Result + ", " + renderExpr(Edit.Rhs) + ", __ATOMIC_SEQ_CST)";
    if (Edit.Rhs)
      return Result + ", " + renderExpr(Edit.Rhs) + ", __ATOMIC_SEQ_CST)";
    return Result + ", 1, __ATOMIC_SEQ_CST)";
  }

  void rewrite(Rewriter &R) {
    std::vector<const AccessEdit *> AllEdits =
        eligibleEditsWithin(0, UINT32_MAX);
    std::vector<const AccessEdit *> Outermost;
    unsigned CoveredUntil = 0;
    for (const AccessEdit *Edit : AllEdits) {
      if (Edit->Begin >= CoveredUntil) {
        Outermost.push_back(Edit);
        CoveredUntil = Edit->End;
      }
    }
    std::sort(Outermost.begin(), Outermost.end(),
              [](const AccessEdit *A, const AccessEdit *B) {
                return A->Begin > B->Begin;
              });
    for (const AccessEdit *Edit : Outermost) {
      SourceLocation Begin = SM.getLocForStartOfFile(SM.getMainFileID())
                                 .getLocWithOffset(Edit->Begin);
      R.ReplaceText(Begin, Edit->End - Edit->Begin, renderEdit(*Edit));
    }

    std::set<std::pair<unsigned, unsigned>> RewrittenTypes;
    struct TypedefInsertion {
      unsigned Offset;
      std::string OriginalType;
    };
    std::map<std::string, TypedefInsertion> Typedefs;
    for (auto &Entry : Candidates) {
      Candidate &C = Entry.second;
      if (!C.Eligible)
        continue;
      ++PromotedCount;
      for (const VarDecl *VD : C.Declarations) {
        SourceRange TypeRange =
            VD->getTypeSourceInfo()->getTypeLoc().getSourceRange();
        auto Offsets = offsetsFor(TypeRange);
        if (!Offsets || !RewrittenTypes.insert(*Offsets).second)
          continue;
        std::string TypeText = VD->getType().getUnqualifiedType().getAsString();
        unsigned DeclarationOffset = SM.getFileOffset(VD->getBeginLoc());
        auto [It, Inserted] = Typedefs.emplace(
            C.AtomicTypeName, TypedefInsertion{DeclarationOffset, TypeText});
        if (!Inserted && DeclarationOffset < It->second.Offset)
          It->second = {DeclarationOffset, TypeText};
        SourceLocation Begin = SM.getLocForStartOfFile(SM.getMainFileID())
                                   .getLocWithOffset(Offsets->first);
        R.ReplaceText(Begin, Offsets->second - Offsets->first,
                      C.AtomicTypeName);
      }
    }

    for (const auto &[Name, Insertion] : Typedefs) {
      SourceLocation Loc = SM.getLocForStartOfFile(SM.getMainFileID())
                               .getLocWithOffset(Insertion.Offset);
      R.InsertText(Loc,
                   "typedef _Atomic(" + Insertion.OriginalType + ") " + Name +
                       ";\n",
                   true, true);
    }
  }
};

class AtomicConsumer : public ASTConsumer {
public:
  AtomicConsumer(ASTContext &Ctx, Rewriter &R, unsigned &Promoted)
      : Ctx(Ctx), R(R), Promoted(Promoted) {}

  void HandleTranslationUnit(ASTContext &) override {
    AtomicCollector Collector(Ctx);
    Collector.collectAndRewrite(R);
    Promoted = Collector.promotedCount();
  }

private:
  ASTContext &Ctx;
  Rewriter &R;
  unsigned &Promoted;
};

class AtomicAction : public ASTFrontendAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef) override {
    R.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<AtomicConsumer>(CI.getASTContext(), R, Promoted);
  }

  void EndSourceFileAction() override {
    SourceManager &SM = R.getSourceMgr();
    if (auto File = SM.getFileEntryRefForID(SM.getMainFileID()))
      llvm::errs() << "[SUMMARY] " << File->getName() << ": promoted "
                   << Promoted << " global(s) to C11 atomics\n";
    if (Inplace)
      R.overwriteChangedFiles();
    else
      R.getEditBuffer(SM.getMainFileID()).write(llvm::outs());
  }

private:
  Rewriter R;
  unsigned Promoted = 0;
};

} // namespace

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  auto Parser = CommonOptionsParser::create(argc, argv, AtomicsCategory);
  if (!Parser) {
    llvm::errs() << Parser.takeError();
    return 1;
  }
  ClangTool Tool(Parser->getCompilations(), Parser->getSourcePathList());
  return Tool.run(newFrontendActionFactory<AtomicAction>().get());
}
